/*
 * Copyright (C) 2008 Till Harbaum <till@harbaum.org>.
 *
 * This file is part of OSM2Go.
 *
 * OSM2Go is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OSM2Go is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OSM2Go.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "appdata.h"

void statusbar_highlight(appdata_t *appdata, gboolean highlight) {
  if(highlight) {
    GdkColor color;
    gdk_color_parse("red", &color); 
    gtk_widget_modify_bg(appdata->statusbar->eventbox, GTK_STATE_NORMAL, &color);
    gtk_widget_modify_base(appdata->statusbar->eventbox, GTK_STATE_NORMAL, &color);
    gtk_widget_modify_fg(appdata->statusbar->eventbox, GTK_STATE_NORMAL, &color);
  } else
    gtk_widget_modify_bg(appdata->statusbar->eventbox, GTK_STATE_NORMAL, NULL);
}


// Set the persistent message, replacing anything currently there.

void statusbar_set(appdata_t *appdata, const char *msg, gboolean highlight) {
  statusbar_highlight(appdata, highlight);

  printf("statusbar_set: %s\n", msg);

  if (appdata->statusbar->mid) {
    gtk_statusbar_remove(GTK_STATUSBAR(appdata->statusbar->widget),
                         appdata->statusbar->cid, appdata->statusbar->mid);
    appdata->statusbar->mid = 0;
  }

  if (msg) {
    guint mid = gtk_statusbar_push(GTK_STATUSBAR(appdata->statusbar->widget),
                                   appdata->statusbar->cid, msg);
    appdata->statusbar->mid = mid;
  }
}


// Clear any brief message currently set, dropping back to the persistent one.

static gboolean statusbar_brief_clear(gpointer data) {
  appdata_t *appdata = (appdata_t *)data;
  if (appdata->statusbar->brief_mid) {
    gtk_statusbar_remove(GTK_STATUSBAR(appdata->statusbar->widget),
                         appdata->statusbar->cid,
                         appdata->statusbar->brief_mid);
    appdata->statusbar->brief_mid = 0;
  }
  return FALSE;
}

// Flash up a brief, temporary message. Once it disappears, drop back to any
// persistent message set with statusbar_set(). 
//
// If msg is NULL, clear the message and don't establish a handler.
//
// If timeout is negative, don't establish a handler. You'll have to clear it
// yourself later. If it's zero, use the default.

void statusbar_brief(appdata_t *appdata, const char *msg, gint timeout) {
  printf("statusbar_brief: %s\n", msg);
  if (appdata->statusbar->brief_handler_id) {
    gtk_timeout_remove(appdata->statusbar->brief_handler_id);
    appdata->statusbar->brief_handler_id = 0;
  }
  statusbar_brief_clear(appdata);
  guint mid = 0;
  if (msg) {
    mid = gtk_statusbar_push(GTK_STATUSBAR(appdata->statusbar->widget),
                                 appdata->statusbar->cid, msg);
    if (mid) {
      appdata->statusbar->brief_mid = mid;
    }
  }
  if (mid && (timeout >= 0)) {
    if (timeout == 0) {
      timeout = STATUSBAR_DEFAULT_BRIEF_TIME;
    }
    appdata->statusbar->brief_handler_id 
      = gtk_timeout_add(timeout, statusbar_brief_clear, appdata);
  }
}

GtkWidget *statusbar_new(appdata_t *appdata) {
  appdata->statusbar = (statusbar_t*)g_new0(statusbar_t, 1);

  appdata->statusbar->eventbox = gtk_event_box_new();
  appdata->statusbar->widget = gtk_statusbar_new();

#ifdef USE_HILDON
  /* why the heck does hildon show this by default? It's useless!! */
  g_object_set(appdata->statusbar->widget,
	       "has-resize-grip", FALSE, 
	       NULL );
#endif
  gtk_container_add(GTK_CONTAINER(appdata->statusbar->eventbox), 
		    appdata->statusbar->widget);

  appdata->statusbar->cid = gtk_statusbar_get_context_id(
		GTK_STATUSBAR(appdata->statusbar->widget), "Msg");

  return appdata->statusbar->eventbox;
}

void statusbar_free(statusbar_t *statusbar) {
  if(statusbar)
    g_free(statusbar);
}

// vim:et:ts=8:sw=2:sts=2:ai
