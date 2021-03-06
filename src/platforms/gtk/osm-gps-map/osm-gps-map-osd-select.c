/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 cino=t0,(0: */
/*
 * SPDX-FileCopyrightText: Till Harbaum 2009 <till@harbaum.org>
 *
 * osm-gps-map is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * osm-gps-map is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "osm-gps-map-osd-select.h"

#include "converter.h"
#include "osm-gps-map.h"

#include <cairo.h>
#include <math.h>    // M_PI/cos()
#include <stdlib.h>  // abs

#define OSD_SELECT   OSD_CUSTOM
#define OSD_DRAG     OSD_CUSTOM+1

#ifdef FREMANTLE
#define OSD_W 80
#else
#define OSD_W 40
#endif

#define OSD_H (2*OSD_W)
#define CRAD  (OSD_W/5)   // corner radius

#define ICON_BORDER  (OSD_W/5)
#define ICON_SIZE    (OSD_W - (2*ICON_BORDER))
#define ICON_LINE_W  (OSD_W/20)

//the osd controls
typedef struct osd_priv_s {
    struct {
        cairo_surface_t *surface;
        gboolean state;
    } select_toggle;

    struct {
        cairo_surface_t *surface;
    } zoom;

} osd_priv_t;

#define ARROW_W  (ICON_SIZE/3)
#define ARROW_H  (ICON_SIZE/3)

static void render_arrow(cairo_t *cr, int angle) {
    // the angles are 0, pi, -pi/2, pi/2
    // now their sin() results (cos() results are just backwards)
    const float an_sin[] = { 0, 0, -1, 1 };
    float x = (2 - an_sin[3 - angle]) * OSD_W / 4;
    float y = 3 * OSD_H / 4 + an_sin[angle] * OSD_W / 4;
#define R(a,b)  x + an_sin[3 - angle] * (a) + an_sin[angle] * (b), y - an_sin[angle] * (a) + an_sin[3 - angle] * (b)

    cairo_move_to (cr, R(-ARROW_W/2, 0));
    cairo_line_to (cr, R(0, -ARROW_H/2));
    cairo_line_to (cr, R(0, -ARROW_H/4));
    cairo_line_to (cr, R(+ARROW_W/2, -ARROW_H/4));
    cairo_line_to (cr, R(+ARROW_W/2, +ARROW_H/4));
    cairo_line_to (cr, R(0, +ARROW_H/4));
    cairo_line_to (cr, R(0, +ARROW_H/2));

    cairo_close_path (cr);
    cairo_stroke(cr);
}

static void
osd_render_toggle(osd_priv_t *priv)
{
    g_assert(priv->select_toggle.surface != NULL);

    /* first fill with transparency */
    cairo_t *cr = cairo_create(priv->select_toggle.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    /* now start painting on top */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* draw dark transparent background for right border */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
    cairo_move_to (cr, OSD_W, 0);
    cairo_line_to (cr, CRAD, 0);
    cairo_arc_negative (cr, CRAD, CRAD, CRAD, -M_PI/2, M_PI);
    cairo_line_to (cr, 0, OSD_H-CRAD);
    cairo_arc_negative (cr, CRAD, OSD_H-CRAD, CRAD, M_PI, M_PI/2);
    cairo_line_to (cr, OSD_W, OSD_H);
    cairo_close_path (cr);
    cairo_fill(cr);

    /* draw select icon on top */
    cairo_set_line_width (cr, ICON_LINE_W);

    float bright = priv->select_toggle.state?0.5:1.0;
    cairo_set_source_rgb(cr, bright, bright, bright);

    cairo_rectangle(cr, ICON_BORDER, ICON_BORDER,
                    ICON_SIZE-ICON_BORDER, ICON_SIZE-ICON_BORDER);
    cairo_stroke(cr);
    double dash[] = { ICON_LINE_W, ICON_LINE_W };
    cairo_set_dash(cr, dash, 2, 0.0);
    cairo_rectangle(cr, ICON_BORDER, ICON_BORDER,
                    ICON_SIZE, ICON_SIZE);
    cairo_stroke(cr);

    /* draw drag icon below */
    bright = priv->select_toggle.state?1.0:0.5;
    cairo_set_source_rgb(cr, bright, bright, bright);

    cairo_set_dash(cr, NULL, 0, 0.0);
    for(int i = 0; i < 4; i++)
      render_arrow(cr, i);

    cairo_destroy(cr);
}

static void
osd_render_zoom(osd_priv_t *priv)
{
    g_assert(priv->zoom.surface != NULL);

    /* first fill with transparency */
    cairo_t *cr = cairo_create(priv->zoom.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    /* now start painting on top */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* draw dark transparent background */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
    cairo_move_to (cr, 0, 0);
    cairo_line_to (cr, OSD_W-CRAD, 0);
    cairo_arc (cr, OSD_W-CRAD, CRAD, CRAD, -M_PI/2, 0);
    cairo_line_to (cr, OSD_W, OSD_H-CRAD);
    cairo_arc (cr, OSD_W-CRAD, OSD_H-CRAD, CRAD, 0, M_PI/2);
    cairo_line_to (cr, 0, OSD_H);
    cairo_close_path (cr);
    cairo_fill(cr);

    /* draw select icon on top */
    cairo_set_line_width (cr, 2*ICON_LINE_W);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

    cairo_move_to (cr, ICON_BORDER, OSD_W/2);
    cairo_line_to (cr, OSD_W-ICON_BORDER, OSD_W/2);
    cairo_move_to (cr, OSD_W/2, ICON_BORDER);
    cairo_line_to (cr, OSD_W/2, OSD_W-ICON_BORDER);
    cairo_stroke(cr);

    cairo_move_to (cr, ICON_BORDER, OSD_W+OSD_W/2);
    cairo_line_to (cr, OSD_W-ICON_BORDER, OSD_W+OSD_W/2);
    cairo_stroke(cr);

    cairo_destroy(cr);
}

static int
in_range(int i, int x, int a)
{
    return (i <= x && x <= a) ? 1 : 0;
}

osd_button_t
osm_gps_map_osd_check(OsmGpsMap *map, gint x, gint y)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP (map), OSD_NONE);

    osd_priv_t *priv = osm_gps_map_osd_get(map);
    GtkWidget *widget = GTK_WIDGET(map);

    y -= (widget->allocation.height - OSD_H)/2;

    if(x < widget->allocation.width/2) {
        if(in_range(0, y, OSD_H) && in_range(0, x, OSD_W)) {
            if(y < OSD_W)
                return OSD_IN;
            else
                return OSD_OUT;
        }
    } else {
        x -= widget->allocation.width - OSD_W;

        if(in_range(0, y, OSD_H) && in_range(0, x, OSD_W)) {
            if(y < OSD_W) {
                if(priv->select_toggle.state) {
                    priv->select_toggle.state = FALSE;
                    osd_render_toggle(priv);
                    osm_gps_map_expose(widget, NULL);
                }

                return OSD_SELECT;
            } else {
                if(!priv->select_toggle.state) {
                    priv->select_toggle.state = TRUE;
                    osd_render_toggle(priv);
                    osm_gps_map_expose(widget, NULL);
                }

                return OSD_DRAG;
            }
        }
    }

    return OSD_NONE;
}

void
osm_gps_map_osd_render(struct osd_priv_s *priv)
{
    /* this function is actually called pretty often since the */
    /* OSD contents may have changed (due to a coordinate/zoom change). */
    /* The different OSD parts have to make sure that they don't */
    /* render unneccessarily often and thus waste CPU power */

    if(!priv->select_toggle.surface) {
        priv->select_toggle.surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, OSD_W, OSD_H);

        osd_render_toggle(priv);
    }

    if(!priv->zoom.surface) {
        priv->zoom.surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, OSD_W, OSD_H);

        osd_render_zoom(priv);
    }
}

void
osm_gps_map_osd_draw(struct osd_priv_s *priv, GtkWidget * widget, GdkDrawable *drawable)
{
    if(!priv->select_toggle.surface)
        osm_gps_map_osd_render(priv);

    // now draw this onto the original context
    cairo_t *cr = gdk_cairo_create(drawable);

    cairo_set_source_surface(cr, priv->select_toggle.surface,
                             widget->allocation.width - OSD_W,
                             (widget->allocation.height - OSD_H)/2);
    cairo_paint(cr);

    cairo_set_source_surface(cr, priv->zoom.surface, 0,
                             (widget->allocation.height - OSD_H)/2);
    cairo_paint(cr);

    cairo_destroy(cr);
}

void
osm_gps_map_osd_free(struct osd_priv_s *priv)
{
    if(priv->select_toggle.surface) {
        cairo_surface_destroy(priv->select_toggle.surface);
        priv->select_toggle.surface = NULL;
    }

    if(priv->zoom.surface) {
        cairo_surface_destroy(priv->zoom.surface);
        priv->zoom.surface = NULL;
    }
}

struct osd_priv_s *
osm_gps_map_osd_select_init(void)
{
    osd_priv_t *priv = g_new0(osd_priv_t, 1);

    priv->select_toggle.state = TRUE;

    return priv;
}

gboolean
osm_gps_map_osd_get_state(OsmGpsMap *map) {
    osd_priv_t *priv = osm_gps_map_osd_get(map);
    g_return_val_if_fail (priv, FALSE);

    return priv->select_toggle.state;
}
