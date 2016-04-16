/*
    This file is part of darktable,
    copyright (c) 2016 tobias ellinghaus.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/exif.h"
#include "lut/cairo.h"
#include "lut/colorchart.h"
#include "lut/common.h"
#include "lut/deltaE.h"
#include "lut/pfm.h"
#include "lut/thinplate.h"
#include "lut/tonecurve.h"
#include "version.h"


#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
  COLUMN_NAME,
  COLUMN_RGB_IN,
  COLUMN_LAB_IN,
  COLUMN_LAB_REF,
  COLUMN_DE_1976,
  COLUMN_DE_1976_FLOAT,
  COLUMN_DE_2000,
  COLUMN_DE_2000_FLOAT,
  NUM_COLUMNS
};

typedef struct dt_lut_t
{
  // gtk gui
  GtkWidget *window, *image_button, *cht_button, *it8_button, *reference_image_button, *reference_it8_box,
      *reference_image_box, *process_button, *export_button, *export_raw_button, *reference_mode, *gray_ramp,
      *number_patches;
  GtkWidget *treeview;
  GtkTreeModel *model;

  // loaded files to be drawn/referenced
  image_t source;
  image_t reference;
  char *reference_filename;

  // computed data
  chart_t *chart;
  GHashTable *picked_source_patches;
  char *tonecurve_encoded, *colorchecker_encoded;
} dt_lut_t;

// boring helper functions
static void init_image(dt_lut_t *self, image_t *image, GCallback motion_cb);
static void image_lab_to_xyz(float *image, const int width, const int height);
static void map_boundingbox_to_view(image_t *image, point_t *bb);
static point_t map_point_to_view(image_t *image, point_t p);
static void get_xyz_sample_from_image(const image_t *const image, box_t *box, float *xyz);
static void add_column(GtkTreeView *treeview, const char *title, int column_id, int sort_column);
static void update_table(dt_lut_t *self);
static void init_table(dt_lut_t *self);
static void get_Lab_from_box(box_t *box, float *Lab);
static void collect_source_patches(dt_lut_t *self);
static void collect_source_patches_foreach(gpointer key, gpointer value, gpointer user_data);
static void collect_reference_patches(dt_lut_t *self);
static void collect_reference_patches_foreach(gpointer key, gpointer value, gpointer user_data);
static box_t *find_patch(GHashTable *table, gpointer key);
static void get_boundingbox(const image_t *const image, point_t *bb);
static box_t get_sample_box(chart_t *chart, box_t *outer_box);
static void get_corners(point_t *bb, box_t *box, point_t *corners);
static void get_pixel_region(const image_t *const image, const point_t *const corners, int *x_start, int *y_start,
                             int *x_end, int *y_end);
static void reset_bb(image_t *image);
static void free_image(image_t *image);
static gboolean handle_motion(GtkWidget *widget, GdkEventMotion *event, dt_lut_t *self, image_t *image);
static int find_closest_corner(point_t *bb, float x, float y);
static void map_mouse_to_0_1(GtkWidget *widget, GdkEventMotion *event, image_t *image, float *x, float *y);
static void update_corner(image_t *image, int which, float *x, float *y);
static gboolean open_source_image(dt_lut_t *self, const char *filename);
static gboolean open_reference_image(dt_lut_t *self, const char *filename);
static gboolean open_image(image_t *image, const char *filename);
static gboolean open_cht(dt_lut_t *self, const char *filename);
static gboolean open_it8(dt_lut_t *self, const char *filename);

static void size_allocate_callback(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
  image_t *image = (image_t *)user_data;
  set_offset_and_scale(image, allocation->width, allocation->height);
}

static gboolean draw_image_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  image_t *image = (image_t *)user_data;
  chart_t *chart = *(image->chart);

  clear_background(cr);

  // done when no image is loaded
  if(!image->image)
  {
    draw_no_image(cr, widget);
    return FALSE;
  }

  center_image(cr, image);

  draw_image(cr, image);

  // done when no chart was loaded
  if(!chart) return FALSE;

  // draw overlay
  point_t bb[4];
  map_boundingbox_to_view(image, bb);

  draw_boundingbox(cr, bb);
  draw_f_boxes(cr, bb, chart);
  draw_d_boxes(cr, bb, chart);
  draw_color_boxes_outline(cr, bb, chart);

  stroke_boxes(cr, 1.0);

  draw_color_boxes_inside(cr, bb, chart, 2.0, image->draw_colored);

  return FALSE;
}

static void map_boundingbox_to_view(image_t *image, point_t *bb)
{
  for(int i = 0; i < 4; i++) bb[i] = map_point_to_view(image, image->bb[i]);
}

static point_t map_point_to_view(image_t *image, point_t p)
{
  point_t result;

  result.x = p.x * image->width / image->scale;
  result.y = p.y * image->height / image->scale;

  return result;
}

static gboolean motion_notify_callback_source(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  gboolean res = handle_motion(widget, event, self, &self->source);
  collect_source_patches(self);
  update_table(self);
  return res;
}

static gboolean motion_notify_callback_reference(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  gboolean res = handle_motion(widget, event, self, &self->reference);
  collect_reference_patches(self);
  update_table(self);
  return res;
}

static gboolean handle_motion(GtkWidget *widget, GdkEventMotion *event, dt_lut_t *self, image_t *image)
{
  if(!(event->state & GDK_BUTTON1_MASK) || !image->image) return FALSE;

  // mouse -> 0..1
  float x, y;
  map_mouse_to_0_1(widget, event, image, &x, &y);

  // dragging the crosses is hard when they are not in a cornerof the bb but sprinkled over the chart
  int closest_corner = find_closest_corner(image->bb, x, y);

  update_corner(image, closest_corner, &x, &y);

  image->bb[closest_corner].x = x;
  image->bb[closest_corner].y = y;

  gtk_widget_queue_draw(widget);

  return TRUE;
}

static int find_closest_corner(point_t *bb, float x, float y)
{
  int closest_corner = 0;
  float distance = G_MAXFLOAT;
  for(int i = 0; i < 4; i++)
  {
    const float d_x = (x - bb[i].x);
    const float d_y = (y - bb[i].y);
    float d = d_x * d_x + d_y * d_y;
    if(d < distance)
    {
      distance = d;
      closest_corner = i;
    }
  }
  // TODO: only react when the distance < some threshold?
  return closest_corner;
}

static void map_mouse_to_0_1(GtkWidget *widget, GdkEventMotion *event, image_t *image, float *x, float *y)
{
  guint width = gtk_widget_get_allocated_width(widget);
  guint height = gtk_widget_get_allocated_height(widget);

  *x = (event->x - image->offset_x) / (width - 2.0 * image->offset_x);
  *y = (event->y - image->offset_y) / (height - 2.0 * image->offset_y);
}

static void update_corner(image_t *image, int which, float *x, float *y)
{
  // keep the corners in clockwise order
  if(which == TOP_LEFT)
  {
    *x = CLAMP(*x, 0.0, image->bb[TOP_RIGHT].x);
    *y = CLAMP(*y, 0.0, image->bb[BOTTOM_LEFT].y);
  }
  else if(which == TOP_RIGHT)
  {
    *x = CLAMP(*x, image->bb[TOP_LEFT].x, 1.0);
    *y = CLAMP(*y, 0.0, image->bb[BOTTOM_RIGHT].y);
  }
  else if(which == BOTTOM_RIGHT)
  {
    *x = CLAMP(*x, image->bb[BOTTOM_LEFT].x, 1.0);
    *y = CLAMP(*y, image->bb[TOP_RIGHT].y, 1.0);
  }
  else if(which == BOTTOM_LEFT)
  {
    *x = CLAMP(*x, 0.0, image->bb[BOTTOM_RIGHT].x);
    *y = CLAMP(*y, image->bb[TOP_LEFT].y, 1.0);
  }
}

static void source_image_changed_callback(GtkFileChooserButton *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  char *new_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  open_source_image(self, new_filename);
  g_free(new_filename);
}

static gboolean open_source_image(dt_lut_t *self, const char *filename)
{
  gboolean res = open_image(&self->source, filename);
  gtk_widget_set_sensitive(self->cht_button, res);
  if(!res) gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->image_button));
  gtk_widget_queue_draw(self->source.drawing_area);

  return res;
}

static void ref_image_changed_callback(GtkFileChooserButton *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  char *new_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  open_reference_image(self, new_filename);
  g_free(new_filename);
}

static char *get_filename_base(const char *filename)
{
  char *last_slash = g_strrstr(filename, "/");
  if(last_slash)
    return g_strdup(last_slash + 1);
  else
    return g_strdup(filename);
}

static gboolean open_reference_image(dt_lut_t *self, const char *filename)
{
  gboolean res = open_image(&self->reference, filename);
  gtk_widget_set_sensitive(self->process_button, FALSE);
  gtk_widget_set_sensitive(self->export_button, FALSE);
  gtk_widget_set_sensitive(self->export_raw_button, FALSE);
  if(!res)
    gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->reference_image_button));
  else
  {
    collect_reference_patches(self);
    update_table(self);
    free(self->reference_filename);
    self->reference_filename = get_filename_base(filename);
  }
  gtk_widget_queue_draw(self->reference.drawing_area);
  return res;
}

static gboolean open_image(image_t *image, const char *filename)
{
  int width, height;

  free_image(image);

  if(!filename) return FALSE;

  float *pfm = read_pfm(filename, &width, &height);

  if(!pfm)
  {
    fprintf(stderr, "error reading image `%s'\n", filename);
    return FALSE;
  }

  // we want the image in XYZ to average patches
  image_lab_to_xyz(pfm, width, height);

  cairo_surface_t *image_surface = cairo_surface_create_from_xyz_data(pfm, width, height);

  if(cairo_surface_status(image_surface) != CAIRO_STATUS_SUCCESS)
  {
    fprintf(stderr, "error creating cairo surface from `%s'\n", filename);
    cairo_surface_destroy(image_surface);
    free(pfm);
    return FALSE;
  }
  image->surface = image_surface;
  image->image = cairo_pattern_create_for_surface(image_surface);
  image->width = width;
  image->height = height;
  image->xyz = pfm;

  // at init time this can fail once
  if(GTK_IS_WIDGET(image->drawing_area))
  {
    guint width = gtk_widget_get_allocated_width(image->drawing_area);
    guint height = gtk_widget_get_allocated_height(image->drawing_area);
    set_offset_and_scale(image, width, height);
  }

  return TRUE;
}

static void cht_changed_callback(GtkFileChooserButton *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  char *new_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  open_cht(self, new_filename);
  g_free(new_filename);
}

static gboolean open_cht(dt_lut_t *self, const char *filename)
{
  if(self->chart) free_chart(self->chart);
  gboolean res = ((self->chart = parse_cht(filename)) != NULL);

  reset_bb(&self->source);
  reset_bb(&self->reference);

  g_hash_table_remove_all(self->picked_source_patches);
  if(res) collect_source_patches(self);
  init_table(self);

  // reset it8/reference entry
  if(!res) gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->cht_button));
  gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->it8_button));
  gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->reference_image_button));

  // fill the gray ramp selector
  if(res)
  {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self->gray_ramp));
    GList *patch_sets = g_hash_table_get_keys(self->chart->patch_sets);
    patch_sets = g_list_sort(patch_sets, (GCompareFunc)g_strcmp0);
    for(GList *iter = patch_sets; iter; iter = g_list_next(iter))
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->gray_ramp), NULL, (char *)iter->data);
    g_list_free(patch_sets);
  }

  gtk_widget_set_sensitive(self->it8_button, res);
  gtk_widget_set_sensitive(self->reference_image_button, res);
  gtk_widget_set_sensitive(self->process_button, FALSE);
  gtk_widget_set_sensitive(self->export_button, FALSE);
  gtk_widget_set_sensitive(self->export_raw_button, FALSE);

  gtk_widget_queue_draw(self->source.drawing_area);
  gtk_widget_queue_draw(self->reference.drawing_area);

  return res;
}

static void reference_mode_changed_callback(GtkComboBox *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  int selected = gtk_combo_box_get_active(widget);
  if(selected == 0)
  {
    // it8
    gtk_widget_set_no_show_all(self->reference_it8_box, FALSE);
    gtk_widget_show_all(self->reference_it8_box);
    gtk_widget_hide(self->reference_image_box);
    gtk_widget_hide(self->reference.drawing_area);
    g_signal_emit_by_name(self->it8_button, "file-set", user_data);
  }
  else
  {
    // image
    gtk_widget_set_no_show_all(self->reference_image_box, FALSE);
    gtk_widget_set_no_show_all(self->reference.drawing_area, FALSE);
    gtk_widget_show_all(self->reference_image_box);
    gtk_widget_show_all(self->reference.drawing_area);
    gtk_widget_hide(self->reference_it8_box);
    g_signal_emit_by_name(self->reference_image_button, "file-set", user_data);
  }
}

static void it8_changed_callback(GtkFileChooserButton *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  char *new_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  open_it8(self, new_filename);
  g_free(new_filename);
}

static gboolean open_it8(dt_lut_t *self, const char *filename)
{
  if(!self->chart || !filename) return FALSE;
  gboolean res = parse_it8(filename, self->chart);
  collect_source_patches(self);
  update_table(self);

  gtk_widget_set_sensitive(self->process_button, FALSE);
  gtk_widget_set_sensitive(self->export_button, FALSE);
  gtk_widget_set_sensitive(self->export_raw_button, FALSE);
  if(!res)
    gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(self->it8_button));
  else
  {
    free(self->reference_filename);
    self->reference_filename = get_filename_base(filename);
  }
  gtk_widget_queue_draw(self->source.drawing_area);

  return res;
}

static char *get_export_filename(dt_lut_t *self, char **name, char **description)
{
  GtkWidget *name_entry = NULL, *description_entry = NULL;
  GtkWidget *dialog
      = gtk_file_chooser_dialog_new("save file", GTK_WINDOW(self->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                    _("_cancel"), GTK_RESPONSE_CANCEL, _("_save"), GTK_RESPONSE_ACCEPT, NULL);

  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);

  char *reference_filename = g_strdup(self->reference_filename);
  char *last_dot = g_strrstr(reference_filename, ".");
  if(last_dot)
  {
    *last_dot = '\0';
    char *new_filename = g_strconcat(reference_filename, ".dtstyle", NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), new_filename);
    g_free(reference_filename);
    g_free(new_filename);
  }

  if(name && description)
  {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    name_entry = gtk_entry_new();
    description_entry = gtk_entry_new();

    if(*name) gtk_entry_set_text(GTK_ENTRY(name_entry), *name);
    if(*description) gtk_entry_set_text(GTK_ENTRY(description_entry), *description);
    g_free(*name);
    g_free(*description);
    *name = NULL;
    *description = NULL;

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("style name"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("style description"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), description_entry, 1, 1, 1, 1);

    gtk_widget_show_all(grid);

    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dialog), grid);
  }

  char *filename = NULL;
  int res = gtk_dialog_run(GTK_DIALOG(dialog));
  if(res == GTK_RESPONSE_ACCEPT)
  {
    filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if(name && description)
    {
      *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(name_entry)));
      *description = g_strdup(gtk_entry_get_text(GTK_ENTRY(description_entry)));
    }
  }
  gtk_widget_destroy(dialog);

  return filename;
}

static void print_patches(dt_lut_t *self, FILE *fd, GList *patch_names)
{
  for(GList *iter = patch_names; iter; iter = g_list_next(iter))
  {
    char s[64];
    char *key = (char *)iter->data;
    box_t *source_patch = (box_t *)g_hash_table_lookup(self->picked_source_patches, key);
    box_t *reference_patch = (box_t *)g_hash_table_lookup(self->chart->box_table, key);
    if(!source_patch || !reference_patch)
    {
      fprintf(stderr, "error: missing patch `%s'\n", key);
      continue;
    }

    float source_Lab[3], reference_Lab[3];
    get_Lab_from_box(source_patch, source_Lab);
    get_Lab_from_box(reference_patch, reference_Lab);

    fprintf(fd, "%s", key);
    for(int i = 0; i < 3; i++) fprintf(fd, ";%s", g_ascii_dtostr(s, sizeof(s), source_Lab[i]));
    for(int i = 0; i < 3; i++) fprintf(fd, ";%s", g_ascii_dtostr(s, sizeof(s), reference_Lab[i]));
    fprintf(fd, "\n");
  }
}

static void print_xml_plugin(FILE *fd, int num, int op_version, const char *operation, const char *op_params,
                             gboolean enabled)
{
  fprintf(fd, "  <plugin>\n");
  fprintf(fd, "    <num>%d</num>\n", num);
  fprintf(fd, "    <module>%d</module>\n", op_version);
  fprintf(fd, "    <operation>%s</operation>\n", operation);
  fprintf(fd, "    <op_params>%s</op_params>\n", op_params);
  fprintf(fd, "    <enabled>%d</enabled>\n", enabled);
  fprintf(fd, "    <blendop_params>gz12eJxjYGBgkGAAgRNODESDBnsIHll8ANNSGQM=</blendop_params>\n");
  fprintf(fd, "    <blendop_version>7</blendop_version>\n");
  fprintf(fd, "    <multi_priority>0</multi_priority>\n");
  fprintf(fd, "    <multi_name> </multi_name>\n");
  fprintf(fd, "  </plugin>\n");
}

static void export_style(dt_lut_t *self, const char *filename, const char *name, const char *description)
{
  int num = 0;

  FILE *fd = fopen(filename, "w");
  if(!fd) return;

  fprintf(fd, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  fprintf(fd, "<darktable_style version=\"1.0\">\n");
  fprintf(fd, "<info>\n");
  fprintf(fd, "  <name>%s</name>\n", name);
  fprintf(fd, "  <description>%s</description>\n", description);
  fprintf(fd, "</info>\n");
  fprintf(fd, "<style>\n");

  // 0: disable basecurve
  print_xml_plugin(fd, num++, 2, "basecurve",
                   "gz09eJxjYIAAM6vnNnqyn22E9n235b6aa3cy6rVdRaK9/Y970fYf95bbMzA0QPEoGEqADYnNhMQGAO0WEJo=", FALSE);
  // 1: set colorin to standard matrix
  print_xml_plugin(fd, num++, 4, "colorin", "gz10eJzjZqA/AAAFcAAM", TRUE);
  // 2: add tonecurve
  print_xml_plugin(fd, num++, 4, "tonecurve", self->tonecurve_encoded, TRUE);
  // 3: add lut
  print_xml_plugin(fd, num++, 2, "colorchecker", self->colorchecker_encoded, TRUE);

  fprintf(fd, "</style>\n");
  fprintf(fd, "</darktable_style>\n");

  fclose(fd);
}

static void export_raw(dt_lut_t *self, char *filename)
{
  GHashTableIter table_iter;
  gpointer key, value;

  FILE *fd = fopen(filename, "w");
  if(!fd) return;

  char *gray_ramp_key = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(self->gray_ramp));

  // TODO: if no gray ramp was selected (for example with ColorChecker where the gray patches are not in their
  // own patch set) we should go over all patches and find those that look neutral and are in a row/column

  fprintf(fd, "patch;L_source;a_source;b_source;L_reference;a_reference;b_reference\n");
  // iterate over all known patches in the chart, gray ramp last
  g_hash_table_iter_init(&table_iter, self->chart->patch_sets);
  while(g_hash_table_iter_next(&table_iter, &key, &value))
  {
    if(!g_strcmp0(gray_ramp_key, (char *)key)) continue;
    GList *patch_names = (GList *)value;
    print_patches(self, fd, patch_names);
  }

  if(gray_ramp_key)
  {
    GList *patch_names = g_hash_table_lookup(self->chart->patch_sets, gray_ramp_key);
    if(patch_names) print_patches(self, fd, patch_names);
  }

  g_free(gray_ramp_key);
  fclose(fd);
}

static void export_raw_button_clicked_callback(GtkButton *button, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  if(!self->chart) return;

  // TODO: propose a filename
  char *filename = get_export_filename(self, NULL, NULL);
  if(filename) export_raw(self, filename);
  g_free(filename);
}

static void export_button_clicked_callback(GtkButton *button, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  if(!self->tonecurve_encoded || !self->colorchecker_encoded) return;

  // TODO: propose a filename
  char *name = g_strdup(self->reference_filename),
       *description = g_strdup_printf("fitted LUT style from %s", self->reference_filename);

  char *name_dot = g_strrstr(name, ".");
  if(name_dot) *name_dot = '\0';

  char *filename = get_export_filename(self, &name, &description);

  if(filename) export_style(self, filename, name, description);
  g_free(name);
  g_free(description);
  g_free(filename);
}

static void add_patches_to_array(dt_lut_t *self, GList *patch_names, int *N, int *i, double *target_L,
                                 double *target_a, double *target_b, double *colorchecker_Lab)
{

  for(GList *iter = patch_names; iter; iter = g_list_next(iter))
  {
    const char *key = (char *)iter->data;
    box_t *source_patch = (box_t *)g_hash_table_lookup(self->picked_source_patches, key);
    box_t *reference_patch = (box_t *)g_hash_table_lookup(self->chart->box_table, key);
    if(!source_patch || !reference_patch)
    {
      fprintf(stderr, "error: missing patch `%s'\n", key);
      continue;
    }

    float source_Lab[3], reference_Lab[3];
    get_Lab_from_box(source_patch, source_Lab);
    get_Lab_from_box(reference_patch, reference_Lab);

    for(int j = 0; j < 3; j++) colorchecker_Lab[3 * (*i) + j] = source_Lab[j];
    target_L[*i] = reference_Lab[0];
    target_a[*i] = reference_Lab[1];
    target_b[*i] = reference_Lab[2];

    const double thrs = 200.0;
    const double deltaE = dt_colorspaces_deltaE_1976(source_Lab, reference_Lab);
    if(deltaE > thrs)
    {
      fprintf(stderr, "warning: ignoring patch %s with large difference deltaE %g!\n", key, deltaE);
      fprintf(stderr, "      %g %g %g -- %g %g %g\n", source_Lab[0], source_Lab[1], source_Lab[2],
              reference_Lab[0], reference_Lab[1], reference_Lab[2]);
      (*N)--; // ignore this patch.
      (*i)--;
    }
    (*i)++;
  }
}

static char *encode_tonecurve(const tonecurve_t *c)
{
  // hardcoded params v4 from tonecurve:
  typedef struct dt_iop_tonecurve_node_t
  {
    float x;
    float y;
  } dt_iop_tonecurve_node_t;
  typedef struct dt_iop_tonecurve_params_t
  {
    dt_iop_tonecurve_node_t tonecurve[3][20]; // three curves (L, a, b) with max number
    // of nodes
    int tonecurve_nodes[3];
    int tonecurve_type[3];
    int tonecurve_autoscale_ab;
    int tonecurve_preset;
    int tonecurve_unbound_ab;
  } dt_iop_tonecurve_params_t;

  dt_iop_tonecurve_params_t params;
  memset(&params, 0, sizeof(params));
  params.tonecurve_autoscale_ab = 0; // manual
  params.tonecurve_type[0] = 2;      // MONOTONE_HERMITE
  params.tonecurve_type[1] = 2;      // MONOTONE_HERMITE
  params.tonecurve_type[2] = 2;      // MONOTONE_HERMITE
  params.tonecurve_nodes[0] = 20;
  params.tonecurve_nodes[1] = 2;
  params.tonecurve_nodes[2] = 2;
  params.tonecurve[1][0].x = 0.0f;
  params.tonecurve[1][0].y = 0.0f;
  params.tonecurve[1][1].x = 1.0f;
  params.tonecurve[1][1].y = 1.0f;
  params.tonecurve[2][0].x = 0.0f;
  params.tonecurve[2][0].y = 0.0f;
  params.tonecurve[2][1].x = 1.0f;
  params.tonecurve[2][1].y = 1.0f;

  for(int k = 0; k < 20; k++)
  {
    const double x = (k / 19.0) * (k / 19.0);
    params.tonecurve[0][k].x = x;
    params.tonecurve[0][k].y = tonecurve_apply(c, 100.0 * x) / 100.0;
  }

  return dt_exif_xmp_encode_internal((uint8_t *)&params, sizeof(params), NULL, FALSE);
}

static char *encode_colorchecker(int num, const double *point, const double **target, int *permutation)
{
// hardcoded v2 of the module
#define MAX_PATCHES 50
  typedef struct dt_iop_colorchecker_params_t
  {
    float source_L[MAX_PATCHES];
    float source_a[MAX_PATCHES];
    float source_b[MAX_PATCHES];
    float target_L[MAX_PATCHES];
    float target_a[MAX_PATCHES];
    float target_b[MAX_PATCHES];
    int32_t num_patches;
  } dt_iop_colorchecker_params_t;

  dt_iop_colorchecker_params_t params;
  memset(&params, 0, sizeof(params));
  num = MIN(24, num); // XXX currently the gui doesn't fare well with other numbers
                      //   assert(num <= MAX_PATCHES);
  params.num_patches = num;

  for(int k = 0; k < num; k++)
  {
    params.source_L[k] = point[3 * permutation[k]];
    params.source_a[k] = point[3 * permutation[k] + 1];
    params.source_b[k] = point[3 * permutation[k] + 2];
    params.target_L[k] = target[0][permutation[k]];
    params.target_a[k] = target[1][permutation[k]];
    params.target_b[k] = target[2][permutation[k]];
  }

#define SWAP(a, b)                                                                                                \
  {                                                                                                               \
    const float tmp = (a);                                                                                        \
    (a) = (b);                                                                                                    \
    (b) = tmp;                                                                                                    \
  }
  // bubble sort by octant and brightness:
  for(int k = 0; k < num - 1; k++)
    for(int j = 0; j < num - k - 1; j++)
    {
      if(thinplate_color_pos(params.source_L[j], params.source_a[j], params.source_b[j])
         < thinplate_color_pos(params.source_L[j + 1], params.source_a[j + 1], params.source_b[j + 1]))
      {
        SWAP(params.source_L[j], params.source_L[j + 1]);
        SWAP(params.source_a[j], params.source_a[j + 1]);
        SWAP(params.source_b[j], params.source_b[j + 1]);
        SWAP(params.target_L[j], params.target_L[j + 1]);
        SWAP(params.target_a[j], params.target_a[j + 1]);
        SWAP(params.target_b[j], params.target_b[j + 1]);
      }
    }
#undef SWAP
#undef MAX_PATCHES

  return dt_exif_xmp_encode_internal((uint8_t *)&params, sizeof(params), NULL, FALSE);
}

static void process_button_clicked_callback(GtkButton *button, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;

  gtk_widget_set_sensitive(self->export_button, FALSE);
  free(self->tonecurve_encoded);
  free(self->colorchecker_encoded);
  self->tonecurve_encoded = NULL;
  self->colorchecker_encoded = NULL;

  char *gray_ramp_key = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(self->gray_ramp));
  if(!self->chart) return;

  // TODO: if no gray ramp was selected (for example with ColorChecker where the gray patches are not in their
  // own patch set) we should go over all patches and find those that look neutral and are in a row/column
  if(!gray_ramp_key) return;

  GList *gray_ramp = g_hash_table_lookup(self->chart->patch_sets, gray_ramp_key);
  if(!gray_ramp)
  {
    g_free(gray_ramp_key);
    return;
  }

  int i = 0;
  int N = g_hash_table_size(self->chart->box_table);
  const int num_tonecurve = g_list_length(gray_ramp);

  double *target_L = (double *)calloc(sizeof(double), (N + 4));
  double *target_a = (double *)calloc(sizeof(double), (N + 4));
  double *target_b = (double *)calloc(sizeof(double), (N + 4));
  double *colorchecker_Lab = (double *)calloc(3 * sizeof(double), N);

  GHashTableIter table_iter;
  gpointer set_key, value;

  g_hash_table_iter_init(&table_iter, self->chart->patch_sets);
  while(g_hash_table_iter_next(&table_iter, &set_key, &value))
  {
    if(!g_strcmp0(gray_ramp_key, (char *)set_key)) continue;
    GList *patch_names = (GList *)value;
    add_patches_to_array(self, patch_names, &N, &i, target_L, target_a, target_b, colorchecker_Lab);
  }

  if(gray_ramp_key) // TODO: ColorChecker like charts without a ramp patch set just happen to work by accident
  {
    GList *patch_names = g_hash_table_lookup(self->chart->patch_sets, gray_ramp_key);
    if(patch_names)
      add_patches_to_array(self, patch_names, &N, &i, target_L, target_a, target_b, colorchecker_Lab);
  }

  tonecurve_t tonecurve;
  double cx[num_tonecurve], cy[num_tonecurve];
  cx[0] = cy[0] = 0.0;                                   // fix black
  cx[num_tonecurve - 1] = cy[num_tonecurve - 1] = 100.0; // fix white
  for(int k = 1; k < num_tonecurve - 1; k++)
    cx[num_tonecurve - 1 - k] = colorchecker_Lab[3 * (N - num_tonecurve + 2 + k - 1)];
  for(int k = 1; k < num_tonecurve - 1; k++) cy[num_tonecurve - 1 - k] = target_L[N - num_tonecurve + 2 + k - 1];
  tonecurve_create(&tonecurve, cx, cy, num_tonecurve);

  for(int k = 0; k < num_tonecurve; k++)
    fprintf(stderr, "L[%g] = %g\n", 100.0 * k / (num_tonecurve - 1.0f),
            tonecurve_apply(&tonecurve, 100.0f * k / (num_tonecurve - 1.0f)));

  // unapply from target data, we will apply it later in the pipe and want to match the colours only:
  for(int k = 0; k < N; k++) target_L[k] = tonecurve_unapply(&tonecurve, target_L[k]);

  int sparsity = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->number_patches)) + 4;
  printf("%d\n", sparsity);
  const double *target[3] = { target_L, target_a, target_b };
  double coeff_L[N + 4], coeff_a[N + 4], coeff_b[N + 4];
  double *coeff[] = { coeff_L, coeff_a, coeff_b };
  int perm[N + 4];
  sparsity = thinplate_match(&tonecurve, 3, N, colorchecker_Lab, target, sparsity, perm, coeff);

  int sp = 0;
  int cperm[300];
  for(int k = 0; k < sparsity; k++)
    if(perm[k] < N) // skip polynomial parts
      cperm[sp++] = perm[k];

  fprintf(stderr, "found %d basis functions:\n", sp);
  for(int k = 0; k < sp; k++)
    fprintf(stderr, "perm[%d] = %d source %g %g %g\n", k, cperm[k], colorchecker_Lab[3 * cperm[k]],
            colorchecker_Lab[3 * cperm[k] + 1], colorchecker_Lab[3 * cperm[k] + 2]);

  self->tonecurve_encoded = encode_tonecurve(&tonecurve);
  self->colorchecker_encoded = encode_colorchecker(sp, colorchecker_Lab, target, cperm);
  gtk_widget_set_sensitive(self->export_button, TRUE);

  free(target_L);
  free(target_a);
  free(target_b);
  free(colorchecker_Lab);

  g_free(gray_ramp_key);
}

static void cht_state_callback(GtkWidget *widget, GtkStateFlags flags, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  // cht not sensitive -> no reference or export
  if(flags & GTK_STATE_FLAG_INSENSITIVE)
  {
    gtk_widget_set_sensitive(self->it8_button, FALSE);
    gtk_widget_set_sensitive(self->reference_image_button, FALSE);
    gtk_widget_set_sensitive(self->process_button, FALSE);
    gtk_widget_set_sensitive(self->export_button, FALSE);
    gtk_widget_set_sensitive(self->export_raw_button, FALSE);
  }
}

static void gray_ramp_changed_callback(GtkComboBox *widget, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  int active = gtk_combo_box_get_active(widget);
  if(active >= 0)
  {
    gtk_widget_set_sensitive(self->process_button, TRUE);
    gtk_widget_set_sensitive(self->export_raw_button, TRUE);
  }
}

static GtkWidget *create_notebook_page_source(dt_lut_t *self)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, TRUE, 0);

  GtkWidget *image_button = gtk_file_chooser_button_new("image of a color chart", GTK_FILE_CHOOSER_ACTION_OPEN);
  g_signal_connect(image_button, "file-set", G_CALLBACK(source_image_changed_callback), self);

  GtkWidget *cht_button
      = gtk_file_chooser_button_new("description of a color chart", GTK_FILE_CHOOSER_ACTION_OPEN);
  g_signal_connect(cht_button, "file-set", G_CALLBACK(cht_changed_callback), self);

  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("image:"), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), image_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("chart:"), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), cht_button, TRUE, TRUE, 0);

  init_image(self, &self->source, G_CALLBACK(motion_notify_callback_source));
  self->source.draw_colored = TRUE;
  gtk_box_pack_start(GTK_BOX(page), self->source.drawing_area, TRUE, TRUE, 0);

  g_signal_connect(cht_button, "state-flags-changed", G_CALLBACK(cht_state_callback), self);

  self->image_button = image_button;
  self->cht_button = cht_button;

  return page;
}

static GtkWidget *create_notebook_page_reference(dt_lut_t *self)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(page), hbox, FALSE, TRUE, 0);

  GtkWidget *reference_mode = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(reference_mode), NULL, "cie/it8 file");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(reference_mode), NULL, "color chart image");
  gtk_combo_box_set_active(GTK_COMBO_BOX(reference_mode), 0);
  g_signal_connect(reference_mode, "changed", G_CALLBACK(reference_mode_changed_callback), self);

  GtkWidget *it8_button
      = gtk_file_chooser_button_new("reference data of a color chart", GTK_FILE_CHOOSER_ACTION_OPEN);
  g_signal_connect(it8_button, "file-set", G_CALLBACK(it8_changed_callback), self);

  GtkWidget *reference_image_button
      = gtk_file_chooser_button_new("image of a color chart", GTK_FILE_CHOOSER_ACTION_OPEN);
  g_signal_connect(reference_image_button, "file-set", G_CALLBACK(ref_image_changed_callback), self);

  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("mode:"), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), reference_mode, TRUE, TRUE, 0);

  GtkWidget *reference_it8_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(reference_it8_box), gtk_label_new("reference it8:"), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(reference_it8_box), it8_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), reference_it8_box, TRUE, TRUE, 0);

  GtkWidget *reference_image_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(reference_image_box), gtk_label_new("reference image:"), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(reference_image_box), reference_image_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), reference_image_box, TRUE, TRUE, 0);

  init_image(self, &self->reference, G_CALLBACK(motion_notify_callback_reference));
  self->reference.draw_colored = FALSE;
  gtk_box_pack_start(GTK_BOX(page), self->reference.drawing_area, TRUE, TRUE, 0);

  gtk_widget_show_all(reference_it8_box);
  gtk_widget_show_all(reference_image_box);
  gtk_widget_show_all(self->reference.drawing_area);
  gtk_widget_hide(reference_image_box);
  gtk_widget_hide(self->reference.drawing_area);
  gtk_widget_set_no_show_all(reference_it8_box, TRUE);
  gtk_widget_set_no_show_all(reference_image_box, TRUE);
  gtk_widget_set_no_show_all(self->reference.drawing_area, TRUE);

  self->reference_mode = reference_mode;
  self->it8_button = it8_button;
  self->reference_image_button = reference_image_button;
  self->reference_it8_box = reference_it8_box;
  self->reference_image_box = reference_image_box;

  return page;
}

static GtkWidget *create_notebook_page_process(dt_lut_t *self)
{
  GtkWidget *page = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(page), 10);
  gtk_grid_set_column_spacing(GTK_GRID(page), 10);

  int line = 0;

  GtkWidget *gray_ramp = gtk_combo_box_text_new();
  gtk_grid_attach(GTK_GRID(page), gtk_label_new("patches with gray ramp"), 0, line, 1, 1);
  gtk_grid_attach(GTK_GRID(page), gray_ramp, 1, line++, 1, 1);

  // TODO: it might make sense to limit this to a smaller range and/or use a slider
  // 50 is the current max in the lut iop
  GtkWidget *number_patches = gtk_spin_button_new_with_range(0, 50, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(number_patches), 24);
  gtk_grid_attach(GTK_GRID(page), gtk_label_new("number of final patches"), 0, line, 1, 1);
  gtk_grid_attach(GTK_GRID(page), number_patches, 1, line++, 1, 1);

  GtkWidget *process_button = gtk_button_new_with_label("process");
  GtkWidget *export_button = gtk_button_new_with_label("export");
  GtkWidget *export_raw_button = gtk_button_new_with_label("export raw data as csv");
  gtk_grid_attach(GTK_GRID(page), process_button, 1, line, 1, 1);
  gtk_grid_attach(GTK_GRID(page), export_button, 2, line, 1, 1);
  gtk_grid_attach(GTK_GRID(page), export_raw_button, 3, line++, 1, 1);

  g_signal_connect(gray_ramp, "changed", G_CALLBACK(gray_ramp_changed_callback), self);
  g_signal_connect(process_button, "clicked", G_CALLBACK(process_button_clicked_callback), self);
  g_signal_connect(export_button, "clicked", G_CALLBACK(export_button_clicked_callback), self);
  g_signal_connect(export_raw_button, "clicked", G_CALLBACK(export_raw_button_clicked_callback), self);

  self->gray_ramp = gray_ramp;
  self->number_patches = number_patches;
  self->process_button = process_button;
  self->export_button = export_button;
  self->export_raw_button = export_raw_button;

  return page;
}

static GtkWidget *create_notebook(dt_lut_t *self)
{
  // the notebook with 2 tabs for input
  GtkWidget *notebook = gtk_notebook_new();

  // first tab: input image + cht file
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_notebook_page_source(self),
                           gtk_label_new("source image"));

  // second tab: mode + either reference image or cie file
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_notebook_page_reference(self),
                           gtk_label_new("reference values"));

  // third tab: analyze data and process it
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), create_notebook_page_process(self), gtk_label_new("process"));

  return notebook;
}

static GtkWidget *create_table(dt_lut_t *self)
{
  GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(scrolled_window, -1, 15);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_window), GTK_SHADOW_ETCHED_IN);
  //   gtk_paned_pack2(GTK_PANED(vpaned), scrolled_window, TRUE, FALSE);

  self->model = GTK_TREE_MODEL(gtk_list_store_new(NUM_COLUMNS,
                                                  G_TYPE_STRING, // COLUMN_NAME
                                                  G_TYPE_STRING, // COLUMN_RGB_IN
                                                  G_TYPE_STRING, // COLUMN_LAB_IN
                                                  G_TYPE_STRING, // COLUMN_LAB_REF
                                                  G_TYPE_STRING, // COLUMN_DE_1976
                                                  G_TYPE_FLOAT,  // COLUMN_DE_1976_FLOAT
                                                  G_TYPE_STRING, // COLUMN_DE_2000
                                                  G_TYPE_FLOAT   // COLUMN_DE_2000_FLOAT
                                                  ));
  self->treeview = gtk_tree_view_new_with_model(self->model);
  gtk_tree_view_set_search_column(GTK_TREE_VIEW(self->treeview), COLUMN_NAME);
  gtk_container_add(GTK_CONTAINER(scrolled_window), self->treeview);

  add_column(GTK_TREE_VIEW(self->treeview), "name", COLUMN_NAME, COLUMN_NAME);
  add_column(GTK_TREE_VIEW(self->treeview), "sRGB (image)", COLUMN_RGB_IN, COLUMN_RGB_IN);
  add_column(GTK_TREE_VIEW(self->treeview), "Lab (image)", COLUMN_LAB_IN, COLUMN_LAB_IN);
  add_column(GTK_TREE_VIEW(self->treeview), "Lab (reference)", COLUMN_LAB_REF, COLUMN_LAB_REF);
  add_column(GTK_TREE_VIEW(self->treeview), "deltaE (1976)", COLUMN_DE_1976, COLUMN_DE_1976_FLOAT);
  add_column(GTK_TREE_VIEW(self->treeview), "deltaE (2000)", COLUMN_DE_2000, COLUMN_DE_2000_FLOAT);

  return scrolled_window;
}

int main(int argc, char *argv[])
{
#ifdef _OPENMP
  omp_set_num_threads(omp_get_num_procs());
#endif

  gtk_init(&argc, &argv);

  if(argc > 4 || (argc == 2 && !strcmp(argv[1], "--help")))
  {
    fprintf(stderr, "Usage: %s [<input Lab pfm file>] [<cht file>] [<reference cgats/it8 or Lab pfm file>]\n",
            argv[0]);
    exit(1);
  }

  dt_lut_t *self = (dt_lut_t *)calloc(1, sizeof(dt_lut_t));

  self->picked_source_patches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free);

  char *source_filename = argc >= 2 ? argv[1] : NULL;
  char *cht_filename = argc >= 3 ? argv[2] : NULL;
  char *it8_filename = NULL;
  char *reference_filename = NULL;
  if(argc >= 4)
  {
    char *upper_string = g_ascii_strup(argv[3], -1);
    if(g_str_has_suffix(upper_string, ".PFM"))
      reference_filename = argv[3];
    else
      it8_filename = argv[3];
    g_free(upper_string);
  }

  // build the GUI
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  self->window = window;
  gtk_window_set_title(GTK_WINDOW(window), "darktable LUT tool");
  gtk_container_set_border_width(GTK_CONTAINER(window), 3);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  g_signal_connect(GTK_WINDOW(window), "destroy", G_CALLBACK(gtk_main_quit), NULL);

  // resizeable container
  GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_container_add(GTK_CONTAINER(window), vpaned);

  // upper half
  gtk_paned_pack1(GTK_PANED(vpaned), create_notebook(self), TRUE, FALSE);

  // lower half
  gtk_paned_pack2(GTK_PANED(vpaned), create_table(self), TRUE, FALSE);

  gtk_widget_set_sensitive(self->cht_button, FALSE);
  gtk_widget_set_sensitive(self->it8_button, FALSE);
  gtk_widget_set_sensitive(self->reference_image_button, FALSE);
  gtk_widget_set_sensitive(self->process_button, FALSE);
  gtk_widget_set_sensitive(self->export_button, FALSE);
  gtk_widget_set_sensitive(self->export_raw_button, FALSE);

  gtk_widget_show_all(window);

  // only load data now so it can fill widgets
  if(source_filename && open_source_image(self, source_filename))
  {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(self->image_button), source_filename);
    if(cht_filename && open_cht(self, cht_filename))
    {
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(self->cht_button), cht_filename);
      if(it8_filename && open_it8(self, it8_filename))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(self->it8_button), it8_filename);
      if(reference_filename && open_reference_image(self, reference_filename))
      {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(self->reference_image_button), reference_filename);
        gtk_combo_box_set_active(GTK_COMBO_BOX(self->reference_mode), 1);
      }
    }
  }

  gtk_main();

  free(self->tonecurve_encoded);
  free(self->colorchecker_encoded);
  g_object_unref(self->model);
  free_image(&self->source);
  free_image(&self->reference);
  free_chart(self->chart);
  g_hash_table_unref(self->picked_source_patches);
  free(self);

  return 0;
}

static void add_column(GtkTreeView *treeview, const char *title, int column_id, int sort_column)
{
  GtkCellRenderer *renderer;
  GtkTreeViewColumn *column;
  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(title, renderer, "text", column_id, NULL);
  gtk_tree_view_column_set_sort_column_id(column, sort_column);
  gtk_tree_view_append_column(treeview, column);
}

// only change the numbers, don't re-fill the table!
static void update_table(dt_lut_t *self)
{
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(self->model, &iter);

  while(valid)
  {
    char *name;

    gtk_tree_model_get(self->model, &iter, COLUMN_NAME, &name, -1);

    box_t *box = (box_t *)g_hash_table_lookup(self->chart->box_table, name);
    if(box)
    {
      float Lab[3] = { 0 };
      char *s_Lab_in, *s_RGB_in, *s_deltaE_1976, *s_deltaE_2000;
      float deltaE_1976 = 0.0, deltaE_2000 = 0.0;

      get_Lab_from_box(box, Lab);

      box_t *patch = (box_t *)g_hash_table_lookup(self->picked_source_patches, name);
      if(patch)
      {
        float in_Lab[3];
        get_Lab_from_box(patch, in_Lab);
        s_RGB_in = g_strdup_printf("%d; %d; %d", (int)(patch->rgb[0] * 255 + 0.5),
                                   (int)(patch->rgb[1] * 255 + 0.5), (int)(patch->rgb[2] * 255 + 0.5));
        s_Lab_in = g_strdup_printf("%.02f; %.02f; %.02f", in_Lab[0], in_Lab[1], in_Lab[2]);
        deltaE_1976 = dt_colorspaces_deltaE_1976(in_Lab, Lab);
        deltaE_2000 = dt_colorspaces_deltaE_2000(in_Lab, Lab);
        s_deltaE_1976 = g_strdup_printf("%.02f", deltaE_1976);
        s_deltaE_2000 = g_strdup_printf("%.02f", deltaE_2000);
      }
      else
      {
        s_Lab_in = g_strdup("?");
        s_RGB_in = g_strdup("?");
        s_deltaE_1976 = g_strdup("-");
        s_deltaE_2000 = g_strdup("-");
      }
      char *s_Lab_ref = g_strdup_printf("%.02f; %.02f; %.02f", Lab[0], Lab[1], Lab[2]);

      gtk_list_store_set(GTK_LIST_STORE(self->model), &iter, COLUMN_RGB_IN, s_RGB_in, COLUMN_LAB_IN, s_Lab_in,
                         COLUMN_LAB_REF, s_Lab_ref, COLUMN_DE_1976, s_deltaE_1976, COLUMN_DE_1976_FLOAT,
                         deltaE_1976, COLUMN_DE_2000, s_deltaE_2000, COLUMN_DE_2000_FLOAT, deltaE_2000, -1);
      g_free(s_RGB_in);
      g_free(s_Lab_in);
      g_free(s_Lab_ref);
      g_free(s_deltaE_1976);
      g_free(s_deltaE_2000);
    } // if(box)

    g_free(name);
    valid = gtk_tree_model_iter_next(self->model, &iter);
  } // while(valid)
}

static void get_Lab_from_box(box_t *box, float *Lab)
{
  switch(box->color_space)
  {
    case DT_COLORSPACE_XYZ:
    {
      float XYZ[3];
      for(int i = 0; i < 3; i++) XYZ[i] = box->color[i] * 0.01;
      dt_XYZ_to_Lab(XYZ, Lab);
      break;
    }
    case DT_COLORSPACE_LAB:
      for(int i = 0; i < 3; i++) Lab[i] = box->color[i];
      break;
    default:
      break;
  }
}

static void init_table(dt_lut_t *self)
{
  GtkTreeIter iter;

  gtk_list_store_clear(GTK_LIST_STORE(self->model));

  if(!self->chart) return;

  GList *patch_names = g_hash_table_get_keys(self->chart->box_table);
  patch_names = g_list_sort(patch_names, (GCompareFunc)g_strcmp0);
  for(GList *name = patch_names; name; name = g_list_next(name))
  {
    gtk_list_store_append(GTK_LIST_STORE(self->model), &iter);
    gtk_list_store_set(GTK_LIST_STORE(self->model), &iter, COLUMN_NAME, (char *)name->data, -1);
  }
  g_list_free(patch_names);

  update_table(self);
}

static void collect_source_patches(dt_lut_t *self)
{
  if(self->chart) g_hash_table_foreach(self->chart->box_table, collect_source_patches_foreach, self);
}

static void collect_reference_patches(dt_lut_t *self)
{
  if(self->chart) g_hash_table_foreach(self->chart->box_table, collect_reference_patches_foreach, self);
}

static void collect_source_patches_foreach(gpointer key, gpointer value, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  box_t *box = (box_t *)value;
  float xyz[3] /*, lab[3], srgb[3]*/;

  box_t *patch = find_patch(self->picked_source_patches, key);

  get_xyz_sample_from_image(&self->source, box, xyz);

  set_color(patch, DT_COLORSPACE_XYZ, xyz[0] * 100.0, xyz[1] * 100.0, xyz[2] * 100.0);
}

static void collect_reference_patches_foreach(gpointer key, gpointer value, gpointer user_data)
{
  dt_lut_t *self = (dt_lut_t *)user_data;
  box_t *patch = (box_t *)value;
  float xyz[3];

  get_xyz_sample_from_image(&self->reference, patch, xyz);

  set_color(patch, DT_COLORSPACE_XYZ, xyz[0] * 100.0, xyz[1] * 100.0, xyz[2] * 100.0);
}

static box_t *find_patch(GHashTable *table, gpointer key)
{
  box_t *patch = (box_t *)g_hash_table_lookup(table, key);
  if(!patch)
  {
    // the patch won't be found in the first pass
    patch = (box_t *)calloc(1, sizeof(box_t));
    g_hash_table_insert(table, g_strdup(key), patch);
  }
  return patch;
}

static void get_xyz_sample_from_image(const image_t *const image, box_t *box, float *xyz)
{
  point_t bb[4];
  point_t corners[4];
  box_t inner_box;
  int x_start, y_start, x_end, y_end;

  xyz[0] = xyz[1] = xyz[2] = 0.0;

  if(!box) return;

  get_boundingbox(image, bb);
  inner_box = get_sample_box(*(image->chart), box);
  get_corners(bb, &inner_box, corners);
  get_pixel_region(image, corners, &x_start, &y_start, &x_end, &y_end);

  float delta_x_top = corners[TOP_RIGHT].x - corners[TOP_LEFT].x;
  float delta_y_top = corners[TOP_RIGHT].y - corners[TOP_LEFT].y;
  float delta_x_bottom = corners[BOTTOM_RIGHT].x - corners[BOTTOM_LEFT].x;
  float delta_y_bottom = corners[BOTTOM_RIGHT].y - corners[BOTTOM_LEFT].y;
  float delta_x_left = corners[BOTTOM_LEFT].x - corners[TOP_LEFT].x;
  float delta_y_left = corners[BOTTOM_LEFT].y - corners[TOP_LEFT].y;
  float delta_x_right = corners[BOTTOM_RIGHT].x - corners[TOP_RIGHT].x;
  float delta_y_right = corners[BOTTOM_RIGHT].y - corners[TOP_RIGHT].y;

  double sample_x = 0.0, sample_y = 0.0, sample_z = 0.0;
  size_t n_samples = 0;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)                                                           \
    shared(corners, x_start, y_start, x_end, y_end, delta_x_top, delta_y_top, delta_x_bottom, delta_y_bottom,     \
           delta_x_left, delta_y_left, delta_x_right,                                                             \
           delta_y_right) reduction(+ : n_samples, sample_x, sample_y, sample_z)
#endif
  for(int y = y_start; y < y_end; y++)
    for(int x = x_start; x < x_end; x++)
    {
      if((x - corners[TOP_LEFT].x) / delta_x_top * delta_y_top + corners[TOP_LEFT].y < y
         && (x - corners[BOTTOM_LEFT].x) / delta_x_bottom * delta_y_bottom + corners[BOTTOM_LEFT].y > y
         && (y - corners[TOP_LEFT].y) / delta_y_left * delta_x_left + corners[TOP_LEFT].x < x
         && (y - corners[TOP_RIGHT].y) / delta_y_right * delta_x_right + corners[TOP_RIGHT].x > x)
      {
        float *pixel = &image->xyz[(x + y * image->width) * 3];
        sample_x += pixel[0];
        sample_y += pixel[1];
        sample_z += pixel[2];
        n_samples++;
      }
    }

  xyz[0] = sample_x / n_samples;
  xyz[1] = sample_y / n_samples;
  xyz[2] = sample_z / n_samples;
}

static void get_boundingbox(const image_t *const image, point_t *bb)
{
  for(int i = 0; i < 4; i++)
  {
    bb[i].x = image->bb[i].x * image->width;
    bb[i].y = image->bb[i].y * image->height;
  }
}

static box_t get_sample_box(chart_t *chart, box_t *outer_box)
{
  box_t inner_box = *outer_box;
  float x_shrink = chart->box_shrink / chart->bb_w, y_shrink = chart->box_shrink / chart->bb_h;
  inner_box.p.x += x_shrink;
  inner_box.p.y += y_shrink;
  inner_box.w -= 2.0 * x_shrink;
  inner_box.h -= 2.0 * y_shrink;
  return inner_box;
}

static void get_corners(point_t *bb, box_t *box, point_t *corners)
{
  corners[TOP_LEFT] = corners[TOP_RIGHT] = corners[BOTTOM_RIGHT] = corners[BOTTOM_LEFT] = box->p;
  corners[TOP_RIGHT].x += box->w;
  corners[BOTTOM_RIGHT].x += box->w;
  corners[BOTTOM_RIGHT].y += box->h;
  corners[BOTTOM_LEFT].y += box->h;

  for(int i = 0; i < 4; i++) corners[i] = transform_coords(corners[i], bb);
}

static void get_pixel_region(const image_t *const image, const point_t *const corners, int *x_start, int *y_start,
                             int *x_end, int *y_end)
{
  *x_start = CLAMP((int)(MIN(corners[TOP_LEFT].x,
                             MIN(corners[TOP_RIGHT].x, MIN(corners[BOTTOM_RIGHT].x, corners[BOTTOM_LEFT].x)))
                         + 0.5),
                   0, image->width);
  *x_end = CLAMP((int)(MAX(corners[TOP_LEFT].x,
                           MAX(corners[TOP_RIGHT].x, MAX(corners[BOTTOM_RIGHT].x, corners[BOTTOM_LEFT].x)))
                       + 0.5),
                 0, image->width);
  *y_start = CLAMP((int)(MIN(corners[TOP_LEFT].y,
                             MIN(corners[TOP_RIGHT].y, MIN(corners[BOTTOM_RIGHT].y, corners[BOTTOM_LEFT].y)))
                         + 0.5),
                   0, image->height);
  *y_end = CLAMP((int)(MAX(corners[TOP_LEFT].y,
                           MAX(corners[TOP_RIGHT].y, MAX(corners[BOTTOM_RIGHT].y, corners[BOTTOM_LEFT].y)))
                       + 0.5),
                 0, image->height);
}

static void reset_bb(image_t *image)
{
  image->bb[TOP_LEFT].x = 0.05;
  image->bb[TOP_LEFT].y = 0.05;
  image->bb[TOP_RIGHT].x = 0.95;
  image->bb[TOP_RIGHT].y = 0.05;
  image->bb[BOTTOM_RIGHT].x = 0.95;
  image->bb[BOTTOM_RIGHT].y = 0.95;
  image->bb[BOTTOM_LEFT].x = 0.05;
  image->bb[BOTTOM_LEFT].y = 0.95;
}

static void init_image(dt_lut_t *self, image_t *image, GCallback motion_cb)
{
  memset(image, 0x00, sizeof(image_t));
  image->chart = &self->chart;
  image->drawing_area = gtk_drawing_area_new();
  gtk_widget_set_size_request(image->drawing_area, -1, 50);
  gtk_widget_add_events(image->drawing_area,
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(image->drawing_area, "size-allocate", G_CALLBACK(size_allocate_callback), image);
  g_signal_connect(image->drawing_area, "draw", G_CALLBACK(draw_image_callback), image);
  g_signal_connect(image->drawing_area, "motion-notify-event", G_CALLBACK(motion_cb), self);
}

static void free_image(image_t *image)
{
  if(!image) return;
  reset_bb(image);
  if(image->image) cairo_pattern_destroy(image->image);
  if(image->surface) cairo_surface_destroy(image->surface);
  free(image->xyz);
  image->image = NULL;
  image->surface = NULL;
  image->xyz = NULL;
}

static void image_lab_to_xyz(float *image, const int width, const int height)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(image)
#endif
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      float *pixel = &image[(x + y * width) * 3];
      dt_Lab_to_XYZ(pixel, pixel);
    }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;