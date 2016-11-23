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

#ifndef TRACK_H
#define TRACK_H

#include "canvas.h"
#include "pos.h"
#include "project.h"

#ifdef __cplusplus
#include <vector>

typedef struct track_point_t {
  struct track_point_t *next;
  pos_t pos;               /* position in lat/lon format */
  time_t time;
  float altitude;
} track_point_t;

struct track_seg_t {
  track_seg_t() : track_point(0) {}
  track_point_t *track_point;
  std::vector<canvas_item_t *> item_chain;
};

struct track_t {
  std::vector<track_seg_t *> segments;
  bool dirty;
  bool active; ///< if the last element in segments is currently written to
};

/**
 * @brief count a point sequence
 * @param point first point
 * @return how many points are in the given sequence
 * @retval 0 point is NULL
 */
gint track_points_count(const track_point_t *point);
bool track_is_empty(const track_seg_t *seg);

extern "C" {
#endif

typedef struct track_t track_t;

/* used internally to save and restore the currently displayed track */
void track_save(project_t *project, track_t *track);
/**
 * @brief restore the track of the current project
 * @param appdata global appdata object
 * @return if a track was loaded
 */
gboolean track_restore(appdata_t *appdata);

/**
 * @brief free the track
 * @param track the track instance to remove
 */
void track_delete(track_t *track);

/* accessible via the menu */
void track_clear(appdata_t *appdata);
void track_export(const track_t *track, const char *filename);
track_t *track_import(const char *filename);
/**
 * @brief set enable state of "track export" and "track clear" menu entries
 * @param appdata global appdata object
 *
 * The state will be set depending on appdata->track.track presence.
 */
void track_menu_set(appdata_t *appdata);

void track_enable_gps(appdata_t *appdata, gboolean enable);

#ifdef __cplusplus
}
#endif

#endif // TRACK_H
