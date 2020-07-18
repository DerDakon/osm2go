/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 cino=t0,(0: */
/*
 * osm-gps-map.c
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2009 <john.stowers@gmail.com>
 *
 * Contributions by
 * Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
 *
 * osm-gps-map.c is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * osm-gps-map.c is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "osm-gps-map.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_set>

#include <cairo.h>
#include <gdk/gdk.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "converter.h"
#include "osm-gps-map-osd-select.h"
#include "osm-gps-map-point.h"
#include "osm-gps-map-source.h"
#include "osm-gps-map-types.h"

#include <osm2go_cpp.h>

/* the version check macro is not available in all versions of libsoup */
#if defined(SOUP_CHECK_VERSION)
#if SOUP_CHECK_VERSION(2, 42, 0)
#define USE_SOUP_SESSION_NEW
#endif
#endif

#define ENABLE_DEBUG 0

#define EXTRA_BORDER (TILESIZE / 2)

#define OSM_GPS_MAP_SCROLL_STEP 10

#ifdef FREMANTLE
#include <hildon/hildon-defines.h>
/* only maemo devices up to version 4 have a fullscreen button */
#define OSM_GPS_MAP_KEY_FULLSCREEN  'f'
#define OSM_GPS_MAP_KEY_ZOOMIN      HILDON_HARDKEY_INCREASE
#define OSM_GPS_MAP_KEY_ZOOMOUT     HILDON_HARDKEY_DECREASE
#else
#define OSM_GPS_MAP_KEY_FULLSCREEN  GDK_F11
#define OSM_GPS_MAP_KEY_ZOOMIN      '+'
#define OSM_GPS_MAP_KEY_ZOOMOUT     '-'
#endif

#include <gdk/gdkkeysyms.h>

#define USER_AGENT "OSM2go " VERSION " (https://github.com/osm2go/osm2go)"

#if !GLIB_CHECK_VERSION(2,28,0)
static void g_slist_free_full(GSList *list, GDestroyNotify free_func)
{
  g_slist_foreach(list, (GFunc) free_func, nullptr);
  g_slist_free(list);
}
#endif

#define MAX_TILE_CACHE_SIZE 20
//for customizing the redering of the gps track
#define UI_GPS_TRACK_WIDTH 4
#define UI_GPS_POINT_INNER_RADIUS 10

struct _OsmGpsMapPrivate
{
    GHashTable *tile_queue;
    std::unordered_set<uint64_t> *missing_tiles;
    GHashTable *tile_cache;

    int map_zoom;
    int max_zoom;
    int min_zoom;
    int map_x;
    int map_y;

    /* Latitude and longitude of the center of the map, in radians */
    gfloat center_rlat;
    gfloat center_rlon;

    /* Incremented at each redraw */
    guint redraw_cycle;
    /* ID of the idle redraw operation */
    guint idle_map_redraw;

    //how we download tiles
    SoupSession *soup_session;

    //contains flags indicating the various special characters
    //the uri string contains, that will be replaced when calculating
    //the uri to download.
    const char *repo_uri;
    const char *image_format;

    //gps tracking state
    OsmGpsMapPoint gps;
    float gps_heading;

    //the osd controls (if present)
    struct osd_priv_s *osd;
    GdkPixmap *dbuf_pixmap;

    //additional images or tracks added to the map
    GSList *tracks;
    GSList *bounds;

    //Used for storing the joined tiles
    GdkPixmap *pixmap;
    GdkGC *gc_map;

    //For tracking click and drag
    int drag_counter;
    int drag_mouse_dx;
    int drag_mouse_dy;
    int drag_start_mouse_x;
    int drag_start_mouse_y;
    int drag_start_map_x;
    int drag_start_map_y;
    guint drag_expose;

    bool gps_valid;
#ifdef OSM_GPS_MAP_KEY_FULLSCREEN
    bool fullscreen;
#endif

    gboolean is_disposed : 2;
    gboolean dragging : 2;
};

#define OSM_GPS_MAP_PRIVATE(o)  (OSM_GPS_MAP (o)->priv)

typedef struct
{
    guint64 hash;
    GdkPixbuf *pixbuf;
    /* We keep track of the number of the redraw cycle this tile was last used,
     * so that osm_gps_map_purge_cache() can remove the older ones */
    guint redraw_cycle;
} OsmCachedTile;

enum
{
    PROP_0,

    PROP_MAP_X,
    PROP_MAP_Y
};

#if !GLIB_CHECK_VERSION(2,38,0)
G_DEFINE_TYPE (OsmGpsMap, osm_gps_map, GTK_TYPE_DRAWING_AREA);
#else
G_DEFINE_TYPE_WITH_PRIVATE (OsmGpsMap, osm_gps_map, GTK_TYPE_DRAWING_AREA);
#endif

namespace {

typedef struct {
    /* The details of the tile to download */
    char *uri;
    OsmGpsMap *map;
    guint64 hashkey;
} tile_download_t;

/*
 * Drawing function forward defintions
 */
void osm_gps_map_map_redraw_idle (OsmGpsMap *map);

void
cached_tile_free (OsmCachedTile *tile)
{
    g_object_unref (tile->pixbuf);
    g_slice_free (OsmCachedTile, tile);
}

/*
 * Description:
 *   Find and replace text within a string.
 *
 * Parameters:
 *   src  (in) - pointer to source string
 *   from (in) - pointer to search text
 *   to   (in) - pointer to replacement text
 *
 * Returns:
 *   Returns a pointer to dynamically-allocated memory containing string
 *   with first occurence of the text pointed to by 'from' replaced by with the
 *   text pointed to by 'to'.
 */
gchar *
replace_string(gchar *src, const gchar *from, const gchar *to)
{
    size_t fromlen = strlen(from);
    size_t tolen   = strlen(to);

    /* Try to find the search text. */
    const char *match = strstr(src, from);
    assert(match != nullptr);

    /* Allocate the destination buffer. */
    size_t size  = strlen(src);
    gchar *value = static_cast<gchar *>(g_realloc(src, size + 1 + tolen - fromlen));

    if (G_UNLIKELY(value == nullptr)) {
      g_free(src);
      return nullptr;
    }

    /* We need to return 'value', so let's make a copy to mess around with. */
    gchar *dst = value;

    /* Find out how many characters to copy up to the 'match'. */
    size_t count = match - src;

    /*
     * Nothing to do to the point where we matched. Then
     * move the source pointer ahead by that amount. And
     * move the destination pointer ahead by the same amount.
     */
    dst += count;

    /*
     * Now move the remainder of the string to make room for the replacement.
     * If pattern and replacement have the same length, do nothing.
     */
    if (tolen != fromlen)
      memmove(dst + tolen, dst + fromlen, size - count - fromlen);

    /* Now copy in the replacement text 'to' at the position of
     * the match. */
    memcpy(dst, to, tolen);

    return value;
}

gchar *
replace_map_uri(const gchar *uri, int zoom, int x, int y)
{
    gchar *url = g_strdup(uri);

    char s[16];

    g_snprintf(s, sizeof(s), "%d", x);
    url = replace_string(url, URI_MARKER_X, s);

    g_snprintf(s, sizeof(s), "%d", y);
    url = replace_string(url, URI_MARKER_Y, s);

    g_snprintf(s, sizeof(s), "%d", zoom);
    url = replace_string(url, URI_MARKER_Z, s);

    return url;
}

void
my_log_handler (const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
    if (!(log_level & G_LOG_LEVEL_DEBUG) || ENABLE_DEBUG)
        g_log_default_handler (log_domain, log_level, message, user_data);
}

/* clears the tracks and all resources */
void
osm_gps_map_free_tracks (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->tracks)
    {
        GSList* tmp = priv->tracks;
        while (tmp != nullptr)
        {
            g_slist_free_full(static_cast<GSList *>(tmp->data), g_free);
            tmp = g_slist_next(tmp);
        }
        g_slist_free(priv->tracks);
        priv->tracks = nullptr;
    }
}

/* clears the bounds and all resources */
void
osm_gps_map_free_bounds(OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->bounds)
    {
        GSList* tmp = priv->bounds;
        while (tmp != nullptr)
        {
            g_slist_free_full(static_cast<GSList *>(tmp->data), g_free);
            tmp = g_slist_next(tmp);
        }
        g_slist_free(priv->bounds);
        priv->bounds = nullptr;
    }
}

void
osm_gps_map_draw_gps_point (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    //incase we get called before we have got a gps point
    if (priv->gps_valid) {
        int x, y;
        int r = UI_GPS_POINT_INNER_RADIUS;
        int mr = 3*r;

        int map_x0 = priv->map_x - EXTRA_BORDER;
        int map_y0 = priv->map_y - EXTRA_BORDER;
        x = lon2pixel(priv->map_zoom, priv->gps.rlon) - map_x0;
        y = lat2pixel(priv->map_zoom, priv->gps.rlat) - map_y0;

        cairo_t *cr = gdk_cairo_create(priv->pixmap);

        // draw ball gradient
        if (r > 0) {
            // draw direction arrow
            if(!std::isnan(priv->gps_heading))
            {
                cairo_move_to (cr, x-r*cos(priv->gps_heading), y-r*sin(priv->gps_heading));
                cairo_line_to (cr, x+3*r*sin(priv->gps_heading), y-3*r*cos(priv->gps_heading));
                cairo_line_to (cr, x+r*cos(priv->gps_heading), y+r*sin(priv->gps_heading));
                cairo_close_path (cr);

                cairo_set_source_rgba (cr, 0.3, 0.3, 1.0, 0.5);
                cairo_fill_preserve (cr);

                cairo_set_line_width (cr, 1.0);
                cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5);
                cairo_stroke(cr);
            }

            cairo_pattern_t *pat = cairo_pattern_create_radial (x-(r/5), y-(r/5), (r/5), x,  y, r);
            cairo_pattern_add_color_stop_rgba (pat, 0, 1, 1, 1, 1.0);
            cairo_pattern_add_color_stop_rgba (pat, 1, 0, 0, 1, 1.0);
            cairo_set_source (cr, pat);
            cairo_arc (cr, x, y, r, 0, 2 * M_PI);
            cairo_fill (cr);
            cairo_pattern_destroy (pat);
            // draw ball border
            cairo_set_line_width (cr, 1.0);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
            cairo_arc (cr, x, y, r, 0, 2 * M_PI);
            cairo_stroke(cr);
        }

        cairo_destroy(cr);
        gtk_widget_queue_draw_area (GTK_WIDGET(map),
                                    x-mr,
                                    y-mr,
                                    mr*2,
                                    mr*2);
    }
}

void
osm_gps_map_blit_tile(OsmGpsMap *map, GdkPixbuf *pixbuf, int offset_x, int offset_y)
{
    OsmGpsMapPrivate *priv = map->priv;

    g_debug("Queing redraw @ %d,%d (w:%d h:%d)", offset_x,offset_y, TILESIZE,TILESIZE);

    /* draw pixbuf onto pixmap */
    gdk_draw_pixbuf (priv->pixmap,
                     priv->gc_map,
                     pixbuf,
                     0,0,
                     offset_x,offset_y,
                     TILESIZE,TILESIZE,
                     GDK_RGB_DITHER_NONE, 0, 0);
}

void
osm_gps_map_tile_download_complete (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
    tile_download_t *dl = (tile_download_t *)user_data;

    if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
        OsmGpsMap *map = OSM_GPS_MAP(dl->map);
        OsmGpsMapPrivate *priv = map->priv;

        /* parse file directly from memory */
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type (priv->image_format, nullptr);
        if (!gdk_pixbuf_loader_write (loader, (unsigned char*)msg->response_body->data, msg->response_body->length, nullptr))
        {
            g_warning("Error: Decoding of image failed");
        }
        gdk_pixbuf_loader_close(loader, nullptr);

        GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

        /* give up loader but keep the pixbuf */
        g_object_ref(pixbuf);
        g_object_unref(loader);

        /* Store the tile into the cache */
        if (G_LIKELY (pixbuf))
        {
            OsmCachedTile *tile = g_slice_new (OsmCachedTile);
            tile->hash = dl->hashkey;
            tile->pixbuf = pixbuf;
            tile->redraw_cycle = priv->redraw_cycle;
            /* if the tile is already in the cache (it could be one
             * rendered from another zoom level), it will be
             * overwritten */
            g_hash_table_insert (priv->tile_cache, &tile->hash, tile);
        }
        osm_gps_map_map_redraw_idle (map);

        g_hash_table_remove(priv->tile_queue, &dl->hashkey);

        g_free(dl->uri);
        g_free(dl);
    }
    else
    {
        g_warning("Error downloading tile: %d - %s", msg->status_code, msg->reason_phrase);
        if (msg->status_code == SOUP_STATUS_NOT_FOUND)
        {
            OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(dl->map);

            priv->missing_tiles->insert(dl->hashkey);
            g_hash_table_remove(priv->tile_queue, &dl->hashkey);
        }
        else if (msg->status_code == SOUP_STATUS_CANCELLED)
        {
            ;//application exiting
        }
        else
        {
            soup_session_requeue_message(session, msg);
            return;
        }
        g_free(dl->uri);
        g_free(dl);
    }
}

guint64
tile_hash(guint zoom, guint x, guint y)
{
  guint64 ret = zoom; // zoom is 1..19, so it fits in 5 bits. Move it to top byte
  ret <<= 56;

  // for the other coordinates they would use 2**zoom bits, so just distribute them
  // evenly in the remaining space
  ret |= (((guint64)y) << 28);
  ret |= x;

  return ret;
}

void
osm_gps_map_download_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;

    //calculate the uri to download
    guint64 hashkey = tile_hash(zoom, x, y);

    //check the tile has not already been queued for download,
    //or has been attempted, and its missing
    if (g_hash_table_lookup_extended(priv->tile_queue, &hashkey, nullptr, nullptr) ||
        priv->missing_tiles->find(hashkey) != priv->missing_tiles->end())
    {
        g_debug("Tile already downloading (or missing)");
    } else {
        tile_download_t *dl = g_new(tile_download_t, 1);
        dl->hashkey = hashkey;
        dl->uri = replace_map_uri(priv->repo_uri, zoom, x, y);
        dl->map = map;

        g_debug("Download tile: %d,%d z:%d\n\t%s", x, y, zoom, dl->uri);

        SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, dl->uri);
        if (msg) {
            g_hash_table_insert (priv->tile_queue, &dl->hashkey, msg);
            soup_session_queue_message (priv->soup_session, msg, osm_gps_map_tile_download_complete, dl);
        } else {
            g_warning("Could not create soup message");
            g_free(dl->uri);
            g_free(dl);
        }
    }
}

GdkPixbuf *
osm_gps_map_load_cached_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    GdkPixbuf *pixbuf = nullptr;

    guint64 hash = tile_hash(zoom, x, y);
    OsmCachedTile *tile = static_cast<OsmCachedTile *>(g_hash_table_lookup(priv->tile_cache, &hash));

    /* set/update the redraw_cycle timestamp on the tile */
    if (tile)
    {
        tile->redraw_cycle = priv->redraw_cycle;
        pixbuf = static_cast<GdkPixbuf *>(g_object_ref(tile->pixbuf));
    }

    return pixbuf;
}

GdkPixbuf *
osm_gps_map_find_bigger_tile (OsmGpsMap *map, int zoom, int x, int y,
                              int *zoom_found)
{
    if (zoom == 0) return nullptr;

    int next_zoom = zoom - 1;
    int next_x = x / 2;
    int next_y = y / 2;

    GdkPixbuf *pixbuf = osm_gps_map_load_cached_tile (map, next_zoom, next_x, next_y);
    if (pixbuf)
        *zoom_found = next_zoom;
    else
        pixbuf = osm_gps_map_find_bigger_tile (map, next_zoom, next_x, next_y,
                                               zoom_found);
    return pixbuf;
}

GdkPixbuf *
osm_gps_map_render_missing_tile_upscaled (OsmGpsMap *map, int zoom,
                                          int x, int y)
{
    int zoom_big;

    GdkPixbuf *big = osm_gps_map_find_bigger_tile (map, zoom, x, y, &zoom_big);
    if (!big) return nullptr;

    g_debug ("Found bigger tile (zoom = %d, wanted = %d)", zoom_big, zoom);

    /* get a Pixbuf for the area to magnify */
    int zoom_diff = zoom - zoom_big;
    int area_size = TILESIZE >> zoom_diff;
    if (area_size == 0)
      return nullptr;
    int modulo = 1 << zoom_diff;
    int area_x = (x % modulo) * area_size;
    int area_y = (y % modulo) * area_size;
    GdkPixbuf *area = gdk_pixbuf_new_subpixbuf (big, area_x, area_y,
                                     area_size, area_size);
    g_object_unref (big);
    GdkPixbuf *pixbuf = gdk_pixbuf_scale_simple (area, TILESIZE, TILESIZE,
                                      GDK_INTERP_NEAREST);
    g_object_unref (area);
    return pixbuf;
}

GdkPixbuf *
osm_gps_map_render_missing_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    /* maybe TODO: render from downscaled tiles, if the following fails */
    return osm_gps_map_render_missing_tile_upscaled (map, zoom, x, y);
}

void
osm_gps_map_load_tile (OsmGpsMap *map, int zoom, int x, int y, int offset_x, int offset_y)
{
    OsmGpsMapPrivate *priv = map->priv;

    g_debug("Load tile %d,%d (%d,%d) z:%d", x, y, offset_x, offset_y, zoom);

    /* try to get file from internal cache first */
    GdkPixbuf *pixbuf = osm_gps_map_load_cached_tile(map, zoom, x, y);

    if(pixbuf)
    {
        osm_gps_map_blit_tile(map, pixbuf, offset_x,offset_y);
        g_object_unref (pixbuf);
    }
    else
    {
        osm_gps_map_download_tile(map, zoom, x, y);

        /* try to render the tile by scaling cached tiles from other zoom
         * levels */
        pixbuf = osm_gps_map_render_missing_tile (map, zoom, x, y);
        if (pixbuf)
        {
            gdk_draw_pixbuf (priv->pixmap,
                             priv->gc_map,
                             pixbuf,
                             0,0,
                             offset_x,offset_y,
                             TILESIZE,TILESIZE,
                             GDK_RGB_DITHER_NONE, 0, 0);
            g_object_unref (pixbuf);
        }
        else
        {
            //prevent some artifacts when drawing not yet loaded areas.
            gdk_draw_rectangle (priv->pixmap,
                                GTK_WIDGET(map)->style->white_gc,
                                TRUE, offset_x, offset_y, TILESIZE, TILESIZE);
        }
    }
}

void
osm_gps_map_fill_tiles_pixel (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    g_debug("Fill tiles: %d,%d z:%d", priv->map_x, priv->map_y, priv->map_zoom);

    int offset_x = - priv->map_x % TILESIZE;
    int offset_y = - priv->map_y % TILESIZE;
    if (offset_x > 0) offset_x -= TILESIZE;
    if (offset_y > 0) offset_y -= TILESIZE;

    int offset_xn = offset_x + EXTRA_BORDER;
    int offset_yn = offset_y + EXTRA_BORDER;

    int width  = GTK_WIDGET(map)->allocation.width;
    int height = GTK_WIDGET(map)->allocation.height;

    int tiles_nx = (width  - offset_x) / TILESIZE + 1;
    int tiles_ny = (height - offset_y) / TILESIZE + 1;

    int tile_x0 =  floorf((float)priv->map_x / (float)TILESIZE);
    int tile_y0 =  floorf((float)priv->map_y / (float)TILESIZE);

    //TODO: implement wrap around
    for (int i=tile_x0; i<(tile_x0+tiles_nx);i++)
    {
        for (int j=tile_y0;  j<(tile_y0+tiles_ny); j++)
        {
            if( j<0 || i<0 || i>=exp(priv->map_zoom * M_LN2) || j>=exp(priv->map_zoom * M_LN2))
            {
                gdk_draw_rectangle (priv->pixmap,
                                    GTK_WIDGET(map)->style->white_gc,
                                    TRUE,
                                    offset_xn, offset_yn,
                                    TILESIZE,TILESIZE);
            }
            else
            {
                osm_gps_map_load_tile(map,
                                      priv->map_zoom,
                                      i,j,
                                      offset_xn,offset_yn);
            }
            offset_yn += TILESIZE;
        }
        offset_xn += TILESIZE;
        offset_yn = offset_y + EXTRA_BORDER;
    }
}

void
osm_gps_map_print_track(OsmGpsMap *map, GSList *trackpoint_list,
                        unsigned short r, unsigned short g, unsigned short b,
                        int lw)
{
    OsmGpsMapPrivate *priv = map->priv;

    int min_x = 0,min_y = 0,max_x = 0,max_y = 0;

    cairo_t *cr = gdk_cairo_create(priv->pixmap);
    cairo_set_line_width (cr, lw);
    cairo_set_source_rgba (cr, r/65535.0, g/65535.0, b/65535.0, 0.6);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

    int map_x0 = priv->map_x - EXTRA_BORDER;
    int map_y0 = priv->map_y - EXTRA_BORDER;
    for(const GSList *list = trackpoint_list; list != nullptr; list = list->next)
    {
        const OsmGpsMapPoint *tp = static_cast<const OsmGpsMapPoint *>(list->data);

        int x = lon2pixel(priv->map_zoom, tp->rlon) - map_x0;
        int y = lat2pixel(priv->map_zoom, tp->rlat) - map_y0;

        // first time through loop
        if (list == trackpoint_list) {
            cairo_move_to(cr, x, y);
        }

        cairo_line_to(cr, x, y);

        max_x = MAX(x,max_x);
        min_x = MIN(x,min_x);
        max_y = MAX(y,max_y);
        min_y = MIN(y,min_y);
    }

    gtk_widget_queue_draw_area (
                                GTK_WIDGET(map),
                                min_x - lw,
                                min_y - lw,
                                max_x + (lw * 2),
                                max_y + (lw * 2));

    cairo_stroke(cr);
    cairo_destroy(cr);
}

/* Prints the gps trip history, and any other tracks */
void
osm_gps_map_print_tracks (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    const unsigned short r = 60000;
    const unsigned short g = 0;
    const unsigned short b = 0;

    if (priv->tracks)
    {
        GSList* tmp = priv->tracks;
        while (tmp != nullptr)
        {
            osm_gps_map_print_track(map, static_cast<GSList *>(tmp->data), r, g, b, UI_GPS_TRACK_WIDTH);
            tmp = g_slist_next(tmp);
        }
    }
}

/* Prints the bound rectangles */
void
osm_gps_map_print_bounds(OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    const unsigned short r = 0x64 * 256;
    const unsigned short g = 0x7d * 256;
    const unsigned short b = 0xab * 256;

    GSList* tmp = priv->bounds;
    while (tmp != nullptr)
    {
        osm_gps_map_print_track(map, static_cast<GSList *>(tmp->data), r, g, b, UI_GPS_TRACK_WIDTH / 2);
        tmp = g_slist_next(tmp);
    }
}

gboolean
osm_gps_map_purge_cache_check(G_GNUC_UNUSED gpointer key, gpointer value, gpointer user)
{
   return (((OsmCachedTile*)value)->redraw_cycle < GPOINTER_TO_UINT(user));
}

void
osm_gps_map_purge_cache (OsmGpsMap *map)
{
   OsmGpsMapPrivate *priv = map->priv;

   if (g_hash_table_size (priv->tile_cache) < MAX_TILE_CACHE_SIZE)
       return;

   /* run through the cache, and remove the tiles which have not been used
    * during the last redraw operation */
   g_hash_table_foreach_remove(priv->tile_cache, osm_gps_map_purge_cache_check,
                               GUINT_TO_POINTER(priv->redraw_cycle - MAX_TILE_CACHE_SIZE / 2));
}

gboolean
osm_gps_map_map_redraw (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    /* on diablo the map comes up at 1x1 pixel size and */
    /* isn't really usable. we'll just ignore this ... */
    if((GTK_WIDGET(map)->allocation.width < 2) ||
       (GTK_WIDGET(map)->allocation.height < 2)) {
        printf("not a useful sized map yet ...\n");
        return FALSE;
    }

    priv->idle_map_redraw = 0;

#ifdef DRAG_DEBUG
    printf("trying redraw\n");
#endif

    /* the motion_notify handler uses priv->pixmap to redraw the area; if we
     * change it while we are dragging, we will end up showing it in the wrong
     * place. This could be fixed by carefully recompute the coordinates, but
     * for now it's easier just to disable redrawing the map while dragging */
    if (priv->dragging)
        return FALSE;

    /* undo all offsets that may have happened when dragging */
    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;

    priv->redraw_cycle++;

    /* draw white background to initialise pixmap */
    gdk_draw_rectangle (priv->pixmap,
                        GTK_WIDGET(map)->style->white_gc,
                        TRUE,
                        0, 0,
                        GTK_WIDGET(map)->allocation.width + EXTRA_BORDER * 2,
                        GTK_WIDGET(map)->allocation.height + EXTRA_BORDER * 2);

    osm_gps_map_fill_tiles_pixel(map);

    osm_gps_map_print_bounds(map);
    osm_gps_map_print_tracks(map);
    osm_gps_map_draw_gps_point(map);

    /* OSD may contain a coordinate/scale, so we may have to re-render it */
    osm_gps_map_osd_render(priv->osd);

    osm_gps_map_purge_cache(map);
    gtk_widget_queue_draw (GTK_WIDGET (map));

    return FALSE;
}

void
osm_gps_map_map_redraw_idle (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->idle_map_redraw == 0)
        priv->idle_map_redraw = g_idle_add ((GSourceFunc)osm_gps_map_map_redraw, map);
}

void
center_coord_update(GtkWidget *widget) {
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    // pixel_x,y, offsets
    gint pixel_x = priv->map_x + widget->allocation.width/2;
    gint pixel_y = priv->map_y + widget->allocation.height/2;

    printf("coord update\n");
    priv->center_rlon = pixel2lon(priv->map_zoom, pixel_x);
    priv->center_rlat = pixel2lat(priv->map_zoom, pixel_y);
}

gboolean
on_window_key_press(GtkWidget *widget,
			 GdkEventKey *event, OsmGpsMapPrivate *priv) {
  gboolean handled = FALSE;
  int step = GTK_WIDGET(widget)->allocation.width/OSM_GPS_MAP_SCROLL_STEP;

  // the map handles some keys on its own ...
  switch(event->keyval) {
  case OSM_GPS_MAP_KEY_FULLSCREEN: {
      GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(widget));
      if(!priv->fullscreen)
          gtk_window_fullscreen(GTK_WINDOW(toplevel));
      else
          gtk_window_unfullscreen(GTK_WINDOW(toplevel));

      priv->fullscreen = !priv->fullscreen;
      handled = TRUE;
      } break;

  case OSM_GPS_MAP_KEY_ZOOMIN:
      osm_gps_map_set_zoom(OSM_GPS_MAP(widget), priv->map_zoom+1);
      handled = TRUE;
      break;

  case OSM_GPS_MAP_KEY_ZOOMOUT:
      osm_gps_map_set_zoom(OSM_GPS_MAP(widget), priv->map_zoom-1);
      handled = TRUE;
      break;

  case GDK_Up:
      priv->map_y -= step;
      center_coord_update(widget);
      osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
      handled = TRUE;
      break;

  case GDK_Down:
      priv->map_y += step;
      center_coord_update(widget);
      osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
      handled = TRUE;
      break;

  case GDK_Left:
      priv->map_x -= step;
      center_coord_update(widget);
      osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
      handled = TRUE;
      break;

  case GDK_Right:
      priv->map_x += step;
      center_coord_update(widget);
      osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
      handled = TRUE;
      break;

  default:
      break;
  }

  return handled;
}

} // namespace

static void
osm_gps_map_init (OsmGpsMap *object)
{
    OsmGpsMapPrivate *priv = static_cast<OsmGpsMapPrivate *>(

#if !GLIB_CHECK_VERSION(2,38,0)
      G_TYPE_INSTANCE_GET_PRIVATE (object, OSM_TYPE_GPS_MAP, OsmGpsMapPrivate));
#else
      osm_gps_map_get_instance_private(object));
#endif

    object->priv = priv;

    priv->pixmap = nullptr;

    memset(&priv->gps, 0, sizeof(priv->gps));
    priv->gps_valid = false;
    priv->gps_heading = NAN;

#ifdef OSM_GPS_MAP_BUTTON_FULLSCREEN
    priv->fullscreen = false;
#endif

    priv->tracks = nullptr;
    priv->bounds = nullptr;

    priv->drag_counter = 0;
    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;
    priv->drag_start_mouse_x = 0;
    priv->drag_start_mouse_y = 0;

    //Change number of concurrent connections option?
#ifdef USE_SOUP_SESSION_NEW
    priv->soup_session =
        soup_session_new_with_options(SOUP_SESSION_USER_AGENT,
                                      USER_AGENT, nullptr);
#else
    priv->soup_session =
        soup_session_async_new_with_options(SOUP_SESSION_USER_AGENT,
                                            USER_AGENT, nullptr);
#endif

    const gchar *prx = g_getenv("http_proxy");
    if (prx != nullptr) {
#ifndef G_VALUE_INIT
#define G_VALUE_INIT  { 0, { { 0 } } }
#endif
        GValue val = G_VALUE_INIT;

        SoupURI* uri = soup_uri_new(prx);
        g_value_init(&val, SOUP_TYPE_URI);
        g_value_take_boxed(&val, uri);

        g_object_set_property(G_OBJECT(priv->soup_session),SOUP_SESSION_PROXY_URI,&val);
    }

    //Hash table which maps tile d/l URIs to SoupMessage requests
    priv->tile_queue = g_hash_table_new (g_int64_hash, g_int64_equal);

    priv->missing_tiles = new std::unordered_set<uint64_t>();

    /* memory cache for most recently used tiles */
    priv->tile_cache = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                              nullptr, (GDestroyNotify)cached_tile_free);

    gtk_widget_add_events (GTK_WIDGET (object),
                           GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK |
                           GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    GTK_WIDGET_SET_FLAGS (object, GTK_CAN_FOCUS);

    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MASK, my_log_handler, nullptr);

    g_signal_connect(G_OBJECT(object), "key_press_event",
                     G_CALLBACK(on_window_key_press), priv);

    priv->osd = osm_gps_map_osd_select_init();
}

namespace {

void
osm_gps_map_setup(OsmGpsMapPrivate *priv) {
    //check if the source given is valid
    const gchar *uri = osm_gps_map_source_get_repo_uri(OSM_GPS_MAP_SOURCE_OPENSTREETMAP);
    assert(uri != nullptr);

    priv->repo_uri = uri;
    priv->image_format = osm_gps_map_source_get_image_format(OSM_GPS_MAP_SOURCE_OPENSTREETMAP);
    priv->max_zoom = osm_gps_map_source_get_max_zoom(OSM_GPS_MAP_SOURCE_OPENSTREETMAP);
    priv->min_zoom = osm_gps_map_source_get_min_zoom(OSM_GPS_MAP_SOURCE_OPENSTREETMAP);
}

GObject *
osm_gps_map_constructor (GType gtype, guint n_properties, GObjectConstructParam *properties)
{
    //Always chain up to the parent constructor
    GObject *object =
        G_OBJECT_CLASS(osm_gps_map_parent_class)->constructor(gtype, n_properties, properties);

    osm_gps_map_setup(OSM_GPS_MAP_PRIVATE(object));

    return object;
}

void
osm_gps_map_dispose (GObject *object)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(object);

    if (priv->is_disposed)
        return;

    priv->is_disposed = TRUE;

    soup_session_abort(priv->soup_session);
    g_object_unref(priv->soup_session);

    g_hash_table_destroy(priv->tile_queue);
    delete priv->missing_tiles;
    g_hash_table_destroy(priv->tile_cache);

    if(priv->pixmap)
        g_object_unref (priv->pixmap);

    if(priv->gc_map)
        g_object_unref(priv->gc_map);

    if (priv->idle_map_redraw != 0)
        g_source_remove (priv->idle_map_redraw);

    if (priv->drag_expose != 0)
        g_source_remove (priv->drag_expose);

    if(priv->osd) {
        osm_gps_map_osd_free(priv->osd);
        priv->osd = nullptr;
    }

    if(priv->dbuf_pixmap)
        g_object_unref (priv->dbuf_pixmap);

    G_OBJECT_CLASS (osm_gps_map_parent_class)->dispose (object);
}

void
osm_gps_map_finalize (GObject *object)
{
    OsmGpsMap *map = OSM_GPS_MAP(object);

    osm_gps_map_free_tracks(map);
    osm_gps_map_free_bounds(map);

    G_OBJECT_CLASS (osm_gps_map_parent_class)->finalize (object);
}

void
osm_gps_map_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(object);

    switch (prop_id)
    {
        case PROP_MAP_X:
            priv->map_x = g_value_get_int (value);
            center_coord_update(GTK_WIDGET(object));
            break;
        case PROP_MAP_Y:
            priv->map_y = g_value_get_int (value);
            center_coord_update(GTK_WIDGET(object));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

void
osm_gps_map_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(object);

    switch (prop_id)
    {
        case PROP_MAP_X:
            g_value_set_int(value, priv->map_x);
            break;
        case PROP_MAP_Y:
            g_value_set_int(value, priv->map_y);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

gboolean
osm_gps_map_scroll_event (GtkWidget *widget, GdkEventScroll  *event)
{
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if (event->direction == GDK_SCROLL_UP)
    {
        osm_gps_map_set_zoom(map, priv->map_zoom+1);
    }
    else
    {
        osm_gps_map_set_zoom(map, priv->map_zoom-1);
    }

    return FALSE;
}

gboolean
osm_gps_map_button_press (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    /* pressed inside OSD control? */
    if(priv->osd) {
        osd_button_t but = osm_gps_map_osd_check(OSM_GPS_MAP(widget), event->x, event->y);

        if(but != OSD_NONE)
        {
            int step =
                GTK_WIDGET(widget)->allocation.width/OSM_GPS_MAP_SCROLL_STEP;
            priv->drag_counter = -1;

            switch(but) {
            case OSD_UP:
                priv->map_y -= step;
                center_coord_update(widget);
                g_object_set(G_OBJECT(widget), "auto-center", FALSE, nullptr);
                osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
                break;

            case OSD_DOWN:
                priv->map_y += step;
                center_coord_update(widget);
                g_object_set(G_OBJECT(widget), "auto-center", FALSE, nullptr);
                osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
                break;

            case OSD_LEFT:
                priv->map_x -= step;
                center_coord_update(widget);
                g_object_set(G_OBJECT(widget), "auto-center", FALSE, nullptr);
                osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
                break;

            case OSD_RIGHT:
                priv->map_x += step;
                center_coord_update(widget);
                g_object_set(G_OBJECT(widget), "auto-center", FALSE, nullptr);
                osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
                break;

            case OSD_IN:
                osm_gps_map_set_zoom(OSM_GPS_MAP(widget), priv->map_zoom+1);
                break;

            case OSD_OUT:
                osm_gps_map_set_zoom(OSM_GPS_MAP(widget), priv->map_zoom-1);
                break;

            default:
                break;
            }

            return FALSE;
        }
    }

    priv->drag_counter = 0;
    priv->drag_start_mouse_x = (int) event->x;
    priv->drag_start_mouse_y = (int) event->y;
    priv->drag_start_map_x = priv->map_x;
    priv->drag_start_map_y = priv->map_y;

    return FALSE;
}

gboolean
osm_gps_map_button_release (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    if (priv->dragging)
    {
        priv->dragging = FALSE;

        priv->map_x = priv->drag_start_map_x;
        priv->map_y = priv->drag_start_map_y;

        priv->map_x += (priv->drag_start_mouse_x - (int) event->x);
        priv->map_y += (priv->drag_start_mouse_y - (int) event->y);

        center_coord_update(widget);

        osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
    }
    /* pressed inside OSD control? */
    else if(priv->osd)
        osm_gps_map_osd_check(OSM_GPS_MAP(widget), event->x, event->y);

#ifdef DRAG_DEBUG
    printf("dragging done\n");
#endif

    priv->drag_counter = -1;

    return FALSE;
}

gboolean
osm_gps_map_expose (GtkWidget *widget, GdkEventExpose  *event);

static gboolean
osm_gps_map_map_expose (GtkWidget *widget)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    priv->drag_expose = 0;
    osm_gps_map_expose (widget, nullptr);
    return FALSE;
}

gboolean
osm_gps_map_motion_notify (GtkWidget *widget, GdkEventMotion  *event)
{
    int x, y;
    GdkModifierType state;
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    if (event->is_hint)
        gdk_window_get_pointer (event->window, &x, &y, &state);
    else
    {
        x = event->x;
        y = event->y;
        state = static_cast<GdkModifierType>(event->state);
    }

    // are we being dragged
    if (!(state & GDK_BUTTON1_MASK))
        return FALSE;

    if (priv->drag_counter < 0)
        return FALSE;

    /* not yet dragged far enough? */
    if(!priv->drag_counter &&
       ( (x - priv->drag_start_mouse_x) * (x - priv->drag_start_mouse_x) +
         (y - priv->drag_start_mouse_y) * (y - priv->drag_start_mouse_y) <
         10*10))
        return FALSE;

    priv->drag_counter++;

    priv->dragging = TRUE;

    priv->drag_mouse_dx = x - priv->drag_start_mouse_x;
    priv->drag_mouse_dy = y - priv->drag_start_mouse_y;

    /* instead of redrawing directly just add an idle function */
    if (!priv->drag_expose)
        priv->drag_expose =
            g_idle_add ((GSourceFunc)osm_gps_map_map_expose, widget);

    return FALSE;
}

gboolean
osm_gps_map_configure(GtkWidget *widget, G_GNUC_UNUSED GdkEventConfigure *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    /* create pixmap */
    if (priv->pixmap)
        g_object_unref (priv->pixmap);

    priv->pixmap = gdk_pixmap_new (
                        widget->window,
                        widget->allocation.width + EXTRA_BORDER * 2,
                        widget->allocation.height + EXTRA_BORDER * 2,
                        -1);

    // pixel_x,y, offsets
    gint pixel_x = lon2pixel(priv->map_zoom, priv->center_rlon);
    gint pixel_y = lat2pixel(priv->map_zoom, priv->center_rlat);

    priv->map_x = pixel_x - widget->allocation.width/2;
    priv->map_y = pixel_y - widget->allocation.height/2;

    if (priv->dbuf_pixmap)
        g_object_unref (priv->dbuf_pixmap);

    priv->dbuf_pixmap = gdk_pixmap_new (
                        widget->window,
                        widget->allocation.width,
                        widget->allocation.height,
                        -1);

    /* and gc, used for clipping (I think......) */
    if(priv->gc_map)
        g_object_unref(priv->gc_map);

    priv->gc_map = gdk_gc_new(priv->pixmap);

    osm_gps_map_map_redraw(OSM_GPS_MAP(widget));

    return FALSE;
}

gboolean
osm_gps_map_expose (GtkWidget *widget, GdkEventExpose  *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    GdkDrawable *drawable = priv->dbuf_pixmap;

#ifdef DRAG_DEBUG
    printf("expose, map %d/%d\n", priv->map_x, priv->map_y);
#endif

    if (!priv->drag_mouse_dx && !priv->drag_mouse_dy && event)
    {
#ifdef DRAG_DEBUG
        printf("  dragging = %d, event = %p\n", priv->dragging, event);
#endif

        gdk_draw_drawable (drawable,
                           widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                           priv->pixmap,
                           event->area.x + EXTRA_BORDER, event->area.y + EXTRA_BORDER,
                           event->area.x, event->area.y,
                           event->area.width, event->area.height);
    }
    else
    {
#ifdef DRAG_DEBUG
        printf("  drag_mouse %d/%d\n",
               priv->drag_mouse_dx - EXTRA_BORDER,
               priv->drag_mouse_dy - EXTRA_BORDER);
#endif

        gdk_draw_drawable (drawable,
                           widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                           priv->pixmap,
                           0,0,
                           priv->drag_mouse_dx - EXTRA_BORDER,
                           priv->drag_mouse_dy - EXTRA_BORDER,
                           -1,-1);

        //Paint white outside of the map if dragging. Its less
        //ugly than painting the corrupted map
        if(priv->drag_mouse_dx>EXTRA_BORDER) {
            gdk_draw_rectangle (drawable,
                                widget->style->white_gc,
                                TRUE,
                                0, 0,
                                priv->drag_mouse_dx - EXTRA_BORDER,
                                widget->allocation.height);
        }
        else if (-priv->drag_mouse_dx > EXTRA_BORDER)
        {
            gdk_draw_rectangle (drawable,
                                widget->style->white_gc,
                                TRUE,
                                priv->drag_mouse_dx + widget->allocation.width + EXTRA_BORDER, 0,
                                -priv->drag_mouse_dx - EXTRA_BORDER,
                                widget->allocation.height);
        }

        if (priv->drag_mouse_dy>EXTRA_BORDER) {
            gdk_draw_rectangle (drawable,
                                widget->style->white_gc,
                                TRUE,
                                0, 0,
                                widget->allocation.width,
                                priv->drag_mouse_dy - EXTRA_BORDER);
        }
        else if (-priv->drag_mouse_dy > EXTRA_BORDER)
        {
            gdk_draw_rectangle (drawable,
                                widget->style->white_gc,
                                TRUE,
                                0, priv->drag_mouse_dy + widget->allocation.height + EXTRA_BORDER,
                                widget->allocation.width,
                                -priv->drag_mouse_dy - EXTRA_BORDER);
        }
    }

    /* draw new OSD */
    if(priv->osd)
        osm_gps_map_osd_draw(priv->osd, widget, drawable);

    gdk_draw_drawable (widget->window,
                       widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                       priv->dbuf_pixmap,
                       0,0,0,0,-1,-1);

    return FALSE;
}

} // namespace

static void
osm_gps_map_class_init (OsmGpsMapClass *klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

#if !GLIB_CHECK_VERSION(2,38,0)
    g_type_class_add_private (klass, sizeof (OsmGpsMapPrivate));
#endif

    object_class->dispose = osm_gps_map_dispose;
    object_class->finalize = osm_gps_map_finalize;
    object_class->constructor = osm_gps_map_constructor;
    object_class->set_property = osm_gps_map_set_property;
    object_class->get_property = osm_gps_map_get_property;

    widget_class->expose_event = osm_gps_map_expose;
    widget_class->configure_event = osm_gps_map_configure;
    widget_class->button_press_event = osm_gps_map_button_press;
    widget_class->button_release_event = osm_gps_map_button_release;
    widget_class->motion_notify_event = osm_gps_map_motion_notify;
    widget_class->scroll_event = osm_gps_map_scroll_event;

    g_object_class_install_property (object_class,
                                     PROP_MAP_X,
                                     g_param_spec_int ("map-x",
                                                       "map-x",
                                                       "initial map x location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       890,
                                                       static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY)));

    g_object_class_install_property (object_class,
                                     PROP_MAP_Y,
                                     g_param_spec_int ("map-y",
                                                       "map-y",
                                                       "initial map y location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       515,
                                                       static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY)));
}

const char*
osm_gps_map_source_get_friendly_name(OsmGpsMapSource_t source)
{
    switch(source)
    {
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return "OpenStreetMap";
        default:
            abort();
    }
}

//http://www.internettablettalk.com/forums/showthread.php?t=5209
//https://garage.maemo.org/plugins/scmsvn/viewcvs.php/trunk/src/maps.c?root=maemo-mapper&view=markup
//http://www.ponies.me.uk/maps/GoogleTileUtils.java
//http://www.mgmaps.com/cache/MapTileCacher.perl
const char*
osm_gps_map_source_get_repo_uri(OsmGpsMapSource_t source)
{
    switch(source)
    {
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return OSM_REPO_URI;
        default:
            abort();
    }
}

const char *
osm_gps_map_source_get_image_format(OsmGpsMapSource_t source)
{
    switch(source) {
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return "png";
        default:
            abort();
    }
}


int
osm_gps_map_source_get_min_zoom(G_GNUC_UNUSED OsmGpsMapSource_t source)
{
    return 1;
}

int
osm_gps_map_source_get_max_zoom(OsmGpsMapSource_t source)
{
    switch(source) {
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return OSM_MAX_ZOOM;
        default:
            abort();
    }
}

void
osm_gps_map_set_center_and_zoom (OsmGpsMap *map, float latitude, float longitude, int zoom)
{
    osm_gps_map_set_center (map, latitude, longitude);
    osm_gps_map_set_zoom (map, zoom);
}

void
osm_gps_map_set_center (OsmGpsMap *map, float latitude, float longitude)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    OsmGpsMapPrivate *priv = map->priv;

    priv->center_rlat = deg2rad(latitude);
    priv->center_rlon = deg2rad(longitude);

    // pixel_x,y, offsets
    int pixel_x = lon2pixel(priv->map_zoom, priv->center_rlon);
    int pixel_y = lat2pixel(priv->map_zoom, priv->center_rlat);

    priv->map_x = pixel_x - GTK_WIDGET(map)->allocation.width/2;
    priv->map_y = pixel_y - GTK_WIDGET(map)->allocation.height/2;

    osm_gps_map_map_redraw_idle(map);
}

int
osm_gps_map_set_zoom (OsmGpsMap *map, int zoom)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), 0);
    OsmGpsMapPrivate *priv = map->priv;

    if (zoom != priv->map_zoom)
    {
        int width_center  = GTK_WIDGET(map)->allocation.width / 2;
        int height_center = GTK_WIDGET(map)->allocation.height / 2;

        int zoom_old = priv->map_zoom;
        //constrain zoom min_zoom -> max_zoom
        priv->map_zoom = CLAMP(zoom, priv->min_zoom, priv->max_zoom);

        priv->map_x = lon2pixel(priv->map_zoom, priv->center_rlon) - width_center;
        priv->map_y = lat2pixel(priv->map_zoom, priv->center_rlat) - height_center;

        g_debug("Zoom changed from %d to %d x:%d",
                zoom_old, priv->map_zoom, priv->map_x);

        /* OSD may contain a scale, so we may have to re-render it */
        osm_gps_map_osd_render(priv->osd);

        osm_gps_map_map_redraw_idle(map);
    }
    return priv->map_zoom;
}

void
osm_gps_map_add_track (OsmGpsMap *map, GSList *track)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    OsmGpsMapPrivate *priv = map->priv;

    priv->tracks = g_slist_append(priv->tracks, track);
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_add_bounds(OsmGpsMap *map, GSList *bounds)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    OsmGpsMapPrivate *priv = map->priv;

    priv->bounds = g_slist_append(priv->bounds, bounds);
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_track_remove_all (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    osm_gps_map_free_tracks(map);
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_gps_add (OsmGpsMap *map, float latitude, float longitude, float heading)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));
    OsmGpsMapPrivate *priv = map->priv;

    priv->gps.rlat = deg2rad(latitude);
    priv->gps.rlon = deg2rad(longitude);
    priv->gps_valid = true;
    priv->gps_heading = deg2rad(heading);

    // dont draw anything if we are dragging
    if (priv->dragging) {
        g_debug("Dragging");
        return;
    }

    // this redraws the map (including the gps track, and adjusts the
    // map center if it was changed
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_gps_clear (OsmGpsMap *map)
{
    osm_gps_map_map_redraw_idle(map);
}

OsmGpsMapPoint
osm_gps_map_convert_screen_to_geographic(OsmGpsMap *map, gint pixel_x, gint pixel_y)
{
    OsmGpsMapPrivate *priv = map->priv;
    OsmGpsMapPoint pt;

    pt.rlat = pixel2lat(priv->map_zoom, priv->map_y + pixel_y);
    pt.rlon = pixel2lon(priv->map_zoom, priv->map_x + pixel_x);

    return pt;
}

void
osm_gps_map_redraw (OsmGpsMap *map)
{
    osm_gps_map_map_redraw_idle(map);
}

struct osd_priv_s *
osm_gps_map_osd_get(OsmGpsMap *map)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), nullptr);
    return map->priv->osd;
}

void
osm_gps_map_repaint (OsmGpsMap *map)
{
    osm_gps_map_expose (GTK_WIDGET(map), nullptr);
}

OsmGpsMapPoint *
osm_gps_map_get_gps (OsmGpsMap *map)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), nullptr);

    if(!map->priv->gps_valid)
        return nullptr;

    return &map->priv->gps;
}
