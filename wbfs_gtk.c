/* wbfs_gtk.c
 *
 * Copyright (C) 2009 Ricardo Massaro
 *
 * Licensed under the terms of the GNU GPL, version 2
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt
 */

#include <stdlib.h>
#include <stdio.h>

#include <unistd.h>
#include <pwd.h>

#include <gtk/gtk.h>
#include <glade/glade.h>

#include "config.h"
#include "wbfs_gtk.h"
#include "wbfs_ops.h"
#include "app_state.h"
#include "list_dir.h"
#include "message.h"
#include "progress.h"
#include "devices.h"

#include "libwbfs.h"

#include "wbfs_gui_glade.h"

#define BYTES_TO_GB(n) ((n) / 1024. / 1024. / 1024.)

static GladeXML *glade_xml;
static char cur_directory[PATH_MAX];
static DIR_ITEM cur_dir_list[1024];

static void update_fs_list(void);
static void update_iso_list(void);
static int load_device(void);

GtkWidget *get_widget(const char *name)
{
  return glade_xml_get_widget(glade_xml, name);
}

static int get_device_id(const char *device)
{
  int i;

  for (i = 0; i < app_state.num_devs; i++)
    if (strcmp(app_state.dev[i], device) == 0)
      return i;
  return -1;
}

/**
 * Get the device selected in the interface.
 * The returned string must be freed with g_free().
 */
static int get_selected_device(char **device)
{
  GtkListStore *store;
  GtkTreeIter iter;
  GtkWidget *widget;
  GtkComboBox *dev_list;

  widget = get_widget("device_list");
  dev_list = GTK_COMBO_BOX(widget);
  store = GTK_LIST_STORE(gtk_combo_box_get_model(dev_list));
  if (gtk_combo_box_get_active_iter(dev_list, &iter)) {
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, device, -1);
    return 1;
  }

  return 0;
}

/**
 * Get the file selected in the interface.
 * The returned string must be freed with g_free().
 */
static int get_selected_file(int *mode, char **name)
{
  GtkWidget *widget;
  GtkTreeView *fs_list;
  GtkTreeSelection *sel;
  GtkTreeModel *model;
  GtkTreeIter iter;

  widget = get_widget("fs_list");
  fs_list = GTK_TREE_VIEW(widget);
  sel = gtk_tree_view_get_selection(fs_list);
  if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
    if (name != NULL)
      gtk_tree_model_get(model, &iter, 0, mode, 1, name, -1);
    return 1;
  }
  return 0;
}

/**
 * Get the disc selected in the interface.
 * The returned string must be freed with g_free().
 */
static int get_selected_disc(char **code, char **name)
{
  GtkWidget *widget;
  GtkTreeView *iso_list;
  GtkTreeSelection *sel;
  GtkTreeModel *model;
  GtkTreeIter iter;

  if (app_state.wbfs == NULL)
    return 0;

  widget = get_widget("iso_list");
  iso_list = GTK_TREE_VIEW(widget);
  sel = gtk_tree_view_get_selection(iso_list);
  if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
    if (code != NULL && name != NULL)
      gtk_tree_model_get(model, &iter, 0, code, 1, name, -1);
    return 1;
  }
  return 0;
}

static void set_label_double(const char *widget_name, const char *format, double n)
{
  GtkWidget *widget;
  char text[256];

  widget = get_widget(widget_name);
  if (format == NULL)
    *text = '\0';
  else
    snprintf(text, sizeof(text), format, n);
  gtk_label_set_markup(GTK_LABEL(widget), text);
}

static void convert_discname_to_filename(char *filename, const char *discname, int max_len)
{
  int i, c;

  for (i = 0; i+1 < max_len && discname[i] != '\0'; i++) {
    c = discname[i];
    if ((c >= 'A' && c <= 'Z')
	|| (c >= 'a' && c <= 'z')
	|| (c >= '0' && c <= '9')
	|| c == '-' || c == '.')
      filename[i] = c;
    else
      filename[i] = '_';
  }
  filename[i] = '\0';
}

/* starter for "extract iso" operation */
static int iso_extract_start(void *p, progress_updater update)
{
  char **data = (char **) p;

  return op_extract_iso(data[0], data[1], update);
}

/* updater for "extract iso" operation */
static void iso_extract_update(int cur, int max)
{
  GtkWidget *widget;
  GtkProgressBar *progress_bar;
  double fraction;
  char txt[32];

  widget = get_widget("progress_bar");
  progress_bar = GTK_PROGRESS_BAR(widget);

  fraction = (double) cur / (double) max;
  snprintf(txt, sizeof(txt), "%d%%", (int) (fraction * 100));

  gtk_progress_bar_set_fraction(progress_bar, fraction);
  gtk_progress_bar_set_text(progress_bar, txt);
}

/* starter for "add iso" operation */
static int iso_add_start(void *p, progress_updater update)
{
  char *filename = (char *) p;

  return op_add_iso(filename, update);
}

/* Updater for "add iso" operation */
static void iso_add_update(int cur, int max)
{
  GtkWidget *widget;
  GtkProgressBar *progress_bar;
  double fraction;
  char txt[32];

  widget = get_widget("progress_bar");
  progress_bar = GTK_PROGRESS_BAR(widget);

  fraction = (double) cur / (double) max;
  snprintf(txt, sizeof(txt), "%d%%", (int) (fraction * 100));

  gtk_progress_bar_set_fraction(progress_bar, fraction);
  gtk_progress_bar_set_text(progress_bar, txt);
}

/**
 * Ask confirmation and add an ISO file to the WBFS partition.
 */
static void confirm_add_iso_file(char *filename)
{
  wbfs_disc_t *disc;
  FILE *f;
  char iso_file_path[PATH_MAX];
  char msg[512];
  char code[16], disc_name[64];
  long long free_space, used_space;

  snprintf(iso_file_path, sizeof(iso_file_path), "%s/%s", cur_directory, filename);

  /* get ISO information */
  f = fopen(iso_file_path, "r");
  if (f == NULL) {
    show_error("Add ISO", "Error: can't open file\n\n%s", iso_file_path);
    return;
  }
  if (fread(code, 1, 6, f) != 6
      || fseeko(f, 0x20, SEEK_SET) != 0
      || fread(disc_name, 1, 0x40, f) != 0x40) {
    fclose(f);
    show_error("Add ISO", "Error: can't read file\n\n%s", iso_file_path);
    return;
  }
  fclose(f);
  code[6] = '\0';
  disc_name[0x39] = '\0';

  /* check if disc is not already there */
  disc = wbfs_open_disc(app_state.wbfs, (u8 *) code);
  if (disc != NULL) {
    wbfs_close_disc(disc);
    show_error("Add ISO", "The disc is already in the WBFS partition.");
    return;
  }

  /* check if there's enough free space */
  free_space = info_get_free_space();
  used_space = info_get_iso_size(iso_file_path, NULL);
  if (used_space > free_space) {
    show_error("Error", "Not enough space in WBFS partition.\n\nYou have %.2fGB free, this disc uses %.2fGB",
	       BYTES_TO_GB(free_space), BYTES_TO_GB(used_space));
    return;
  }
  if (! show_confirmation("Add ISO", "Add ISO file '%s'?\n\nCode: %s\nTitle: %s\nSpace: %.2fGB",
			  filename, code, disc_name, BYTES_TO_GB(used_space)))
    return;
  
  /* add iso */
  snprintf(msg, sizeof(msg), "Adding ISO file '%s'\n", filename);
  show_progress_dialog("Adding ISO", msg, iso_add_start, iso_file_path, iso_add_update, &cancel_wbfs_op, 0);
  update_iso_list();
}

/**
 * Change the current directory to the directory in the "fs_cur_dir" entry.
 */
static void change_cur_dir(void)
{
  GtkWidget *widget;
  GtkEntry *fs_cur_dir;
  char full_path[PATH_MAX];

  widget = get_widget("fs_cur_dir");
  fs_cur_dir = GTK_ENTRY(widget);
  strncpy(full_path, gtk_entry_get_text(fs_cur_dir), sizeof(full_path));
  full_path[sizeof(full_path)-1] = '\0';

  if (chdir(full_path) != 0)
    show_error("Error", "Can't change directory to '%s'.", full_path);
  else
    update_fs_list();
}

/**
 * Update the filesystem directory list.
 */
static void update_fs_list(void)
{
  GtkListStore *store;
  GtkTreeIter iter;
  GtkWidget *widget;
  GtkTreeView *fs_list;
  GtkEntry *fs_cur_dir;
  int i, list_dir_flags;

  widget = get_widget("fs_list");
  fs_list = GTK_TREE_VIEW(widget);
  store = GTK_LIST_STORE(gtk_tree_view_get_model(fs_list));
  widget = get_widget("fs_cur_dir");
  fs_cur_dir = GTK_ENTRY(widget);

  /* get current directory and copy it to current dir display */
  if (getcwd(cur_directory, sizeof(cur_directory)) == NULL)
    strcpy(cur_directory, "/");
  gtk_entry_set_text(fs_cur_dir, cur_directory);
  
  gtk_list_store_clear(store);

  /* free old dir list */
  for (i = 0; cur_dir_list[i].name != NULL; i++) {
    free(cur_dir_list[i].name);
    cur_dir_list[i].name = NULL;
  }

  /* read new dir list */
  list_dir_flags = LISTDIR_CASE_INSENSITIVE;
  if (app_state.show_hidden_files)
    list_dir_flags |= LISTDIR_SHOW_HIDDEN;
  if (list_dir_attr(cur_directory, "iso", list_dir_flags, cur_dir_list, ARRAY_SIZE(cur_dir_list)) == 0) {
    for (i = 0; cur_dir_list[i].name != NULL; i++) {
      char size[32];

      if (cur_dir_list[i].is_dir)
        size[0] = '\0';
      else {
        if (cur_dir_list[i].size < 1024)
          snprintf(size, sizeof(size), "%llu B", cur_dir_list[i].size);
        else if (cur_dir_list[i].size < 1024*1024)
          snprintf(size, sizeof(size), "%.2f kB", cur_dir_list[i].size / 1024.);
        else if (cur_dir_list[i].size < 1024*1024*1024)
          snprintf(size, sizeof(size), "%.2f MB", cur_dir_list[i].size / 1024. / 1024.);
        else
          snprintf(size, sizeof(size), "%.2f GB", cur_dir_list[i].size / 1024. / 1024. / 1024.);
      }

      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter,
			 0, cur_dir_list[i].is_dir,
			 1, cur_dir_list[i].name,
			 2, size,
			 -1);
    }
  }
  if (GTK_WIDGET_REALIZED(fs_list))
    gtk_tree_view_scroll_to_point(fs_list, 0, 0);
}

/**
 * Update the device ISO list
 */
static void update_iso_list(void)
{
  GtkListStore *store;
  GtkTreeIter iter;
  GtkWidget *widget;
  GtkTreeView *iso_list;
  u8 *buf;
  u32 size, block_count;
  int i, n;

  widget = get_widget("iso_list");
  iso_list = GTK_TREE_VIEW(widget);
  store = GTK_LIST_STORE(gtk_tree_view_get_model(iso_list));

  gtk_list_store_clear(store);

  if (app_state.wbfs == NULL)
    return;

  n = wbfs_count_discs(app_state.wbfs);
  buf = wbfs_ioalloc(0x100);
  for (i = 0; i < n; i++)
    if (wbfs_get_disc_info(app_state.wbfs, i, buf, 0x100, &size) == 0) {
      char code_txt[16], size_txt[32], name_txt[256];

      memcpy(code_txt, buf, 6);
      code_txt[6] = '\0';
      snprintf(size_txt, sizeof(size_txt), "%.2f GB", (size * 4ULL) / 1024.0 / 1024.0 / 1024.0);
      memcpy(name_txt, buf + 0x20, 0x40);
      name_txt[0x39] = '\0';

      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter,
			 0, code_txt,
			 1, name_txt,
			 2, size_txt,
			 -1);
    }
  wbfs_iofree(buf);

  /* set space usage information */
  block_count = wbfs_count_usedblocks(app_state.wbfs);
  set_label_double("label_total_space", " <b>%.2f</b>GB",
		   (double) app_state.wbfs->n_wbfs_sec * app_state.wbfs->wbfs_sec_sz / 1024. / 1024. / 1024.);
  set_label_double("label_used_space", " <b>%.2f</b>GB", 
		   (double) (app_state.wbfs->n_wbfs_sec - block_count) * app_state.wbfs->wbfs_sec_sz / 1024. / 1024. / 1024.);
  set_label_double("label_free_space", " <b>%.2f</b>GB",
		   (double) block_count * app_state.wbfs->wbfs_sec_sz / 1024. / 1024. / 1024.);
}

/**
 * Load the device 'app_state.cur_dev'
 */
static int load_device(void)
{
  GtkWidget *widget;
  char device_label[256];
  char *captured_msgs;

  widget = get_widget("device_name");

  /* close current device */
  if (app_state.wbfs != NULL) {
    wbfs_close(app_state.wbfs);
    app_state.wbfs = NULL;
  }
  if (app_state.cur_dev < 0)
    return 1;

  /* open device */
  start_msg_capture();
  app_state.wbfs = wbfs_try_open_partition(app_state.dev[app_state.cur_dev], 0);
  captured_msgs = end_msg_capture();
  if (app_state.wbfs == NULL) {
    gtk_label_set_markup(GTK_LABEL(widget), "<b>(none)</b>");
    set_label_double("label_total_space", NULL, 0.);
    set_label_double("label_used_space", NULL, 0.);
    set_label_double("label_free_space", NULL, 0.);
    if (*captured_msgs != '\0')
      show_error("Error", "Can't open device '%s': %s.\n\n(Do you have appropriate permissions?)",
		 app_state.dev[app_state.cur_dev],
		 captured_msgs);
    else
      show_error("Error", "Can't open device '%s'.\n\n(Do you have appropriate permissions?)",
		 app_state.dev[app_state.cur_dev]);
    update_iso_list();
    return 1;
  }

  /* set device label */
  snprintf(device_label, sizeof(device_label), "<b>%s</b>", app_state.dev[app_state.cur_dev]);
  gtk_label_set_markup(GTK_LABEL(widget), device_label);

  update_iso_list();
  return 0;
}

/**
 * Reload the list of devices
 */
static void reload_device_list(void)
{
  int i;
  GtkListStore *store;
  GtkTreeIter iter;
  GtkWidget *widget;
  GtkComboBox *dev_list;
  int new_sel_index;

  app_reload_device_list();

  widget = get_widget("device_list");
  dev_list = GTK_COMBO_BOX(widget);
  store = GTK_LIST_STORE(gtk_combo_box_get_model(dev_list));

  /* if no device open and there's a preferred one, select it */
  if (app_state.wbfs == NULL && app_state.def_dev >= 0)
    new_sel_index = app_state.def_dev;
  else {
    /* get current selection index */
    new_sel_index = -1;
    if (gtk_combo_box_get_active_iter(dev_list, &iter)) {
      char *cur_sel = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 0, &cur_sel, -1);
      for (i = 0; i < app_state.num_devs; i++) {
	if (cur_sel != NULL && strcmp(cur_sel, app_state.dev[i]) == 0) {
	  new_sel_index = i;
	  break;
	}
      }
      if (cur_sel != NULL)
	g_free(cur_sel);
    }
  }

  /* re-fill list */
  gtk_list_store_clear(store);
  for (i = 0; i < app_state.num_devs; i++) {
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0, app_state.dev[i], -1);
  }

  /* set selection */
  if (new_sel_index < 0)
    new_sel_index = 0;
  gtk_combo_box_set_active(dev_list, new_sel_index);
}

/**
 * Choose icon for file depending on type (file, directory, etc.)
 */
static void fs_list_icon_data_func(GtkTreeViewColumn *tree_column,
				   GtkCellRenderer *cell,
				   GtkTreeModel *tree_model,
				   GtkTreeIter *iter,
				   gpointer data)
{
  GValue val = {0};
  int is_dir;
  char *name = NULL;

  gtk_tree_model_get(tree_model, iter, 0, &is_dir, 1, &name, -1);

  /* set 'val' to stock icon to use */
  g_value_init(&val, G_TYPE_STRING);
  if (strcmp(name, ".") == 0)
    g_value_set_static_string(&val, GTK_STOCK_REFRESH);
  else if (strcmp(name, "..") == 0)
    g_value_set_static_string(&val, GTK_STOCK_GO_UP);
  else {
    switch (is_dir) {
    case 0: g_value_set_static_string(&val, GTK_STOCK_FILE); break;
    case 1: g_value_set_static_string(&val, GTK_STOCK_DIRECTORY); break;
    default: g_value_set_static_string(&val, GTK_STOCK_DIALOG_WARNING);
    }
  }

  if (name != NULL)
    g_free(name);

  g_object_set_property(G_OBJECT(cell), "stock-id", &val);
}

/**
 * Initialize widgets before main window is displayed.
 */
static void init_widgets(void)
{
  GtkCellRenderer *renderer;
  GtkListStore *list_store;
  GtkTreeView *iso_list;
  GtkTreeView *fs_list;
  GtkComboBox *dev_list;
  GtkEntry *fs_cur_dir;
  GtkWidget *widget;
  GtkTreeViewColumn *col;

  /* setup menus */
  widget = get_widget("menu_ignore_mounted_devices");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), ! app_state.ignore_mounted_devices);
  widget = get_widget("menu_view_hidden_files");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), app_state.show_hidden_files);
  widget = get_widget("menu_view_partitions");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), app_state.list_partitions);

  /* setup device list store */
  widget = get_widget("device_list");
  dev_list = GTK_COMBO_BOX(widget);
  list_store = gtk_list_store_new(1, G_TYPE_STRING);
  gtk_combo_box_set_model(dev_list, GTK_TREE_MODEL(list_store));
  g_object_unref(list_store);

  /* setup ISO list store and model */
  widget = get_widget("iso_list");
  iso_list = GTK_TREE_VIEW(widget);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_insert_column_with_attributes(iso_list, -1, "Code", renderer, "text", 0, NULL);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_insert_column_with_attributes(iso_list, -1, "Name", renderer, "text", 1, NULL);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_insert_column_with_attributes(iso_list, -1, "Size", renderer, "text", 2, NULL);
  list_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  gtk_tree_view_set_model(iso_list, GTK_TREE_MODEL(list_store));
  g_object_unref(list_store);
  col = gtk_tree_view_get_column(iso_list, 0);
  gtk_tree_view_column_set_sort_column_id(col, 0);
  col = gtk_tree_view_get_column(iso_list, 1);
  gtk_tree_view_column_set_sort_column_id(col, 1);
  gtk_tree_view_column_set_expand(col, 1);
  col = gtk_tree_view_get_column(iso_list, 2);
  gtk_tree_view_column_set_sort_column_id(col, 2);

  /* setup fs list store and model */
  widget = get_widget("fs_list");
  fs_list = GTK_TREE_VIEW(widget);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_insert_column_with_data_func(fs_list, -1, "", renderer, fs_list_icon_data_func, NULL, NULL);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_insert_column_with_attributes(fs_list, -1, "Name", renderer, "text", 1, NULL);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_insert_column_with_attributes(fs_list, -1, "Size", renderer, "text", 2, NULL);
  list_store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
  gtk_tree_view_set_model(fs_list, GTK_TREE_MODEL(list_store));
  g_object_unref(list_store);
  col = gtk_tree_view_get_column(fs_list, 1);
  gtk_tree_view_column_set_sort_column_id(col, 1);
  gtk_tree_view_column_set_expand(col, 1);
  col = gtk_tree_view_get_column(fs_list, 2);
  gtk_tree_view_column_set_sort_column_id(col, 2);

  /* setup fs dir */
  widget = get_widget("fs_cur_dir");
  fs_cur_dir = GTK_ENTRY(widget);
  cur_dir_list[0].name = NULL;
  update_fs_list();
}

/* ---- Callbacks -------------------------------------- */

void fs_list_row_activated_cb(GtkTreeView *tree_view,
                              GtkTreePath *path,
                              GtkTreeViewColumn *column,
                              gpointer user_data)
{
  char filename[256];
  char full_path[PATH_MAX];
  char *name;
  int mode;

  if (! get_selected_file(&mode, &name))
    return;

  strncpy(filename, name, sizeof(filename)-1);
  filename[sizeof(filename)-1] = '\0';
  g_free(name);

  snprintf(full_path, sizeof(full_path), "%s/%s", cur_directory, filename);

  switch (mode) {
  case 0:
    if (app_state.wbfs == NULL)
      show_message("Add ISO", "You must first load a WBFS device.");
    else
      confirm_add_iso_file(filename);
    break;

  case 1:
    if (chdir(full_path) != 0)
      show_error("Error", "Can't change directory to '%s'.", full_path);
    else
      update_fs_list();
    break;
    
  default:
    show_error("Error", "File '%s' has unsupported type.", full_path);
    break;
  }

}

void fs_go_home_clicked_cb(GtkButton *b, gpointer data)
{
  struct passwd *pw;

  pw = getpwuid (getuid());
  if (!pw)
    show_error("Error", "Can't get home directory for current user.");
  else if (chdir(pw->pw_dir) != 0)
    show_error("Error", "Can't change to directory '%s'.", pw->pw_dir);
  else
    update_fs_list();
}

void fs_set_dir_clicked_cb(GtkButton *b, gpointer data)
{
  change_cur_dir();
}

void fs_cur_dir_activate_cb(GtkEntry *e, gpointer data)
{
  change_cur_dir();
}

void reload_device_clicked_cb(GtkButton *b, gpointer data)
{
  char *device;

  get_selected_device(&device);

  app_state.cur_dev = get_device_id(device);
  load_device();

  g_free(device);
}

void reload_device_list_clicked_cb(GtkButton *b, gpointer data)
{
  reload_device_list();
}

void main_window_realize_cb(GtkWidget *w, gpointer data)
{
  reload_device_list();
  if (app_state.def_dev >= 0) {
    app_state.cur_dev = app_state.def_dev;
    start_msg_capture();
    load_device();
    end_msg_capture();
  }
}

void main_window_delete_event_cb(GtkWidget *widget, gpointer data)
{
  GtkWindow *main_window;
  int h,w,x,y;
  struct passwd *pw;
  FILE *f;

  main_window = GTK_WINDOW(get_widget("main_window"));
  gtk_window_get_size (main_window, &w, &h);
  gtk_window_get_position (main_window, &x, &y);

  pw = getpwuid (getuid ());
  if (pw)
  {
    int len = strlen (pw->pw_dir) + 15;
    char *path = malloc(len);

    snprintf(path, len, "%s/%s", pw->pw_dir, ".wbfs_gtk");

    f = fopen (path, "w");
    if (f)
    {
      fprintf(f, "[wbfs_gtk]\nwidth=%d\nheight=%d\nx=%d\ny=%d\n",
              w, h, x, y);
      fclose (f);
    }
    free (path);
  }


  gtk_main_quit();
}

void menu_init_wbfs_partition_activate_cb(GtkWidget *c, gpointer data)
{
  char *cur_sel;
  char mount_point[256];
  char device[256];
  char msg[1024];
  int ret;

  /* get selected device */
  if (! get_selected_device(&cur_sel))
    return;
  strncpy(device, cur_sel, sizeof(device));
  device[sizeof(device)-1] = '\0';
  g_free(cur_sel);

  /* give fair warning */
  snprintf(msg, sizeof(msg),
	   "WARNING: ALL PARTITION DATA WILL BE PERMANENTLY LOST!\n"
	   "\n"
	   "This will format for WBFS the partition\n"
	   "\n"
	   "%s\n"
	   "\n"
	   "Are you ABSOLUTELY SURE you want to do this?",
	   device);
  ret = show_warning_yes_no("Initialize WBFS Partition", msg);
  if (ret != 1)
    return;
  
  /* give extra warning if device seems to be mounted */
  if (is_device_mounted(device, mount_point, sizeof(mount_point))) {
    ret = show_warning_yes_no("Initialize WBFS Partition",
			      "The partition %s seems to be mounted in directory\n"
			      "\n"
			      "%s\n"
			      "\n"
			      "It is HIGHLY RECOMMENDED that you unmount the partition before proceeding. Are you sure you want to proceed?\n",
			      device,
			      mount_point);
    if (ret != 1)
      return;
  }
  
  if (op_init_partition(device) != 0)
    show_error("Error", "Error initializing partition.");
  else
    show_message("Initialize WBFS Partition", "Partition initialized successfully.");

  app_state.cur_dev = get_device_id(device);
  load_device();
}

void menu_about_activate_cb(GtkWidget *w, gpointer data)
{
  GtkWidget *about_dialog;

  about_dialog = get_widget("about_dialog");
  gtk_dialog_run(GTK_DIALOG(about_dialog));
}

void about_dialog_response_cb(GtkWidget *w, gpointer data)
{
  GtkWidget *about_dialog;

  about_dialog = get_widget("about_dialog");
  gtk_widget_hide(about_dialog);
}

void iso_extract_clicked_cb(GtkButton *b, gpointer user_data)
{
  char *code, *name;

  if (get_selected_disc(&code, &name)) {
    char iso_file[256];

    convert_discname_to_filename(iso_file, name, sizeof(iso_file)-4);
    strcat(iso_file, ".iso");

    if (show_confirmation("Extract ISO", "Extract ISO from\n\n%s\n\nto\n\n%s", name, iso_file)) {
      char iso_file_path[PATH_MAX];
      char msg[256];
      char *p[2];

      snprintf(iso_file_path, sizeof(iso_file_path), "%s/%s", cur_directory, iso_file);
      p[0] = code;
      p[1] = iso_file_path;

      snprintf(msg, sizeof(msg), "Extracting ISO to\n%s\n", iso_file);
      show_progress_dialog("Extracting ISO", msg, iso_extract_start, p, iso_extract_update, &cancel_wbfs_op, 1);
      update_fs_list();
    }
    
    g_free(code);
    g_free(name);
  }
}

void fs_add_iso_clicked_cb(GtkButton *b, gpointer user_data)
{
  int mode;
  char *filename;

  if (app_state.wbfs == NULL) {
    show_message("Add ISO", "You must first load a WBFS device.");
    return;
  }

  if (get_selected_file(&mode, &filename)) {
    if (mode != 0)
      show_message("Add ISO", "Please select an ISO file.", filename);
    else
      confirm_add_iso_file(filename);
    
    g_free(filename);
  }
}

void iso_remove_clicked_cb(GtkButton *b, gpointer user_data)
{
  char *code, *name;

  if (get_selected_disc(&code, &name)) {
    if (show_confirmation("Remove Disc", "Remove disc '%s' (%s)?", name, code))
      op_remove_disc(code);
    update_iso_list();

    g_free(code);
    g_free(name);
  }
}

void menu_iso_remove_activate_cb(GtkWidget *w, gpointer data)
{
  char *code, *name;

  if (get_selected_disc(&code, &name)) {
    if (show_confirmation("Remove Disc", "Remove disc '%s' (%s)?", name, code))
      op_remove_disc(code);
    update_iso_list();

    g_free(code);
    g_free(name);
  }
}

void menu_ignore_mounted_devices_toggled_cb(GtkCheckMenuItem *c, gpointer data)
{
  app_state.ignore_mounted_devices = gtk_check_menu_item_get_active(c) ? 0 : 1;
  reload_device_list();
}

void menu_view_hidden_files_toggled_cb(GtkCheckMenuItem *c, gpointer data)
{
  app_state.show_hidden_files = gtk_check_menu_item_get_active(c) ? 1 : 0;
  update_fs_list();
}

void menu_view_partitions_toggled_cb(GtkCheckMenuItem *c, gpointer data)
{
  app_state.list_partitions = gtk_check_menu_item_get_active(c) ? 1 : 0;
  reload_device_list();
}

void menu_iso_rename_activate_cb(GtkWidget *w, gpointer data)
{
  char *code, *name;

  if (get_selected_disc(&code, &name)) {
    char new_name[256];

    strncpy(new_name, name, sizeof(new_name));
    new_name[sizeof(new_name)-1] = '\0';

    if (show_text_input("Rename Disc", new_name, sizeof(new_name), "Rename disc with id '%s' to:", code)) {
      op_rename_disc(code, new_name);
    }

    g_free(code);
    g_free(name);

    update_iso_list();
  }
}

gboolean iso_list_button_release_event_cb(GtkWidget *w,
					  GdkEventButton *event,
					  gpointer user_data)
{
  if (app_state.wbfs == NULL)
    return FALSE;

  if (event->button == 3) {
    if (get_selected_disc(NULL, NULL)) {
      GtkWidget *widget;

      widget = get_widget("menu_iso_context");
      gtk_menu_popup(GTK_MENU(widget), NULL, NULL, NULL, NULL,
		     event->button, event->time);
    }
    return TRUE;
  }
  return FALSE;
}

void menu_quit_activate_cb(GtkWidget *w, gpointer data)
{
  gtk_main_quit();
}

void restore_main_window_size_pos (GtkWindow *main_window)
{
  struct passwd *pw;
  char *path;
  FILE *f;
  int len;
  gint w, h, x, y;

  pw = getpwuid (getuid ());
  if (!pw)
    return;

  len = strlen (pw->pw_dir) + 15;
  path = malloc (len);

  snprintf(path, len, "%s/%s", pw->pw_dir, ".wbfs_gtk");

  f = fopen (path, "r");

  if (!f)
    goto out;

  if (fscanf (f, "[wbfs_gtk]\nwidth=%d\nheight=%d\nx=%d\ny=%d\n",
              &w, &h, &x, &y) != 4)
	  goto out2;

  gtk_window_move (main_window, x, y);
  gtk_window_resize (main_window, w, h);

out2:
  fclose (f);
out:
  free (path);
}

int main(int argc, char *argv[])
{
  GtkWidget *main_window;

  app_init();
  gtk_init(&argc, &argv);
  glade_init();

  /* load glade XML */
  glade_xml = glade_xml_new_from_buffer((char *) wbfs_gui_glade, sizeof(wbfs_gui_glade), NULL, NULL);
  glade_xml_signal_autoconnect(glade_xml);
  init_widgets();

  /* show main window */
  main_window = get_widget("main_window");
  restore_main_window_size_pos (GTK_WINDOW(main_window));
  gtk_widget_show(main_window);

  gtk_main();

  return 0;
}
