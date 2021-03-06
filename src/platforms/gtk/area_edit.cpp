/*
 * SPDX-FileCopyrightText: 2008 Till Harbaum <till@harbaum.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "area_edit.h"

#include <gps_state.h>
#include <misc.h>
#include <notifications.h>
#include <settings.h>

#include "osm-gps-map.h"
#include "osm-gps-map-osd-select.h"

#include "osm2go_annotations.h"
#include <osm2go_cpp.h>
#include "osm2go_i18n.h"
#include "osm2go_platform.h"
#include "osm2go_platform_gtk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>

#define TAB_LABEL_MAP    "Map"
#define TAB_LABEL_DIRECT "Direct"
#define TAB_LABEL_EXTENT "Extent"

/* maemo5 just got maemo mapper */
#ifdef FREMANTLE
#include "dbus.h"
#define HAS_MAEMO_MAPPER
#endif

/* limit of square kilometers above the warning is enabled */
/* should be integral so it can be compared with the preprocessor */
#define WARN_OVER 5

namespace {

/**
 * @brief parse a latitude value from an input widget
 * @param widget the input widget
 * @param lat storage for the parsed value
 * @returns if the input was a valid latitude value
 *
 * If the function returns false, lat will not be modified.
 */
bool pos_lat_get(GtkWidget *widget, pos_float_t &lat)
{
  gchar *endch;
  const char *p = gtk_entry_get_text(GTK_ENTRY(widget));
  pos_float_t t = g_strtod(p, &endch);
  if (*endch != '\0')
    return false;
  bool ret = pos_lat_valid(t);
  if(ret)
    lat = t;
  return ret;
}

void table_attach(GtkTable *table, GtkWidget *widget, int x, int y)
{
  gtk_table_attach_defaults(table, widget, x, x + 1, y, y + 1);
}

void pos_lat_entry_set(GtkWidget *entry, pos_float_t lat)
{
  char str[32];
  pos_lat_str(str, sizeof(str), lat);
  gtk_entry_set_text(GTK_ENTRY(entry), str);
}

void pos_lon_entry_set(GtkWidget *entry, pos_float_t lon)
{
  char str[32];
  pos_lon_str(str, sizeof(str), lon);
  gtk_entry_set_text(GTK_ENTRY(entry), str);
}

/**
 * @brief parse a longitude value from an input widget
 * @param widget the input widget
 * @param lon storage for the parsed value
 * @returns if the input was a valid longitude value
 *
 * If the function returns false, lon will not be modified.
 */
bool pos_lon_get(GtkWidget *widget, pos_float_t &lon)
{
  gchar *endch;
  const char *p = gtk_entry_get_text(GTK_ENTRY(widget));
  pos_float_t t = g_strtod(p, &endch);
  if (*endch != '\0')
    return false;
  bool ret = pos_lon_valid(t);
  if(ret)
    lon = t;
  return ret;
}

void mark(GtkWidget *widget, bool valid)
{
  gtk_widget_set_state(widget, valid ? GTK_STATE_NORMAL : GTK_STATE_PRELIGHT);
}

void callback_modified_lat(GtkWidget *widget)
{
  pos_float_t tmp;
  mark(widget, pos_lat_get(widget, tmp));
}

/* a entry that is colored red when being "active" */
GtkWidget *pos_lat_entry_new(pos_float_t lat)
{
  GtkWidget *widget = osm2go_platform::entry_new();
  gtk_widget_modify_text(widget, GTK_STATE_PRELIGHT, osm2go_platform::invalid_text_color());

  pos_lat_entry_set(widget, lat);

  g_signal_connect(widget, "changed", G_CALLBACK(callback_modified_lat), nullptr);

  return widget;
}

void callback_modified_lon(GtkWidget *widget)
{
  pos_float_t tmp;
  mark(widget, pos_lon_get(widget, tmp));
}

/* a entry that is colored red when filled with invalid coordinate */
GtkWidget *pos_lon_entry_new(pos_float_t lon)
{
  GtkWidget *widget = osm2go_platform::entry_new();
  gtk_widget_modify_text(widget, GTK_STATE_PRELIGHT, osm2go_platform::invalid_text_color());

  pos_lon_entry_set(widget, lon);

  g_signal_connect(widget, "changed", G_CALLBACK(callback_modified_lon), nullptr);

  return widget;
}

void pos_dist_entry_set(GtkWidget *entry, pos_float_t dist, bool is_mil)
{
  char str[32] = "---";
  if(!std::isnan(dist)) {
    /* is this to be displayed as miles? */
    if(is_mil) dist /= KMPMIL;  // kilometer per mile

    snprintf(str, sizeof(str), "%.4f", dist);
    remove_trailing_zeroes(str);
  }
  gtk_entry_set_text(GTK_ENTRY(entry), str);
}

pos_float_t pos_dist_get(GtkWidget *widget, bool is_mil)
{
  const gchar *p = gtk_entry_get_text(GTK_ENTRY(widget));
  return g_strtod(p, nullptr) * (is_mil?KMPMIL:1.0);
}

struct area_context_t {
  explicit area_context_t(area_edit_t &a, GtkWidget *dlg);
  area_context_t() O2G_DELETED_FUNCTION;
  area_context_t(const area_context_t &) O2G_DELETED_FUNCTION;
  area_context_t &operator=(const area_context_t &) O2G_DELETED_FUNCTION;
#if __cplusplus >= 201103L
  area_context_t(area_context_t &&) = delete;
  area_context_t &operator=(area_context_t &&) = delete;
  ~area_context_t() = default;
#endif

  osm2go_platform::DialogGuard dialog;
  GtkWidget * const notebook;
  area_edit_t &area;
  pos_area bounds;      ///< local copy to work on
  GtkWidget *warning;

  struct {
    GtkWidget *minlat, *maxlat, *minlon, *maxlon;
    GtkWidget *error;
  } direct;

  struct {
    GtkWidget *lat, *lon, *height, *width, *mil_km;
    bool is_mil;
    GtkWidget *error;
  } extent;

#ifdef HAS_MAEMO_MAPPER
  struct {
    GtkWidget *fetch;
  } mmapper;
#endif

  struct {
    OsmGpsMap *widget;
    bool needs_redraw;
    OsmGpsMapPoint start;
  } map;
};

area_context_t::area_context_t(area_edit_t &a, GtkWidget *dlg)
  : dialog(dlg)
  , notebook(osm2go_platform::notebook_new())
  , area(a)
  , bounds(a.bounds)
  , warning(nullptr)
{
  memset(&direct, 0, sizeof(direct));
  memset(&extent, 0, sizeof(extent));
#ifdef HAS_MAEMO_MAPPER
  mmapper.fetch = nullptr;
#endif
  memset(&map, 0, sizeof(map));
}

/**
 * @brief calculate the selected area in square kilometers
 */
double
selected_area(const area_context_t *context)
{
  pos_float_t center_lat = context->bounds.centerLat();
  double vscale = DEG2RAD(POS_EQ_RADIUS / 1000.0);
  double hscale = DEG2RAD(cos(DEG2RAD(center_lat)) * POS_EQ_RADIUS / 1000.0);

  return vscale * context->bounds.latDist() *
         hscale * context->bounds.lonDist();
}

bool
current_tab_is(GtkNotebook *nb, GtkWidget *w, const char *str)
{
  const char *name = gtk_notebook_get_tab_label_text(nb, w);

  return (strcmp(name, static_cast<const char *>(_(str))) == 0);
}

bool
current_tab_is(GtkNotebook *nb, int page_num, const char *str)
{
  return current_tab_is(nb, gtk_notebook_get_nth_page(nb, page_num), str);
}

bool
current_tab_is(area_context_t *context, const char *str)
{
  GtkNotebook *nb = osm2go_platform::notebook_get_gtk_notebook(context->notebook);

  gint page_num = gtk_notebook_get_current_page(nb);

  if(page_num < 0)
    return false;

  return current_tab_is(nb, page_num, str);
}

inline trstring
warn_text(double area, bool imperial_units)
{
  if (unlikely(imperial_units))
    return trstring("The currently selected area is %1 mi² in size. "
                    "This is more than the recommended %2 mi².\n\n"
                    "Continuing may result in a big or failing download and low "
                    "mapping performance in a densly mapped area (e.g. cities)!")
                    .arg(area / (KMPMIL * KMPMIL), 0, 'f', 2)
                    .arg(static_cast<float>(WARN_OVER) / (KMPMIL * KMPMIL), 0, 'f', 2);
  else
    return trstring("The currently selected area is %1 km² in size. "
                    "This is more than the recommended %2 km².\n\n"
                    "Continuing may result in a big or failing download and low "
                    "mapping performance in a densly mapped area (e.g. cities)!")
                    .arg(area, 0, 'f', 2)
// performance microoptimization, just because we can
#if WARN_OVER == 5
                    .arg("5.0");
#else
#error assumed optimization now incorrect, please remove the other code
                    .arg(WARN_OVER, 0, 'f', 2);
#endif
}

void
on_area_warning_clicked(area_context_t *context)
{
  double area = selected_area(context);

  warning_dlg(warn_text(area, context->extent.is_mil), context->dialog.get());
}

bool
area_warning(area_context_t *context)
{
  bool ret = true;

  /* check if area size exceeds recommended values */
  double area = selected_area(context);

  if(area > WARN_OVER) {
    ret = osm2go_platform::yes_no(_("Area size warning!"),
                                  trstring("%1\n\nDo you really want to continue?").arg(warn_text(area, context->extent.is_mil)),
                                  MISC_AGAIN_ID_AREA_TOO_BIG | MISC_AGAIN_FLAG_DONT_SAVE_NO, context->dialog.get());
  }

  return ret;
}

void area_main_update(area_context_t *context)
{
  /* also setup the local error messages here, so they are */
  /* updated for all entries at once */
  gboolean sensitive;
  if(!context->bounds.valid()) {
    sensitive = FALSE;
  } else if(!context->bounds.normalized()) {
    gtk_label_set_text(GTK_LABEL(context->direct.error),
                       _("\"From\" must be smaller than \"to\" value!"));
    gtk_label_set_text(GTK_LABEL(context->extent.error),
                       _("Extents must be positive!"));
    sensitive = FALSE;
  } else {
    gtk_label_set_text(GTK_LABEL(context->direct.error), "");
    gtk_label_set_text(GTK_LABEL(context->extent.error), "");

    sensitive = TRUE;
  }
  gtk_dialog_set_response_sensitive(context->dialog, GTK_RESPONSE_ACCEPT, sensitive);

  /* check if area size exceeds recommended values */
  if(selected_area(context) > WARN_OVER)
    gtk_widget_show(context->warning);
  else
    gtk_widget_hide(context->warning);
}

std::pair<OsmGpsMapPoint, OsmGpsMapPoint>
pos_rad_box(OsmGpsMapPoint start, OsmGpsMapPoint end)
{
  return std::pair<OsmGpsMapPoint, OsmGpsMapPoint>(start, end);
}

std::pair<OsmGpsMapPoint, OsmGpsMapPoint>
pos_box(const pos_area &b)
{
  OsmGpsMapPoint start, end;

  start.rlat = DEG2RAD(b.min.lat);
  start.rlon = DEG2RAD(b.min.lon);
  end.rlat = DEG2RAD(b.max.lat);
  end.rlon = DEG2RAD(b.max.lon);

  return pos_rad_box(start, end);
}

struct add_bounds {
  OsmGpsMap * const map;
  explicit add_bounds(OsmGpsMap *m) : map(m) {}
  inline void operator()(const pos_area &b) const
  {
    osm_gps_map_add_bounds(map, pos_box(b));
  }
};

/* the contents of the map tab have been changed */
void map_update(area_context_t *context, bool forced)
{
  /* map is first tab (page 0) */
  if(!forced && !current_tab_is(context, TAB_LABEL_MAP)) {
    g_debug("schedule map redraw");
    context->map.needs_redraw = true;
    return;
  }

  g_debug("do map redraw");

  OsmGpsMapPoint none = { 0, 0 };
  std::pair<OsmGpsMapPoint, OsmGpsMapPoint> boundtrack(none, none);
  /* check if the position is invalid */
  pos_t pos;
  int zoom;
  if(!context->bounds.valid()) {
    /* no coordinates given: display around the current GPS position if available */
    pos = context->area.gps_state->get_pos();
    if(pos.valid()) {
      zoom = 12;
    } else {
      /* no GPS position available: display the entire world */
      pos.lat = 0.0;
      pos.lon = 0.0;
      zoom = 1;
    }
  } else {
    pos = context->bounds.center();

    /* we know the widgets pixel size, we know the required real size, */
    /* we want the zoom! */
    GtkWidget *wd = GTK_WIDGET(context->map.widget);
    double vzoom = wd->allocation.height / context->bounds.latDist();
    double hzoom = wd->allocation.width  / context->bounds.lonDist();

    /* use smallest zoom, so everything fits on screen */
    zoom = log2((45.0 / 32.0) * std::min(vzoom, hzoom)) - 1;

    /* ---------- draw border (as a gps track) -------------- */
    if(context->bounds.normalized())
      boundtrack = pos_box(context->bounds);
  }
  osm_gps_map_add_track(context->map.widget, boundtrack);

  osm_gps_map_set_center_and_zoom(context->map.widget, pos.lat, pos.lon, zoom);

  // show all other bounds
  std::for_each(context->area.other_bounds.begin(), context->area.other_bounds.end(),
                add_bounds(context->map.widget));

  context->map.needs_redraw = false;
}

gboolean on_map_configure(area_context_t *context)
{
  map_update(context, false);
  return FALSE;
}

/* the contents of the direct tab have been changed */
void direct_update(area_context_t *context)
{
  pos_lat_entry_set(context->direct.minlat, context->bounds.min.lat);
  pos_lon_entry_set(context->direct.minlon, context->bounds.min.lon);
  pos_lat_entry_set(context->direct.maxlat, context->bounds.max.lat);
  pos_lon_entry_set(context->direct.maxlon, context->bounds.max.lon);
}

/* update the contents of the extent tab */
void extent_update(area_context_t *context)
{
  pos_float_t center_lat = context->bounds.centerLat();
  pos_float_t center_lon = context->bounds.centerLon();

  pos_lat_entry_set(context->extent.lat, center_lat);
  pos_lat_entry_set(context->extent.lon, center_lon);

  double vscale = DEG2RAD(POS_EQ_RADIUS / 1000.0);
  double hscale = DEG2RAD(cos(DEG2RAD(center_lat)) * POS_EQ_RADIUS / 1000.0);

  double height = vscale * context->bounds.latDist();
  double width  = hscale * context->bounds.lonDist();

  pos_dist_entry_set(context->extent.width, width, context->extent.is_mil);
  pos_dist_entry_set(context->extent.height, height, context->extent.is_mil);
}

void callback_modified_direct(area_context_t *context)
{
  /* direct is second tab (page 1) */
  if(!current_tab_is(context, TAB_LABEL_DIRECT))
    return;

  /* parse the fields from the direct entry pad */
  if(unlikely(!pos_lat_get(context->direct.minlat, context->bounds.min.lat) ||
              !pos_lon_get(context->direct.minlon, context->bounds.min.lon) ||
              !pos_lat_get(context->direct.maxlat, context->bounds.max.lat) ||
              !pos_lon_get(context->direct.maxlon, context->bounds.max.lon)))
    return;

  area_main_update(context);

  /* also adjust other views */
  extent_update(context);
  map_update(context, false);
}

void callback_modified_extent(area_context_t *context)
{
  /* extent is third tab (page 2) */
  if(!current_tab_is(context, TAB_LABEL_EXTENT))
    return;

  pos_float_t center_lat, center_lon;
  if(unlikely(!pos_lat_get(context->extent.lat, center_lat) ||
                !pos_lon_get(context->extent.lon, center_lon)))
    return;

  double vscale = DEG2RAD(POS_EQ_RADIUS / 1000.0);
  double hscale = DEG2RAD(cos(DEG2RAD(center_lat)) * POS_EQ_RADIUS / 1000.0);

  double height = pos_dist_get(context->extent.height, context->extent.is_mil);
  double width  = pos_dist_get(context->extent.width, context->extent.is_mil);

  height /= 2 * vscale;
  context->bounds.min.lat = center_lat - height;
  context->bounds.max.lat = center_lat + height;

  width /= 2 * hscale;
  context->bounds.min.lon = center_lon - width;
  context->bounds.max.lon = center_lon + width;

  area_main_update(context);

  /* also update other tabs */
  direct_update(context);
  map_update(context, false);
}

void callback_modified_unit(area_context_t *context)
{
  /* get current values */
  double height = pos_dist_get(context->extent.height, context->extent.is_mil);
  double width  = pos_dist_get(context->extent.width, context->extent.is_mil);

  /* adjust unit flag */
  context->extent.is_mil = (osm2go_platform::combo_box_get_active(context->extent.mil_km) == 0);

  /* save values */
  pos_dist_entry_set(context->extent.width, width, context->extent.is_mil);
  pos_dist_entry_set(context->extent.height, height, context->extent.is_mil);
}

#ifdef HAS_MAEMO_MAPPER
void callback_fetch_mm_clicked(area_context_t *context)
{
  dbus_mm_pos_t mmpos;
  if(!dbus_mm_set_position(&mmpos)) {
    error_dlg(_("Unable to communicate with Maemo Mapper. "
              "You need to have Maemo Mapper installed to use this feature."),
              context->dialog.get());
    return;
  }

  if(!mmpos.valid) {
    error_dlg(_("No valid position received yet. You need to "
           "scroll or zoom the Maemo Mapper view in order to force it to send "
           "its current view position to osm2go."), context->dialog.get());
    return;
  }

  /* maemo mapper is fourth tab (page 3) */
  if(!current_tab_is(context, "M.Mapper"))
    return;

  /* maemo mapper pos data ... */
  pos_float_t center_lat = mmpos.pos.lat;
  pos_float_t center_lon = mmpos.pos.lon;
  int zoom = mmpos.zoom;

  if(!pos_lat_valid(center_lat) || !pos_lon_valid(center_lon))
    return;

  double vscale = DEG2RAD(POS_EQ_RADIUS);
  double height = 8 * (1<<zoom) / vscale;
  context->bounds.min.lat = center_lat - height;
  context->bounds.max.lat = center_lat + height;

  double hscale = DEG2RAD(cos(DEG2RAD(center_lat)) * POS_EQ_RADIUS);
  double width  = 16 * (1<<zoom) / hscale;
  context->bounds.min.lon = center_lon - width;
  context->bounds.max.lon = center_lon + width;

  area_main_update(context);

  /* also update other tabs */
  direct_update(context);
  extent_update(context);
  map_update(context, false);
}
#endif

gboolean
on_map_button_press_event(GtkWidget *widget, GdkEventButton *event, area_context_t *context)
{
  OsmGpsMap *map = OSM_GPS_MAP(widget);

  /* osm-gps-map needs this event to handle the OSD */
  if(osm_gps_map_osd_check(map, event->x, event->y) != OSD_NONE)
    return FALSE;

  if(osm_gps_map_osd_get_state(map) == TRUE)
    return FALSE;

  /* remove existing marker */
  OsmGpsMapPoint none = { 0, 0 };
  osm_gps_map_add_track(map, std::pair<OsmGpsMapPoint, OsmGpsMapPoint>(none, none));

  /* and remember this location as the start */
  context->map.start = osm_gps_map_convert_screen_to_geographic(map, event->x, event->y);

  return TRUE;
}

gboolean
on_map_motion_notify_event(GtkWidget *widget, GdkEventMotion  *event, area_context_t *context)
{
  OsmGpsMap *map = OSM_GPS_MAP(widget);

  if(!std::isnan(context->map.start.rlon) &&
     !std::isnan(context->map.start.rlat)) {
    OsmGpsMapPoint end = osm_gps_map_convert_screen_to_geographic(map, event->x, event->y);

    osm_gps_map_add_track(map, pos_rad_box(context->map.start, end));
  }

  /* returning true here disables dragging in osm-gps-map */
  return osm_gps_map_osd_get_state(map) == TRUE ? FALSE : TRUE;
}

gboolean
on_map_button_release_event(GtkWidget *widget, GdkEventButton *event, area_context_t *context)
{
  OsmGpsMap *map = OSM_GPS_MAP(widget);

  if(!std::isnan(context->map.start.rlon) &&
     !std::isnan(context->map.start.rlat)) {

    OsmGpsMapPoint start = context->map.start;
    OsmGpsMapPoint end = osm_gps_map_convert_screen_to_geographic(map, event->x, event->y);

    osm_gps_map_add_track(map, pos_rad_box(start, end));

    context->bounds = pos_area::normalized(pos_t(RAD2DEG(start.rlat), RAD2DEG(start.rlon)),
                                           pos_t(RAD2DEG(end.rlat),   RAD2DEG(end.rlon)));

    area_main_update(context);
    direct_update(context);
    extent_update(context);

    context->map.start.rlon = context->map.start.rlat = NAN;
  }

  /* osm-gps-map needs this event to handle the OSD */
  if(osm_gps_map_osd_check(map, event->x, event->y) != OSD_NONE)
    return FALSE;

  /* returning true here disables dragging in osm-gps-map */
  return osm_gps_map_osd_get_state(map) == TRUE ? FALSE : TRUE;
}

/* updating the map while the user manually changes some coordinates */
/* may confuse the map. so we delay those updates until the map tab */
/* is becoming visible */
void on_page_switch(GtkNotebook *nb, GtkWidget *pg, guint pgnum, area_context_t *context)
{
  if(!context->map.needs_redraw)
    return;

#ifdef FREMANTLE
  // the pages of the normal notebook are not used on FREMANTLE, so the sender
  // widget is not the one that can be queried for the actual title
# define SWITCH_PARAM pgnum
# define UNUSED_SWITCH_PARAM pg
#else
# define SWITCH_PARAM pg
# define UNUSED_SWITCH_PARAM pgnum
#endif
  (void)UNUSED_SWITCH_PARAM;
  if (!current_tab_is(nb, SWITCH_PARAM, TAB_LABEL_MAP))
    return;
#undef UNUSED_SWITCH_PARAM
#undef SWITCH_PARAM

  map_update(context, true);
}

gboolean map_gps_update(gpointer data)
{
  area_context_t *context = static_cast<area_context_t *>(data);

  pos_t pos = context->area.gps_state->get_pos();

  if(pos.valid())
    osm_gps_map_gps_add(context->map.widget, pos.lat, pos.lon, NAN);
  else
    osm_gps_map_gps_clear(context->map.widget);

  return TRUE;
}

} // namespace

area_edit_t::area_edit_t(gps_state_t *gps, pos_area &b, osm2go_platform::Widget *dlg)
  : gps_state(gps)
  , parent(dlg)
  , bounds(b)
{
}

bool area_edit_t::run() {
  GtkWidget *vbox;

  area_context_t context(*this,
                         gtk_dialog_new_with_buttons(static_cast<const gchar *>(_("Area editor")),
                                               GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                               GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                               nullptr));

  osm2go_platform::dialog_size_hint(context.dialog, osm2go_platform::MISC_DIALOG_HIGH);
  context.warning = gtk_dialog_add_button(context.dialog, _("Warning"), GTK_RESPONSE_HELP);

  gtk_button_set_image(GTK_BUTTON(context.warning),
                       gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_BUTTON));
  g_signal_connect_swapped(context.warning, "clicked",
                           G_CALLBACK(on_area_warning_clicked), &context);

  /* ------------- fetch from map ------------------------ */

  context.map.needs_redraw = false;
  context.map.widget = OSM_GPS_MAP(g_object_new(OSM_TYPE_GPS_MAP,
                                                nullptr));

  g_signal_connect_swapped(context.map.widget, "configure-event",
                           G_CALLBACK(on_map_configure), &context);
  g_signal_connect(context.map.widget, "button-press-event",
                   G_CALLBACK(on_map_button_press_event), &context);
  g_signal_connect(context.map.widget, "motion-notify-event",
                   G_CALLBACK(on_map_motion_notify_event), &context);
  g_signal_connect(context.map.widget, "button-release-event",
                   G_CALLBACK(on_map_button_release_event), &context);

  /* install handler for timed updates of the gps button */
  osm2go_platform::Timer timer;
  timer.restart(1, map_gps_update, &context);
  context.map.start.rlon = context.map.start.rlat = NAN;

  osm2go_platform::notebook_append_page(context.notebook, GTK_WIDGET(context.map.widget), _(TAB_LABEL_MAP));

  /* ------------ direct min/max edit --------------- */

  vbox = gtk_vbox_new(FALSE, 10);

  guint tableColumns = 5;
  GtkTable *table = GTK_TABLE(gtk_table_new(3, tableColumns, FALSE));
  gtk_table_set_col_spacings(table, 10);
  gtk_table_set_row_spacings(table, 5);

  context.direct.minlat = pos_lat_entry_new(bounds.min.lat);
  table_attach(table, context.direct.minlat, 0, 0);
  GtkWidget *label = gtk_label_new(_("° to"));
  table_attach(table,  label, 1, 0);
  context.direct.maxlat = pos_lat_entry_new(bounds.max.lat);
  table_attach(table, context.direct.maxlat, 2, 0);
  label = gtk_label_new(_("°"));
  table_attach(table,  label, tableColumns - 1, 0);

  context.direct.minlon = pos_lon_entry_new(bounds.min.lon);
  table_attach(table, context.direct.minlon, 0, 1);
  label = gtk_label_new(_("° to"));
  table_attach(table,  label, 1, 1);
  context.direct.maxlon = pos_lon_entry_new(bounds.max.lon);
  table_attach(table, context.direct.maxlon, 2, 1);
  label = gtk_label_new(_("°"));
  table_attach(table,  label, tableColumns - 1, 1);

  /* setup this page */
  g_signal_connect_swapped(context.direct.minlat, "changed",
                           G_CALLBACK(callback_modified_direct), &context);
  g_signal_connect_swapped(context.direct.minlon, "changed",
                           G_CALLBACK(callback_modified_direct), &context);
  g_signal_connect_swapped(context.direct.maxlat, "changed",
                           G_CALLBACK(callback_modified_direct), &context);
  g_signal_connect_swapped(context.direct.maxlon, "changed",
                           G_CALLBACK(callback_modified_direct), &context);

  /* --- hint --- */
  label = gtk_label_new(_("(recommended min/max diff <0.03 degrees)"));
  gtk_table_attach_defaults(table, label, 0, tableColumns - 1, 2, 3);

  const GdkColor *color = osm2go_platform::invalid_text_color();
  /* error label */
  context.direct.error = gtk_label_new(nullptr);
  gtk_widget_modify_fg(context.direct.error, GTK_STATE_NORMAL, color);
  gtk_table_attach_defaults(table, context.direct.error, 0, tableColumns - 1, 3, 4);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(table), FALSE, FALSE, 0);
  osm2go_platform::notebook_append_page(context.notebook, vbox, _(TAB_LABEL_DIRECT));

  /* ------------- center/extent edit ------------------------ */

  vbox = gtk_vbox_new(FALSE, 10);
  table = GTK_TABLE(gtk_table_new(3, 5, FALSE));  // x, y
  gtk_table_set_col_spacings(table, 10);
  gtk_table_set_row_spacings(table, 5);

  label = gtk_label_new(_("Center:"));
  gtk_misc_set_alignment(GTK_MISC(label), 1.f, 0.5f);
  gtk_table_attach_defaults(table, label, 0, 1, 0, 1);
  context.extent.lat = pos_lat_entry_new(0.0);
  gtk_table_attach_defaults(table, context.extent.lat, 1, 2, 0, 1);
  gtk_table_attach_defaults(table, gtk_label_new(_("°")), 2, 3, 0, 1);

  context.extent.lon = pos_lon_entry_new(0.0);
  gtk_table_attach_defaults(table, context.extent.lon, 3, 4, 0, 1);
  gtk_table_attach_defaults(table, gtk_label_new(_("°")), 4, 5, 0, 1);

  gtk_table_set_row_spacing(table, 0, 10);

  label = gtk_label_new(_("Width:"));
  gtk_misc_set_alignment(GTK_MISC(label), 1.f, 0.5f);
  gtk_table_attach_defaults(table, label, 0, 1, 1, 2);
  context.extent.width = osm2go_platform::entry_new();
  gtk_table_attach_defaults(table, context.extent.width, 1, 2, 1, 2);

  label = gtk_label_new(_("Height:"));
  gtk_misc_set_alignment(GTK_MISC(label), 1.f, 0.5f);
  gtk_table_attach_defaults(table,  label, 0, 1, 2, 3);
  context.extent.height = osm2go_platform::entry_new();
  gtk_table_attach_defaults(table, context.extent.height, 1, 2, 2, 3);

  settings_t::ref settings = settings_t::instance();
  context.extent.is_mil = settings->imperial_units;
  std::vector<trstring::native_type> units(2);
  units[0] = _("mi");
  units[1] = _("km");
  context.extent.mil_km = osm2go_platform::combo_box_new(_("Unit"), units,
                                                         context.extent.is_mil ? 0 : 1);

  gtk_table_attach(table, context.extent.mil_km, 3, 4, 1, 3,
		   static_cast<GtkAttachOptions>(0), static_cast<GtkAttachOptions>(0),
		   0, 0);

  /* setup this page */
  extent_update(&context);

  /* connect signals after inital update to avoid confusion */
  g_signal_connect_swapped(context.extent.lat, "changed",
                           G_CALLBACK(callback_modified_extent), &context);
  g_signal_connect_swapped(context.extent.lon, "changed",
                           G_CALLBACK(callback_modified_extent), &context);
  g_signal_connect_swapped(context.extent.width, "changed",
                           G_CALLBACK(callback_modified_extent), &context);
  g_signal_connect_swapped(context.extent.height, "changed",
                           G_CALLBACK(callback_modified_extent), &context);
  g_signal_connect_swapped(context.extent.mil_km, "changed",
                           G_CALLBACK(callback_modified_unit), &context);

  /* --- hint --- */
  label = gtk_label_new(_("(recommended width/height < 2km/1.25mi)"));
  gtk_table_attach_defaults(table, label, 0, 3, 3, 4);

  /* error label */
  context.extent.error = gtk_label_new(nullptr);
  gtk_widget_modify_fg(context.extent.error, GTK_STATE_NORMAL, color);
  gtk_table_attach_defaults(table, context.extent.error, 0, 3, 4, 5);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(table), FALSE, FALSE, 0);
  osm2go_platform::notebook_append_page(context.notebook, vbox, _(TAB_LABEL_EXTENT));

#ifdef HAS_MAEMO_MAPPER
  /* ------------- fetch from maemo mapper ------------------------ */

  vbox = gtk_vbox_new(FALSE, 8);
  context.mmapper.fetch = osm2go_platform::button_new_with_label(_("Get from Maemo Mapper"));
  gtk_box_pack_start(GTK_BOX(vbox), context.mmapper.fetch, FALSE, FALSE, 0);

  g_signal_connect_swapped(context.mmapper.fetch, "clicked",
                           G_CALLBACK(callback_fetch_mm_clicked), &context);

  /* --- hint --- */
  label = gtk_label_new(_("(recommended MM zoom level < 7)"));
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  osm2go_platform::notebook_append_page(context.notebook, vbox, _("M.Mapper"));
#endif

  /* ------------------------------------------------------ */

  gtk_box_pack_start(context.dialog.vbox(), context.notebook, TRUE, TRUE, 0);

  g_signal_connect(osm2go_platform::notebook_get_gtk_notebook(context.notebook),
                   "switch-page", G_CALLBACK(on_page_switch), &context);

  gtk_widget_show_all(context.dialog.get());

  area_main_update(&context);

  bool ok = false;
  int response;
  do {
    response = gtk_dialog_run(context.dialog);

    if(GTK_RESPONSE_ACCEPT == response) {
      if(area_warning(&context)) {
        /* copy modified values back to given storage */
        bounds = context.bounds;
        ok = true;
        break;
      }
    }
  } while(response == GTK_RESPONSE_HELP || response == GTK_RESPONSE_ACCEPT);

  settings->imperial_units = context.extent.is_mil;

  return ok;
}
