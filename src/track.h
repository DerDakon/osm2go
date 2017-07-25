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

#include <vector>

struct track_point_t {
  track_point_t();
  track_point_t(const pos_t &p, float alt = 0.0f, time_t t = 0);
  pos_t pos;               /* position in lat/lon format */
  time_t time;
  float altitude;
};

struct track_seg_t {
  std::vector<track_point_t> track_points;
  std::vector<canvas_item_t *> item_chain;
};

struct track_t {
  track_t();

  std::vector<track_seg_t> segments;
  bool dirty;
  bool active; ///< if the last element in segments is currently written to
};

struct appdata_t;
struct project_t;
typedef struct track_t track_t;

/* used internally to save and restore the currently displayed track */
void track_save(struct project_t *project, track_t *track);
/**
 * @brief restore the track of the current project
 * @param appdata global appdata object
 * @return if a track was loaded
 */
bool track_restore(appdata_t &appdata);

/* accessible via the menu */
void track_clear(appdata_t &appdata);
void track_export(const track_t *track, const char *filename);
track_t *track_import(const char *filename);
/**
 * @brief set enable state of "track export" and "track clear" menu entries
 * @param appdata global appdata object
 *
 * The state will be set depending on appdata->track.track presence.
 */
void track_menu_set(appdata_t &appdata);

void track_enable_gps(appdata_t &appdata, gboolean enable);

#endif // TRACK_H
