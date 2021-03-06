/*
 * SPDX-FileCopyrightText: 2008 Till Harbaum <till@harbaum.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "osm.h"

#include <osm2go_platform.h>

class map_t;
class presets_items;

bool info_dialog(osm2go_platform::Widget *parent, map_t *map, osm_t::ref osm, presets_items *presets, object_t &object);

void relation_membership_dialog(osm2go_platform::Widget *parent, const presets_items *presets,
                                osm_t::ref osm, object_t &object);

void relation_list(osm2go_platform::Widget *parent, map_t *map, osm_t::ref osm, presets_items *presets);

/**
 * @returns if the dialog was accepted by the user
 */
bool relation_show_members(osm2go_platform::Widget *parent, relation_t *relation, osm_t::ref osm, const presets_items *p);
