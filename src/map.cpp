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
 * along with OSM2Go.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "map.h"

#include "appdata.h"
#include "canvas.h"
#include "diff.h"
#include "gps.h"
#include "iconbar.h"
#include "info.h"
#include "map_edit.h"
#include "map_hl.h"
#include "misc.h"
#include "notifications.h"
#include "osm2go_platform.h"
#include "project.h"
#include "style.h"
#include "track.h"
#include "uicontrol.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "osm2go_annotations.h"
#include <osm2go_cpp.h>
#include <osm2go_i18n.h>
#include <osm2go_stl.h>

int map_item_t::get_segment(lpos_t pos) const {
  return item->get_segment(pos);
}

/* this is a chain of map_items which is attached to all entries */
/* in the osm tree (node_t, way_t, ...) to be able to get a link */
/* to the screen representation of a give node/way/etc */
struct map_item_chain_t {
  std::vector<map_item_t *> map_items;
  canvas_item_t *firstCanvasItem() const;
};

canvas_item_t *map_item_chain_t::firstCanvasItem() const {
  if(map_items.empty())
    return nullptr;
  return map_items.front()->item;
}

static void map_statusbar(map_t *map, map_item_t *map_item) {
  const std::string &str = map_item->object.get_name();
  MainUi::NotificationFlags flags = map_item->object.obj->tags.hasTagCollisions() ?
                                    MainUi::Highlight : MainUi::NoFlags;
  map->appdata.uicontrol->showNotification(str.c_str(), flags);
}

void map_t::outside_error() {
  error_dlg(_("Items must not be placed outside the working area!"));
}

static inline void map_item_destroy_canvas_item(map_item_t *m) {
  delete m->item;
}

void visible_item_t::item_chain_destroy()
{
  if(map_item_chain == nullptr)
    return;

  std::for_each(map_item_chain->map_items.begin(), map_item_chain->map_items.end(),
                map_item_destroy_canvas_item);

  delete map_item_chain;
  map_item_chain = nullptr;
}

static void map_node_select(map_t *map, node_t *node) {
  map_item_t *map_item = &map->selected;

  assert(map->highlight.isEmpty());

  map_item->object = node;
  map_item->highlight = false;

  /* node may not have any visible representation at all */
  if(node->map_item_chain)
    map_item->item = node->map_item_chain->firstCanvasItem();
  else
    map_item->item = nullptr;

  map_statusbar(map, map_item);
  map->appdata.iconbar->map_item_selected(map_item->object);

  /* highlight node */
  int x = map_item->object.node->lpos.x, y = map_item->object.node->lpos.y;

  /* create a copy of this map item and mark it as being a highlight */
  map_item_t *new_map_item = new map_item_t(*map_item);
  new_map_item->highlight = true;

  float radius = 0;
  style_t::IconCache::iterator it;
  if(map->style->icon.enable &&
     (it = map->style->node_icons.find(node->id)) != map->style->node_icons.end()) {
    /* icons are technically square, so a radius slightly bigger */
    /* than sqrt(2)*MAX(w,h) should fit nicely */
    radius = 0.75 * map->style->icon.scale * it->second->maxDimension();
  } else {
    radius = map->style->highlight.width + map->style->node.radius;
    if(!node->ways)
      radius += map->style->node.border_radius;
  }

  radius *= map->state.detail;

  map->highlight.circle_new(map, CANVAS_GROUP_NODES_HL, new_map_item, x, y,
                            radius, map->style->highlight.color);

  if(!map_item->item) {
    /* and draw a fake node */
    new_map_item = new map_item_t(*map_item);
    new_map_item->highlight = true;
    map->highlight.circle_new(map, CANVAS_GROUP_NODES_IHL, new_map_item, x, y,
                              map->style->node.radius, map->style->highlight.node_color);
  }
}

struct set_point_pos {
  std::vector<lpos_t> &points;
  explicit set_point_pos(std::vector<lpos_t> &p) : points(p) {}
  void operator()(const node_t *n) {
    points.push_back(n->lpos);
  }
};

/**
 * @brief create a canvas point array for a way
 * @param way the way to convert
 * @return canvas node array if at least 2 nodes were present
 * @retval nullptr the way has less than 2 nodes
 */
static std::vector<lpos_t>  __attribute__((nonnull(1)))
points_from_node_chain(const way_t *way)
{
  const unsigned int nodes = way->node_chain.size();
  std::vector<lpos_t> points(nodes);

  // the vector has the correct allocated size now, fill as usual
  points.clear();

  /* a way needs at least 2 points to be drawn */
  if (unlikely(nodes < 2))
    return points;

  /* allocate space for nodes */
  std::for_each(way->node_chain.begin(), way->node_chain.end(),
                set_point_pos(points));

  return points;
}

struct draw_selected_way_functor {
  node_t *last;
  const float arrow_width;
  map_t * const map;
  way_t * const way;
  draw_selected_way_functor(float a, map_t *m, way_t *w)
    : last(nullptr), arrow_width(a), map(m), way(w) {}
  void operator()(node_t *node);
};

void draw_selected_way_functor::operator()(node_t* node)
{
  map_item_t item;
  item.object = node;

  /* draw an arrow between every two nodes */
  if(last) {
    struct { float x, y;} center, diff;
    center.x = (last->lpos.x + node->lpos.x)/2;
    center.y = (last->lpos.y + node->lpos.y)/2;
    diff.x = node->lpos.x - last->lpos.x;
    diff.y = node->lpos.y - last->lpos.y;

    /* only draw arrow if there's sufficient space */
    /* TODO: what if there's not enough space anywhere? */
    float len = std::sqrt(std::pow(diff.x, 2) + std::pow(diff.y, 2));
    if(len > map->style->highlight.arrow_limit * arrow_width) {
      /* create a new map item for every arrow */
      map_item_t *new_map_item = new map_item_t(object_t(way), true);

      len /= arrow_width;
      diff.x /= len;
      diff.y /= len;

      std::vector<lpos_t> points(4);
      points[0] = lpos_t(center.x + diff.x, center.y + diff.y);
      points[1] = lpos_t(center.x + diff.y - diff.x, center.y - diff.x - diff.y);
      points[2] = lpos_t(center.x - diff.y - diff.x, center.y + diff.x - diff.y);
      points[3] = points[0];

      map->highlight.polygon_new(map, CANVAS_GROUP_WAYS_DIR, new_map_item,
                                 points, map->style->highlight.arrow_color);
    }
  }

  if(!map->highlight.isHighlighted(item)) {
    /* create a new map item for every node */
    map->highlight.circle_new(map, CANVAS_GROUP_NODES_IHL,
                              new map_item_t(object_t(node), true),
                              node->lpos.x, node->lpos.y,
                              map->style->node.radius * map->state.detail,
                              map->style->highlight.node_color);
  }

  last = node;
}

void map_t::select_way(way_t *way) {
  map_item_t *map_item = &selected;

  assert(highlight.isEmpty());

  map_item->object = way;
  map_item->highlight = false;
  map_item->item      = way->map_item_chain->firstCanvasItem();

  map_statusbar(this, map_item);
  appdata.iconbar->map_item_selected(map_item->object);
  appdata.uicontrol->setActionEnable(MainUi::MENU_ITEM_MAP_HIDE_SEL, true);

  float arrow_width = ((map_item->object.way->draw.flags & OSM_DRAW_FLAG_BG)?
                        style->highlight.width + map_item->object.way->draw.bg.width / 2:
                        style->highlight.width + map_item->object.way->draw.width / 2)
                       * state.detail;

  const node_chain_t &node_chain = map_item->object.way->node_chain;
  std::for_each(node_chain.begin(), node_chain.end(),
                draw_selected_way_functor(arrow_width, this, way));

  /* a way needs at least 2 points to be drawn */
  assert(map_item->object.way == way);
  const std::vector<lpos_t> &points = points_from_node_chain(way);
  if(!points.empty()) {
    /* create a copy of this map item and mark it as being a highlight */
    map_item_t *new_map_item = new map_item_t(*map_item);
    new_map_item->highlight = true;

    highlight.polyline_new(this, CANVAS_GROUP_WAYS_HL, new_map_item, points,
		 ((way->draw.flags & OSM_DRAW_FLAG_BG)?
                  2 * style->highlight.width + way->draw.bg.width:
                  2 * style->highlight.width + way->draw.width)
                 * state.detail, style->highlight.color);
  }
}

struct relation_select_functor {
  map_t * const map;
  relation_select_functor(map_t *m) : map(m) {}
  void operator()(member_t &member);
};

void relation_select_functor::operator()(member_t& member)
{
  canvas_item_t *item = nullptr;

  switch(member.object.type) {
  case object_t::NODE: {
    node_t *node = member.object.node;
    printf("  -> node " ITEM_ID_FORMAT "\n", node->id);

    item = map->canvas->circle_new(CANVAS_GROUP_NODES_HL,
                             node->lpos.x, node->lpos.y,
                             map->style->highlight.width + map->style->node.radius,
                             0, map->style->highlight.color, NO_COLOR);
    break;
    }
  case object_t::WAY: {
    way_t *way = member.object.way;
    /* a way needs at least 2 points to be drawn */
    const std::vector<lpos_t> &points = points_from_node_chain(way);
    if(!points.empty()) {
      if(way->draw.flags & OSM_DRAW_FLAG_AREA)
        item = map->canvas->polygon_new(CANVAS_GROUP_WAYS_HL, points, 0, 0,
                                  map->style->highlight.color);
      else
        item = map->canvas->polyline_new(CANVAS_GROUP_WAYS_HL, points,
                                   (way->draw.flags & OSM_DRAW_FLAG_BG) ?
                                     2 * map->style->highlight.width + way->draw.bg.width :
                                     2 * map->style->highlight.width + way->draw.width,
                                   map->style->highlight.color);
    }
    break;
    }

  default:
    break;
  }

  /* attach item to item chain */
  if(item)
    map->highlight.items.push_back(item);
}


void map_t::select_relation(relation_t *relation) {
  printf("highlighting relation " ITEM_ID_FORMAT "\n", relation->id);

  assert(highlight.isEmpty());

  selected.object = relation;
  selected.highlight = false;
  selected.item = nullptr;

  map_statusbar(this, &selected);
  appdata.iconbar->map_item_selected(selected.object);

  /* process all members */
  relation_select_functor fc(this);
  std::for_each(relation->members.begin(), relation->members.end(), fc);
}

static void map_object_select(map_t *map, object_t &object) {
  switch(object.type) {
  case object_t::NODE:
    map_node_select(map, object.node);
    break;
  case object_t::WAY:
    map->select_way(object.way);
    break;
  case object_t::RELATION:
    map->select_relation(object.relation);
    break;
  default:
    assert_unreachable();
  }
}

void map_t::item_deselect() {

  /* save tags for "last" function in info dialog */
  if(selected.object.is_real() && selected.object.obj->tags.hasRealTags()) {
    if(selected.object.type == object_t::NODE)
      last_node_tags = selected.object.obj->tags.asMap();
    else if(selected.object.type == object_t::WAY)
      last_way_tags = selected.object.obj->tags.asMap();
  }

  /* remove statusbar message */
  appdata.uicontrol->showNotification(nullptr);

  /* disable/enable icons in icon bar */
  appdata.iconbar->map_item_selected(object_t());
  appdata.uicontrol->setActionEnable(MainUi::MENU_ITEM_MAP_HIDE_SEL, false);

  /* remove highlight */
  highlight.clear();

  /* forget about selection */
  selected.object.type = object_t::ILLEGAL;
}

static void map_node_new(map_t *map, node_t *node, unsigned int radius,
                         int width, color_t fill, color_t border) {
  map_item_t *map_item = new map_item_t(object_t(node));

  style_t::IconCache::const_iterator it;

  if(!map->style->icon.enable ||
     (it = map->style->node_icons.find(node->id)) == map->style->node_icons.end())
    map_item->item = map->canvas->circle_new(CANVAS_GROUP_NODES,
       node->lpos.x, node->lpos.y, radius, width, fill, border);
  else
    map_item->item = map->canvas->image_new(CANVAS_GROUP_NODES, it->second->buffer(),
                                            node->lpos.x, node->lpos.y,
		      map->state.detail * map->style->icon.scale,
		      map->state.detail * map->style->icon.scale);

  map_item->item->set_zoom_max(node->zoom_max / (2 * map->state.detail));

  /* attach map_item to nodes map_item_chain */
  if(!node->map_item_chain)
    node->map_item_chain = new map_item_chain_t();
  node->map_item_chain->map_items.push_back(map_item);

  map_item->item->set_user_data(map_item);

  map_item->item->destroy_connect(map_item_t::free, map_item);
}

static map_item_t *map_way_new(map_t *map, canvas_group_t group,
                               way_t *way, const std::vector<lpos_t> &points, unsigned int width,
                               color_t color, color_t fill_color) {
  map_item_t *map_item = new map_item_t(object_t(way));

  if(way->draw.flags & OSM_DRAW_FLAG_AREA) {
    if(map->style->area.color & 0xff)
      map_item->item = map->canvas->polygon_new(group, points,
					  width, color, fill_color);
    else
      map_item->item = map->canvas->polyline_new(group, points,
					   width, color);
  } else {
    map_item->item = map->canvas->polyline_new(group, points, width, color);
  }

  map_item->item->set_zoom_max(way->zoom_max / (2 * map->state.detail));

  /* a ways outline itself is never dashed */
  if (group != CANVAS_GROUP_WAYS_OL && way->draw.dash_length_on > 0)
    map_item->item->set_dashed(width, way->draw.dash_length_on, way->draw.dash_length_off);

  map_item->item->set_user_data(map_item);

  map_item->item->destroy_connect(map_item_t::free, map_item);

  return map_item;
}

void map_t::show_node(node_t *node) {
  map_node_new(this, node, style->node.radius, 0, style->node.color, 0);
}

struct map_way_draw_functor {
  map_t * const map;
  explicit map_way_draw_functor(map_t *m) : map(m) {}
  void operator()(way_t *way);
  void operator()(std::pair<item_id_t, way_t *> pair) {
    operator()(pair.second);
  }
};

void map_way_draw_functor::operator()(way_t *way)
{
  /* don't draw a way that's not there anymore */
  if(way->flags & (OSM_FLAG_DELETED | OSM_FLAG_HIDDEN))
    return;

  /* attach map_item to ways map_item_chain */
  if(!way->map_item_chain)
    way->map_item_chain = new map_item_chain_t();
  std::vector<map_item_t *> &chain = way->map_item_chain->map_items;
  map_item_t *map_item;

  /* allocate space for nodes */
  /* a way needs at least 2 points to be drawn */
  const std::vector<lpos_t> &points = points_from_node_chain(way);
  if(unlikely(points.empty())) {
    /* draw a single dot where this single node is */
    map_item = new map_item_t(object_t(way));

    assert(!way->node_chain.empty());
    const lpos_t &firstPos = way->node_chain.front()->lpos;
    map_item->item = map->canvas->circle_new(CANVAS_GROUP_WAYS, firstPos.x, firstPos.y,
                                             map->style->node.radius, 0,
                                             map->style->node.color, 0);

    // TODO: decide: do we need canvas_item_t::set_zoom_max() here too?

    map_item->item->set_user_data(map_item);

    map_item->item->destroy_connect(map_item_t::free, map_item);
  } else {
    /* draw way */
    float width = way->draw.width * map->state.detail;

    if(way->draw.flags & OSM_DRAW_FLAG_AREA) {
      map_item = map_way_new(map, CANVAS_GROUP_POLYGONS, way, points,
                             width, way->draw.color, way->draw.area.color);
    } else if(way->draw.flags & OSM_DRAW_FLAG_BG) {
      chain.push_back(map_way_new(map, CANVAS_GROUP_WAYS_INT, way, points,
                                  width, way->draw.color, NO_COLOR));

      map_item = map_way_new(map, CANVAS_GROUP_WAYS_OL, way, points,
                             way->draw.bg.width * map->state.detail,
                             way->draw.bg.color, NO_COLOR);

    } else {
      map_item = map_way_new(map, CANVAS_GROUP_WAYS, way, points,
                             width, way->draw.color, NO_COLOR);
    }
  }

  chain.push_back(map_item);
}

void map_t::draw(way_t *way) {
  map_way_draw_functor m(this);
  m(way);
}

struct map_node_draw_functor {
  map_t * const map;
  explicit map_node_draw_functor(map_t *m) : map(m) {}
  void operator()(node_t *node);
  void operator()(std::pair<item_id_t, node_t *> pair) {
    operator()(pair.second);
  }
};

void map_node_draw_functor::operator()(node_t *node)
{
  /* don't draw a node that's not there anymore */
  if(node->flags & OSM_FLAG_DELETED)
    return;

  if(!node->ways)
    map_node_new(map, node,
		 map->style->node.radius * map->state.detail,
		 map->style->node.border_radius * map->state.detail,
		 map->style->node.fill_color,
		 map->style->node.color);

  else if(map->style->node.show_untagged || node->tags.hasRealTags())
    map_node_new(map, node,
		 map->style->node.radius * map->state.detail, 0,
		 map->style->node.color, 0);
}

void map_t::draw(node_t *node) {
  map_node_draw_functor m(this);
  m(node);
}

void map_t::redraw_item(object_t object) {
  /* a relation cannot be redrawn as it doesn't have a visual */
  /* representation */
  if(object.type == object_t::RELATION)
    return;

  /* check if the item to be redrawn is the selected one */
  bool is_selected = (object == selected.object);
  // object must not be passed by reference or by pointer because of this:
  // map_t::item_deselect would modify object.type of the selected object, if
  // exactly that is passed in the switch statements below would see an
  // invalid type
  if(is_selected)
    item_deselect();

  assert(object.is_real());
  static_cast<visible_item_t *>(object.obj)->item_chain_destroy();

  switch (object.type){
  case object_t::WAY:
    style->colorize_way(object.way);
    draw(object.way);
    break;
  case object_t::NODE:
    style->colorize_node(object.node);
    draw(object.node);
    break;
  default:
    assert_unreachable();
  }

  /* restore selection if there was one */
  if(is_selected)
    map_object_select(this, object);
}

static void map_frisket_rectangle(std::vector<lpos_t> &points,
                                  int x0, int x1, int y0, int y1) {
  points[0] = lpos_t(x0, y0);
  points[1] = lpos_t(x1, y0);
  points[2] = lpos_t(x1, y1);
  points[3] = lpos_t(x0, y1);
  points[4] = points[0];
}

/* Draw the frisket area which masks off areas it'd be unsafe to edit,
 * plus its inner edge marker line */
static void map_frisket_draw(map_t *map, const bounds_t &bounds) {
  std::vector<lpos_t> points(5);

  /* don't draw frisket at all if it's completely transparent */
  if(map->style->frisket.color & 0xff) {
    color_t color = map->style->frisket.color;

    float mult = map->style->frisket.mult;

    /* top rectangle */
    map_frisket_rectangle(points, mult * bounds.min.x, mult * bounds.max.x,
                          mult * bounds.min.y, bounds.min.y);
    map->canvas->polygon_new(CANVAS_GROUP_FRISKET, points,
		       1, NO_COLOR, color);

    /* bottom rectangle */
    map_frisket_rectangle(points, mult * bounds.min.x, mult * bounds.max.x,
                          bounds.max.y, mult * bounds.max.y);
    map->canvas->polygon_new(CANVAS_GROUP_FRISKET, points,
		       1, NO_COLOR, color);

    /* left rectangle */
    map_frisket_rectangle(points, mult * bounds.min.x, bounds.min.x,
                          mult * bounds.min.y, mult * bounds.max.y);
    map->canvas->polygon_new(CANVAS_GROUP_FRISKET, points,
		       1, NO_COLOR, color);

    /* right rectangle */
    map_frisket_rectangle(points, bounds.max.x, mult * bounds.max.x,
                          mult * bounds.min.y, mult * bounds.max.y);
    map->canvas->polygon_new(CANVAS_GROUP_FRISKET, points,
		       1, NO_COLOR, color);

  }

  if(map->style->frisket.border.present) {
    // Edge marker line
    int ew2 = map->style->frisket.border.width/2;
    map_frisket_rectangle(points, bounds.min.x - ew2, bounds.max.x + ew2,
                                  bounds.min.y - ew2, bounds.max.y + ew2);

    map->canvas->polyline_new(CANVAS_GROUP_FRISKET, points,
			map->style->frisket.border.width,
			map->style->frisket.border.color);

  }
}

static void free_map_item_chain(std::pair<item_id_t, visible_item_t *> pair) {
  delete pair.second->map_item_chain;
  pair.second->map_item_chain = nullptr;
}

template<bool b> void free_track_item_chain(track_seg_t &seg) {
  if(b)
    std::for_each(seg.item_chain.begin(), seg.item_chain.end(),
                  std::default_delete<canvas_item_t>());
  seg.item_chain.clear();
}

static void map_free_map_item_chains(appdata_t &appdata) {
  if(unlikely(!appdata.project || !appdata.project->osm))
    return;

  osm_t::ref osm = appdata.project->osm;
  /* free all map_item_chains */
  std::for_each(osm->nodes.begin(), osm->nodes.end(),
                free_map_item_chain);

  std::for_each(osm->ways.begin(), osm->ways.end(),
                free_map_item_chain);

  if (appdata.track.track) {
    /* remove all segments */
    std::for_each(appdata.track.track->segments.begin(),
                  appdata.track.track->segments.end(),
                  free_track_item_chain<false>);
  }
}

/* get the item at position x, y */
map_item_t *map_t::item_at(int x, int y) {
  canvas_item_t *item = canvas->get_item_at(canvas->window2world(x, y));

  if(!item) {
    printf("  there's no item\n");
    return nullptr;
  }

  printf("  there's an item (%p)\n", item);

  map_item_t *map_item = item->get_user_data();

  if(!map_item) {
    printf("  item has no user data!\n");
    return nullptr;
  }

  if(map_item->highlight)
    printf("  item is highlight\n");

  printf("  item is %s #" ITEM_ID_FORMAT "\n",
	 map_item->object.type_string(),
	 map_item->object.obj->id);

  return map_item;
}

/* get the real item (no highlight) at x, y */
void map_t::pen_down_item() {
  pen_down.on_item = item_at(pen_down.at.x, pen_down.at.y);

  /* no item or already a real one */
  if(!pen_down.on_item || !pen_down.on_item->highlight)
    return;

  /* get the item (parent) this item is the highlight of */
  switch(pen_down.on_item->object.type) {
  case object_t::NODE:
  case object_t::WAY: {
    visible_item_t * const vis = static_cast<visible_item_t *>(pen_down.on_item->object.obj);
    if(vis->map_item_chain && !vis->map_item_chain->map_items.empty()) {
      map_item_t *parent = vis->map_item_chain->map_items.front();

      if(parent) {
        printf("  using parent item %s #" ITEM_ID_FORMAT "\n", vis->apiString(), vis->id);
        pen_down.on_item = parent;
        return;
      }
    }
    break;
  }

  default:
    assert_unreachable();
  }

  printf("  no parent, working on highlight itself\n");
}

/* Limitations on the amount by which we can scroll. Keeps part of the
 * map visible at all times */
static void map_limit_scroll(map_t *map, canvas_t::canvas_unit_t unit, int &sx, int &sy) {

  /* get scale factor for pixel->meter conversion. set to 1 if */
  /* given coordinates are already in meters */
  double scale = (unit == canvas_t::UNIT_METER) ? 1.0 : map->canvas->get_zoom();

  /* convert pixels to meters if necessary */
  double sx_cu = sx / scale;
  double sy_cu = sy / scale;

  /* get size of visible area in canvas units (meters) */
  canvas_dimensions dim = map->canvas->get_viewport_dimensions(canvas_t::UNIT_METER) / 2;

  // Data rect minimum and maximum
  // limit stops - prevent scrolling beyond these
  const bounds_t &bounds = map->appdata.project->osm->bounds;
  int min_sy_cu = 0.95 * (bounds.min.y - dim.height);
  int min_sx_cu = 0.95 * (bounds.min.x - dim.width);
  int max_sy_cu = 0.95 * (bounds.max.y + dim.height);
  int max_sx_cu = 0.95 * (bounds.max.x + dim.width);
  if (sy_cu < min_sy_cu)
    sy = min_sy_cu * scale;
  else if (sy_cu > max_sy_cu)
    sy = max_sy_cu * scale;
  if (sx_cu < min_sx_cu)
    sx = min_sx_cu * scale;
  else if (sx_cu > max_sx_cu)
    sx = max_sx_cu * scale;
}

/* Limit a proposed zoom factor to sane ranges.
 * Specifically the map is allowed to be no smaller than the viewport. */
static bool map_limit_zoom(map_t *map, double &zoom) {
    // Data rect minimum and maximum
    const bounds_t &bounds = map->appdata.project->osm->bounds;

    /* get size of visible area in pixels and convert to meters of intended */
    /* zoom by deviding by zoom (which is basically pix/m) */
    canvas_dimensions dim = map->canvas->get_viewport_dimensions(canvas_t::UNIT_PIXEL) / zoom;

    double oldzoom = zoom;
    if (dim.height < dim.width) {
      int lim_h = dim.height * 0.95;
      const int min_y = bounds.min.y;
      const int max_y = bounds.max.y;

      if (max_y-min_y < lim_h) {
          double corr = (static_cast<double>(max_y) - min_y) / lim_h;
          zoom /= corr;
      }
    } else {
      int lim_w = dim.width * 0.95;
      const int min_x = bounds.min.x;
      const int max_x = bounds.max.x;

      if (max_x-min_x < lim_w) {
          double corr = (static_cast<double>(max_x) - min_x) / lim_w;
          zoom /= corr;
      }
    }
    if (zoom != oldzoom) {
        printf("Can't zoom further out (%f)\n", zoom);
        return true;
    }
    return false;
}


/*
 * Scroll the map to a point if that point is currently offscreen.
 * Return true if this was possible, false if position is outside
 * working area
 */
bool map_t::scroll_to_if_offscreen(const lpos_t lpos) {

  // Ignore anything outside the working area
  if(unlikely(!appdata.project->osm))
    return false;

  if (!appdata.project->osm->bounds.contains(lpos)) {
    printf("cannot scroll to (%d, %d): outside the working area\n", lpos.x, lpos.y);
    return false;
  }

  // Viewport dimensions in canvas space

  /* get size of visible area in canvas units (meters) */
  double pix_per_meter = canvas->get_zoom();
  canvas_dimensions dim = canvas->get_viewport_dimensions(canvas_t::UNIT_METER);

  // Is the point still onscreen?
  bool recentre_needed = false;
  int sx, sy;
  canvas->scroll_get(canvas_t::UNIT_METER, sx, sy);
  int viewport_left   = sx - dim.width / 2;
  int viewport_right  = sx + dim.width / 2;
  int viewport_top    = sy - dim.height / 2;
  int viewport_bottom = sy + dim.height / 2;

  if (lpos.x > viewport_right) {
    printf("** off right edge (%d > %d)\n", lpos.x, viewport_right);
    recentre_needed = true;
  } else if (lpos.x < viewport_left) {
    printf("** off left edge (%d < %d)\n", lpos.x, viewport_left);
    recentre_needed = true;
  }
  if (lpos.y > viewport_bottom) {
    printf("** off bottom edge (%d > %d)\n", lpos.y, viewport_bottom);
    recentre_needed = true;
  } else if (lpos.y < viewport_top) {
    printf("** off top edge (%d < %d)\n", lpos.y, viewport_top);
    recentre_needed = true;
  }

  if(recentre_needed) {
    // Just centre both at once
    int new_sx = pix_per_meter * lpos.x; // XXX (lpos.x - (aw/2));
    int new_sy = pix_per_meter * lpos.y; // XXX (lpos.y - (ah/2));

    map_limit_scroll(this, canvas_t::UNIT_PIXEL, new_sx, new_sy);
    canvas->scroll_to(canvas_t::UNIT_PIXEL, new_sx, new_sy);
  }
  return true;
}

/* Deselects the current way or node if its zoom_max
 * means that it's not going to render at the current map zoom. */
static void map_deselect_if_zoom_below_zoom_max(map_t *map) {
  if (map->selected.object.type == object_t::WAY) {
    printf("will deselect way if zoomed below %f\n",
            map->selected.object.way->zoom_max);
    if (map->state.zoom < map->selected.object.way->zoom_max) {
      printf("  deselecting way!\n");
      map->item_deselect();
    }
  } else if (map->selected.object.type == object_t::NODE) {
    printf("will deselect node if zoomed below %f\n",
            map->selected.object.node->zoom_max);
    if (map->state.zoom < map->selected.object.node->zoom_max) {
      printf("  deselecting node!\n");
      map->item_deselect();
    }
  }
}

#define GPS_RADIUS_LIMIT  3.0

void map_t::set_zoom(double zoom, bool update_scroll_offsets) {
  bool at_zoom_limit = map_limit_zoom(this, zoom);

  state.zoom = zoom;
  canvas->set_zoom(state.zoom);

  map_deselect_if_zoom_below_zoom_max(this);

  if(update_scroll_offsets) {
    if (!at_zoom_limit) {
      /* zooming affects the scroll offsets */
      int sx, sy;
      canvas->scroll_get(canvas_t::UNIT_PIXEL, sx, sy);
      map_limit_scroll(this, canvas_t::UNIT_PIXEL, sx, sy);

      // keep the map visible
      canvas->scroll_to(canvas_t::UNIT_PIXEL, sx, sy);
    }

    canvas->scroll_get(canvas_t::UNIT_METER, state.scroll_offset.x,
                       state.scroll_offset.y);
  }

  if(gps_item) {
    float radius = style->track.width / 2.0;
    if(zoom < GPS_RADIUS_LIMIT) {
      radius *= GPS_RADIUS_LIMIT;
      radius /= zoom;

      gps_item->set_radius(radius);
    }
  }
}

static bool distance_above(map_t *map, int x, int y, int limit) {
  int sx, sy;

  /* add offsets generated by mouse within map and map scrolling */
  sx = (x-map->pen_down.at.x);
  sy = (y-map->pen_down.at.y);

  return sx*sx + sy*sy > limit*limit;
}

/* scroll with respect to two screen positions */
static void map_do_scroll(map_t *map, int x, int y) {
  int sx, sy;

  map->canvas->scroll_get(canvas_t::UNIT_PIXEL, sx, sy);
  sx -= x-map->pen_down.at.x;
  sy -= y-map->pen_down.at.y;
  map_limit_scroll(map, canvas_t::UNIT_PIXEL, sx, sy);
  map->canvas->scroll_to(canvas_t::UNIT_PIXEL, sx, sy);

  map->canvas->scroll_get(canvas_t::UNIT_METER,
                    map->state.scroll_offset.x,
                    map->state.scroll_offset.y);
}

/* scroll a certain step */
void map_t::scroll_step(int x, int y) {
  int sx, sy;
  canvas->scroll_get(canvas_t::UNIT_PIXEL, sx, sy);
  sx += x;
  sy += y;
  map_limit_scroll(this, canvas_t::UNIT_PIXEL, sx, sy);
  canvas->scroll_to(canvas_t::UNIT_PIXEL, sx, sy);

  canvas->scroll_get(canvas_t::UNIT_METER, state.scroll_offset.x, state.scroll_offset.y);
}

bool map_t::item_is_selected_node(const map_item_t *map_item) const {
  printf("check if item is a selected node\n");

  if(!map_item) {
    printf("  no item requested\n");
    return false;
  }

  /* clicked the highlight directly */
  if(map_item->object.type != object_t::NODE) {
    printf("  didn't click node\n");
    return false;
  }

  if(selected.object.type == object_t::NODE) {
    printf("  selected item is a node\n");

    return selected.object == map_item->object;
  } else if(selected.object.type == object_t::WAY) {
    printf("  selected item is a way\n");

    return selected.object.way->contains_node(map_item->object.node);
  } else {
    printf("  selected item is unknown\n");
    return false;
  }
}

/* return true if the item given is the currenly selected way */
/* also return false if nothing is selected or the selection is no way */
bool map_t::item_is_selected_way(const map_item_t *map_item) const {
  printf("check if item is the selected way\n");

  if(!map_item) {
    printf("  no item requested\n");
    return false;
  }

  if(selected.object.type != object_t::WAY) {
    printf("  selected item is not a way\n");
    return false;
  }

  return map_item->object == selected.object;
}

void map_t::highlight_refresh() {
  object_t old = selected.object;

  printf("type to refresh is %d\n", old.type);
  if(old.type == object_t::ILLEGAL)
    return;

  item_deselect();
  map_object_select(this, old);
}

static void map_handle_click(map_t *map) {

  /* problem: on_item may be the highlight itself! So store it! */
  map_item_t map_item;
  if(map->pen_down.on_item)
    map_item = *map->pen_down.on_item;

  /* if we aready have something selected, then de-select it */
  map->item_deselect();

  /* select the clicked item (if there was one) */
  if(map_item.object.type != object_t::ILLEGAL)
    map_object_select(map, map_item.object);
}

struct hl_nodes {
  const node_t * const cur_node;
  const lpos_t pos;
  map_t * const map;
  node_t *& res_node;
  hl_nodes(const node_t *c, lpos_t p, map_t *m, node_t *&rnode)
    : cur_node(c), pos(p), map(m), res_node(rnode) {}
  void operator()(const std::pair<item_id_t, node_t *> &p);
  void operator()(node_t *node);
};

void hl_nodes::operator()(const std::pair<item_id_t, node_t *> &p)
{
  node_t * const node = p.second;

  if((node != cur_node) && (!(node->flags & OSM_FLAG_DELETED)))
    operator()(node);
}

void hl_nodes::operator()(node_t* node)
{
  int nx = abs(pos.x - node->lpos.x);
  int ny = abs(pos.y - node->lpos.y);

  if(nx < map->style->node.radius && ny < map->style->node.radius &&
     (nx*nx + ny*ny) < map->style->node.radius * map->style->node.radius)
    res_node = node;
}

static void map_touchnode_update(map_t *map, int x, int y) {
  map->touchnode_clear();

  const node_t *cur_node = nullptr;

  /* the "current node" which is the one we are working on and which */
  /* should not be highlighted depends on the action */
  switch(map->action.type) {

    /* in idle mode the dragged node is not highlighted */
  case MAP_ACTION_IDLE:
    assert(map->pen_down.on_item != nullptr);
    assert_cmpnum(map->pen_down.on_item->object.type, object_t::NODE);
    cur_node = map->pen_down.on_item->object.node;
    break;

  default:
    break;
  }

  /* check if we are close to one of the other nodes */
  lpos_t pos = map->canvas->window2world(x, y);
  node_t *rnode = nullptr;
  hl_nodes fc(cur_node, pos, map, rnode);
  std::map<item_id_t, node_t *> &nodes = map->appdata.project->osm->nodes;
  std::for_each(nodes.begin(), nodes.end(), fc);

  if(rnode != nullptr) {
    delete map->touchnode;

    map->touchnode = map->canvas->circle_new(CANVAS_GROUP_DRAW, rnode->lpos.x, rnode->lpos.y,
                                             2 * map->style->node.radius, 0,
                                             map->style->highlight.touch_color, NO_COLOR);

    map->touchnode_node = rnode;
  }

  /* during way creation also nodes of the new way */
  /* need to be searched */
  if(!map->touchnode && map->action.way && map->action.way->node_chain.size() > 1) {
    const node_chain_t &chain = map->action.way->node_chain;
    std::for_each(chain.begin(), chain.end() - 1, fc);
  }
}

void map_t::button_press(float x, float y) {
  printf("left button pressed\n");
  pen_down.is = true;

  /* save press position */
  pen_down.at.x = x;
  pen_down.at.y = y;
  pen_down.drag = false;     // don't assume drag yet

  /* determine wether this press was on an item */
  pen_down_item();

  /* check if the clicked item is a highlighted node as the user */
  /* might want to drag that */
  pen_down.on_selected_node = item_is_selected_node(pen_down.on_item);

  /* button press */
  switch(action.type) {

  case MAP_ACTION_WAY_NODE_ADD:
    map_edit_way_node_add_highlight(this, pen_down.on_item, x, y);
    break;

  case MAP_ACTION_WAY_CUT:
    map_edit_way_cut_highlight(this, pen_down.on_item, x, y);
    break;

  case MAP_ACTION_NODE_ADD:
    map_hl_cursor_draw(this, x, y, style->node.radius);
    break;

  case MAP_ACTION_WAY_ADD:
    map_hl_cursor_draw(this, x, y, style->node.radius);
    map_touchnode_update(this, x, y);
    break;

  default:
    break;
  }
}

void map_t::button_release(int x, int y) {
  pen_down.is = false;

  /* before button release is handled */
  switch(action.type) {
  case MAP_ACTION_BG_ADJUST:
    bg_adjust(x, y);
    bg.offset.x += x - pen_down.at.x;
    bg.offset.y += y - pen_down.at.y;
    break;

  case MAP_ACTION_IDLE:
    /* check if distance to press is above drag limit */
    if(!pen_down.drag)
      pen_down.drag = distance_above(this, x, y, MAP_DRAG_LIMIT);

    if(!pen_down.drag) {
      printf("left button released after click\n");

      map_item_t old_sel = selected;
      map_handle_click(this);

      if(old_sel.object.type != object_t::ILLEGAL && old_sel.object == selected.object) {
        printf("re-selected same item of type %s, pushing it to the bottom\n",
               old_sel.object.type_string());
        if(selected.item == nullptr) {
          printf("  item has no visible representation to push\n");
        } else {
          selected.item->to_bottom();

          /* update clicked item, to correctly handle the click */
          pen_down_item();

          map_handle_click(this);
        }
      }
    } else {
      printf("left button released after drag\n");

      /* just scroll if we didn't drag an selected item */
      if(!pen_down.on_selected_node) {
        map_do_scroll(this, x, y);
      } else {
        printf("released after dragging node\n");
        map_hl_cursor_clear(this);

        /* now actually move the node */
        map_edit_node_move(this, pen_down.on_item, x, y);
      }
    }
    break;

  case MAP_ACTION_NODE_ADD: {
    printf("released after NODE ADD\n");
    map_hl_cursor_clear(this);

    /* convert mouse position to canvas (world) position */
    lpos_t pos = canvas->window2world(x, y);

    node_t *node = nullptr;
    osm_t::ref osm = appdata.project->osm;
    if(!osm->bounds.contains(pos))
      outside_error();
    else {
      node = osm->node_new(pos);
      osm->node_attach(node);
      draw(node);
    }
    set_action(MAP_ACTION_IDLE);

    item_deselect();

    if(node) {
      map_node_select(this, node);

      /* let the user specify some tags for the new node */
      info_dialog(appdata_t::window, this, osm, appdata.presets.get());
    }
    break;
  }
  case MAP_ACTION_WAY_ADD:
    printf("released after WAY ADD\n");
    map_hl_cursor_clear(this);

    map_edit_way_add_segment(this, x, y);
    break;

  case MAP_ACTION_WAY_NODE_ADD:
    printf("released after WAY NODE ADD\n");
    map_hl_cursor_clear(this);

    map_edit_way_node_add(this, x, y);
    break;

  case MAP_ACTION_WAY_CUT:
    printf("released after WAY CUT\n");
    map_hl_cursor_clear(this);

    map_edit_way_cut(this, x, y);
    break;

  default:
    break;
  }
}

void map_t::handle_motion(int x, int y)
{
  /* check if distance to press is above drag limit */
  if(!pen_down.drag)
    pen_down.drag = distance_above(this, x, y, MAP_DRAG_LIMIT);

  /* drag */
  switch(action.type) {
  case MAP_ACTION_BG_ADJUST:
    bg_adjust(x, y);
    break;

  case MAP_ACTION_IDLE:
    if(pen_down.drag) {
      /* just scroll if we didn't drag an selected item */
      if(!pen_down.on_selected_node)
        map_do_scroll(this, x, y);
      else {
        map_hl_cursor_draw(this, x, y, style->node.radius);
        map_touchnode_update(this, x, y);
      }
    }
    break;

  case MAP_ACTION_NODE_ADD:
    map_hl_cursor_draw(this, x, y, style->node.radius);
    break;

  case MAP_ACTION_WAY_ADD:
    map_hl_cursor_draw(this, x, y, style->node.radius);
    map_touchnode_update(this, x, y);
    break;

  case MAP_ACTION_WAY_NODE_ADD:
    map_hl_cursor_clear(this);
    map_edit_way_node_add_highlight(this, item_at(x, y), x, y);
    break;

  case MAP_ACTION_WAY_CUT:
    map_hl_cursor_clear(this);
    map_edit_way_cut_highlight(this, item_at(x, y), x, y);
    break;

  default:
    break;
  }
}

map_t::map_t(appdata_t &a, map_highlight_t &hl)
  : gps_item(nullptr)
  , appdata(a)
  , canvas(canvas_t::create())
  , state(appdata.map_state)
  , highlight(hl)
  , cursor(nullptr)
  , touchnode(nullptr)
  , touchnode_node(nullptr)
  , style(appdata.style)
  , elements_drawn(0)
{
  memset(&selected, 0, sizeof(selected));
  memset(&bg, 0, sizeof(bg));
  memset(&action, 0, sizeof(action));
  memset(&pen_down, 0, sizeof(pen_down));
  pen_down.at.x = -1;
  pen_down.at.y = -1;
  action.type = MAP_ACTION_IDLE;
}

map_t::~map_t()
{
  map_free_map_item_chains(appdata);
}

void map_t::init() {
  const bounds_t &bounds = appdata.project->osm->bounds;

  /* update canvas background color */
  set_bg_color_from_style();

  /* set initial zoom */
  set_zoom(state.zoom, false);
  paint();

  float mult = style->frisket.mult;
  canvas->set_bounds(mult * bounds.min.x, mult * bounds.min.y,
                     mult * bounds.max.x, mult * bounds.max.y);

  printf("restore scroll position %d/%d\n",
         state.scroll_offset.x, state.scroll_offset.y);

  map_limit_scroll(this, canvas_t::UNIT_METER,
                   state.scroll_offset.x, state.scroll_offset.y);
  canvas->scroll_to(canvas_t::UNIT_METER, state.scroll_offset.x, state.scroll_offset.y);
}


void map_t::clear(clearLayers layers) {
  printf("freeing map contents\n");

  unsigned int group_mask;
  switch(layers) {
  case MAP_LAYER_ALL:
    // add one so this is a usually illegal bitmask
    group_mask = ((1 << (CANVAS_GROUPS + 1)) - 1);
    remove_gps_position();
    break;
  case MAP_LAYER_OBJECTS_ONLY:
    group_mask = ((1 << CANVAS_GROUP_POLYGONS) |
                  (1 << CANVAS_GROUP_WAYS_HL) |
                  (1 << CANVAS_GROUP_WAYS_OL) |
                  (1 << CANVAS_GROUP_WAYS) |
                  (1 << CANVAS_GROUP_WAYS_INT) |
                  (1 << CANVAS_GROUP_NODES_HL) |
                  (1 << CANVAS_GROUP_NODES_IHL) |
                  (1 << CANVAS_GROUP_NODES) |
                  (1 << CANVAS_GROUP_WAYS_DIR));
    break;
  }

  map_free_map_item_chains(appdata);

  /* remove a possibly existing highlight */
  item_deselect();

  canvas->erase(group_mask);
}

void map_t::paint() {
  osm_t::ref osm = appdata.project->osm;

  style->colorize_world(osm);

  assert(canvas != nullptr);

  printf("drawing ways ...\n");
  std::for_each(osm->ways.begin(), osm->ways.end(), map_way_draw_functor(this));

  printf("drawing single nodes ...\n");
  std::for_each(osm->nodes.begin(), osm->nodes.end(), map_node_draw_functor(this));

  printf("drawing frisket...\n");
  map_frisket_draw(this, osm->bounds);
}

/* called from several icons like e.g. "node_add" */
void map_t::set_action(map_action_t act) {
  printf("map action set to %d\n", act);

  action.type = act;

  /* enable/disable ok/cancel buttons */
  // MAP_ACTION_IDLE=0, NODE_ADD, BG_ADJUST, WAY_ADD, WAY_NODE_ADD, WAY_CUT
  bool ok_state = false;
  bool cancel_state = true;

  const char *statusbar_text;
  bool idle = false;

  switch(act) {
  case MAP_ACTION_BG_ADJUST:
    statusbar_text = _("Adjust background image position");
    ok_state = true;
    /* an existing selection only causes confusion ... */
    item_deselect();
    break;

  case MAP_ACTION_WAY_ADD: {
    statusbar_text = _("Place first node of new way");
    printf("starting new way\n");

    item_deselect();
    map_edit_way_add_begin(this);
    break;
  }

  case MAP_ACTION_NODE_ADD:
    statusbar_text = _("Place a node");
    ok_state = true;
    item_deselect();
    break;

  case MAP_ACTION_IDLE:
    statusbar_text = nullptr;
    cancel_state = false;
    idle = true;
    break;

  case MAP_ACTION_WAY_CUT:
    statusbar_text = _("Select segment to cut way");
    break;

  case MAP_ACTION_WAY_NODE_ADD:
    statusbar_text = _("Place node on selected way");
    break;
  }

  appdata.iconbar->map_cancel_ok(cancel_state, ok_state);
  appdata.iconbar->map_action_idle(idle, selected.object);
  appdata.uicontrol->setActionEnable(MainUi::MENU_ITEM_WMS_ADJUST, idle);

  appdata.uicontrol->showNotification(statusbar_text);
}


void map_t::action_ok() {
  /* reset action now as this erases the statusbar and some */
  /* of the actions may set it */
  map_action_t type = action.type;
  set_action(MAP_ACTION_IDLE);

  switch(type) {
  case MAP_ACTION_WAY_ADD:
    map_edit_way_add_ok(this);
    break;

  case MAP_ACTION_BG_ADJUST:
    /* save changes to bg_offset in project */
    appdata.project->wms_offset.x = bg.offset.x;
    appdata.project->wms_offset.y = bg.offset.y;
    break;

  case MAP_ACTION_NODE_ADD:
    {
    pos_t pos = appdata.gps_state->get_pos();
    if(!pos.valid())
      break;

    node_t *node = nullptr;
    osm_t::ref osm = appdata.project->osm;

    if(!osm->bounds.ll.contains(pos)) {
      map_t::outside_error();
    } else {
      node = osm->node_new(pos);
      osm->node_attach(node);
      draw(node);
    }
    set_action(MAP_ACTION_IDLE);

    item_deselect();

    if(node) {
      map_node_select(this, node);

      /* let the user specify some tags for the new node */
      info_dialog(appdata_t::window, this, osm, appdata.presets.get());
    }
    }

  default:
    break;
  }
}

struct node_deleted_from_ways {
  map_t * const map;
  explicit node_deleted_from_ways(map_t *m) : map(m) { }
  void operator()(way_t *way);
};

/* redraw all affected ways */
void node_deleted_from_ways::operator()(way_t *way) {
  if(way->node_chain.size() == 1) {
    /* this way now only contains one node and thus isn't a valid */
    /* way anymore. So it'll also get deleted (which in turn may */
    /* cause other nodes to be deleted as well) */
    map->appdata.project->osm->way_delete(way);
  } else {
    object_t object(way);
    map->redraw_item(object);
  }
}

struct short_way {
  const node_t * const node;
  short_way(const node_t *n) : node(n) {}
  bool operator()(const std::pair<item_id_t, way_t *> &p) {
    const way_t *way = p.second;
    return way->node_chain.size() < 3 && way->contains_node(node);
  }
};

/* called from icon "trash" */
void map_t::delete_selected() {
  /* work on local copy since de-selecting destroys the selection */
  map_item_t item = selected;

  const char *objtype = item.object.type_string();
  g_string msgtitle(g_strdup_printf(_("Delete selected %s?"), objtype));
  if(!yes_no_f(nullptr, MISC_AGAIN_ID_DELETE | MISC_AGAIN_FLAG_DONT_SAVE_NO,
               msgtitle.get(), _("Do you really want to delete the selected %s?"), objtype))
    return;

  msgtitle.reset();

  /* deleting the selected item de-selects it ... */
  item_deselect();

  printf("request to delete %s #" ITEM_ID_FORMAT "\n",
         objtype, item.object.obj->id);

  osm_t::ref osm = appdata.project->osm;
  switch(item.object.type) {
  case object_t::NODE: {
    /* check if this node is part of a way with two nodes only. */
    /* we cannot delete this as this would also delete the way */
    if(osm->find_way(short_way(item.object.node)) != nullptr &&
       !yes_no_f(nullptr, 0, _("Delete node in short way(s)?"),
                 _("Deleting this node will also delete one or more ways "
                   "since they'll contain only one node afterwards. "
                   "Do you really want this?")))
      return;

    /* and mark it "deleted" in the database */
    const way_chain_t &chain = osm->node_delete(item.object.node);
    std::for_each(chain.begin(), chain.end(), node_deleted_from_ways(this));

    break;
  }

  case object_t::WAY:
    osm->way_delete(item.object.way);
    break;

  case object_t::RELATION:
    osm->relation_delete(item.object.relation);
    break;

  default:
    assert_unreachable();
  }
}

/* ----------------------- track related stuff ----------------------- */

/**
 * @brief allocate a point array and initialize it with screen coordinates
 * @param bounds screen boundary
 * @param point first track point to use
 * @param count number of points to use
 * @return point array
 */
static std::vector<lpos_t> canvas_points_init(const bounds_t &bounds,
                                           std::vector<track_point_t>::const_iterator point,
                                           const unsigned int count) {
  std::vector<lpos_t> points;
  points.reserve(count);
  lpos_t lpos;

  for(unsigned int i = 0; i < count; i++) {
    points.push_back(point->pos.toLpos(bounds));
    point++;
  }

  return points;
}

void map_t::track_draw_seg(track_seg_t &seg) {
  const bounds_t &bounds = appdata.project->osm->bounds;

  /* a track_seg needs at least 2 points to be drawn */
  if (seg.track_points.empty())
    return;

  /* nothing should have been drawn by now ... */
  assert(seg.item_chain.empty());

  const std::vector<track_point_t>::const_iterator itEnd = seg.track_points.end();
  std::vector<track_point_t>::const_iterator it = seg.track_points.begin();
  while(it != itEnd) {
    /* skip all points not on screen */
    std::vector<track_point_t>::const_iterator last = itEnd;
    while(it != itEnd && !bounds.ll.contains(it->pos)) {
      last = it;
      it++;
    }

    if(it == itEnd) {
      // the segment ends in a segment that is not on screen
      elements_drawn = 0;
      return;
    }

    unsigned int visible = 0;

    /* count nodes that _are_ on screen */
    std::vector<track_point_t>::const_iterator tmp = it;
    while(tmp != itEnd && bounds.ll.contains(tmp->pos)) {
      tmp++;
      visible++;
    }

    /* the last element is still on screen, so save the number of elements in
     * the point list to avoid recalculation on update */
    if(tmp == itEnd)
      elements_drawn = visible;

    /* actually start drawing with the last position that was offscreen */
    /* so the track nicely enters the viewing area */
    if(last != itEnd) {
      it = last;
      visible++;
    }

    /* also use last one that's offscreen to nicely leave the visible area */
    /* also determine the first item to use in the next loop */
    if(tmp != itEnd && tmp + 1 != itEnd) {
      visible++;
      tmp++;
    } else {
      tmp = itEnd;
    }

    /* allocate space for nodes */
    printf("visible are %u\n", visible);
    std::vector<lpos_t> points = canvas_points_init(bounds, it, visible);
    it = tmp;

    canvas_item_t *item = canvas->polyline_new(CANVAS_GROUP_TRACK, points,
                                               style->track.width, style->track.color);
    seg.item_chain.push_back(item);
  }
}

/* update the last visible fragment of this segment since a */
/* gps position may have been added */
void map_t::track_update_seg(track_seg_t &seg) {
  const bounds_t &bounds = appdata.project->osm->bounds;

  printf("-- APPENDING TO TRACK --\n");

  /* there are two cases: either the second last point was on screen */
  /* or it wasn't. We'll have to start a new screen item if the latter */
  /* is the case */

  /* search last point */
  const std::vector<track_point_t>::const_iterator itEnd = seg.track_points.end();
  std::vector<track_point_t>::const_iterator last = itEnd - 1;
  /* check if the last and second_last points are visible */
  const bool last_is_visible = bounds.ll.contains(last->pos);
  const bool second_last_is_visible = (elements_drawn > 0);

  /* if both are invisible, then nothing has changed on screen */
  if(!last_is_visible && !second_last_is_visible) {
    printf("second_last and last entry are invisible -> doing nothing\n");
    elements_drawn = 0;
    return;
  }

  const std::vector<track_point_t>::const_iterator begin = // start of track to draw
                                                   second_last_is_visible
                                                   ? itEnd - elements_drawn - 1
                                                   : itEnd - 2;

  /* since we are updating an existing track, it sure has at least two
   * points, second_last must be valid and its "next" (last) also */
  assert(begin != itEnd);
  assert(last != itEnd);
  assert_cmpnum_op(seg.track_points.size(), >=, itEnd - begin);

  /* count points to be placed */
  const size_t npoints = itEnd - begin;
  elements_drawn = last_is_visible ? npoints : 0;

  lpos_t lpos = last->pos.toLpos(bounds);
  lpos_t lpos2 = (last - 1)->pos.toLpos(bounds);
  /* if both items appear on the screen in the same position (e.g. because they are
   * close to each other and a low zoom level) don't redraw as nothing would change
   * visually. */
  if(lpos == lpos2)
    return;

  std::vector<lpos_t> points = canvas_points_init(bounds, begin, npoints);

  if(second_last_is_visible) {
    /* there must be something already on the screen and there must */
    /* be visible nodes in the chain */
    assert(!seg.item_chain.empty());

    printf("second_last is visible -> updating last segment to %zu points\n", npoints);

    static_cast<canvas_item_polyline *>(seg.item_chain.back())->set_points(points);
  } else {
    assert(begin + 1 == last);
    assert(last_is_visible);

    printf("second last is invisible -> start new screen segment with %zu points\n", npoints);

    canvas_item_t *item = canvas->polyline_new(CANVAS_GROUP_TRACK, points,
                                               style->track.width, style->track.color);
    seg.item_chain.push_back(item);
  }
}

struct map_track_seg_draw_functor {
  map_t * const map;
  explicit map_track_seg_draw_functor(map_t *m) : map(m) {}
  void operator()(track_seg_t &seg) {
    map->track_draw_seg(seg);
  }
};

void map_t::track_draw(TrackVisibility visibility, track_t &track) {
  if(unlikely(track.segments.empty()))
    return;

  track.clear();
  if(visibility < ShowPosition)
    remove_gps_position();

  canvas->erase(1 << CANVAS_GROUP_TRACK);

  switch(visibility) {
  case DrawAll:
    std::for_each(track.segments.begin(), track.segments.end(),
                  map_track_seg_draw_functor(this));
    break;
  case DrawCurrent:
    if(track.active)
      track_draw_seg(track.segments.back());
    break;
  default:
    break;
  }
}

void track_t::clear() {
  printf("removing track\n");

  std::for_each(segments.begin(), segments.end(), free_track_item_chain<true>);
}

/**
 * @brief show the marker item for the current GPS position
 */
void map_t::track_pos(const lpos_t lpos) {
  /* remove the old item */
  remove_gps_position();

  float radius = style->track.width / 2.0;
  double zoom = canvas->get_zoom();
  if(zoom < GPS_RADIUS_LIMIT) {
    radius *= GPS_RADIUS_LIMIT;
    radius /= zoom;
  }

  gps_item = canvas->circle_new(CANVAS_GROUP_GPS, lpos.x, lpos.y, radius, 0,
                                style->track.gps_color, NO_COLOR);
}

/**
 * @brief remove the marker item for the current GPS position
 */
void map_t::remove_gps_position() {
  delete gps_item;
  gps_item = nullptr;
}

/* ------------------- map background ------------------ */

void map_t::set_bg_color_from_style()
{
  canvas->set_background(style->background.color);
}

/* -------- hide and show objects (for performance reasons) ------- */

void map_t::hide_selected() {
  if(selected.object.type != object_t::WAY) {
    printf("selected item is not a way\n");
    return;
  }

  way_t *way = selected.object.way;
  printf("hiding way #" ITEM_ID_FORMAT "\n", way->id);

  item_deselect();
  way->flags |= OSM_FLAG_HIDDEN;
  way->item_chain_destroy();

  appdata.uicontrol->setActionEnable(MainUi::MENU_ITEM_MAP_SHOW_ALL, true);
}

struct map_show_all_functor {
  map_t * const map;
  explicit map_show_all_functor(map_t *m) : map(m) {}
  void operator()(std::pair<item_id_t, way_t *> pair);
};

void map_show_all_functor::operator()(std::pair<item_id_t, way_t *> pair)
{
  way_t * const way = pair.second;
  if(way->flags & OSM_FLAG_HIDDEN) {
    way->flags &= ~OSM_FLAG_HIDDEN;
    map->draw(way);
  }
}

void map_t::show_all() {
  std::map<item_id_t, way_t *> &ways = appdata.project->osm->ways;
  std::for_each(ways.begin(), ways.end(), map_show_all_functor(this));

  appdata.uicontrol->setActionEnable(MainUi::MENU_ITEM_MAP_SHOW_ALL, false);
}

void map_t::detail_change(float detail, const char *banner_msg) {
  if(banner_msg)
    appdata.uicontrol->showNotification(banner_msg, MainUi::Busy);
  /* deselecting anything allows us not to care about automatic deselection */
  /* as well as items becoming invisible by the detail change */
  item_deselect();

  state.detail = detail;
  printf("changing detail factor to %f\n", state.detail);

  clear(MAP_LAYER_OBJECTS_ONLY);
  paint();
  if(banner_msg)
    appdata.uicontrol->showNotification(nullptr, MainUi::Busy);
}

void map_t::detail_increase() {
  detail_change(state.detail * MAP_DETAIL_STEP, _("Increasing detail level"));
}

void map_t::detail_decrease() {
  detail_change(state.detail / MAP_DETAIL_STEP, _("Decreasing detail level"));
}

void map_t::detail_normal() {
  detail_change(1.0, _("Restoring default detail level"));
}

node_t *map_t::touchnode_get_node() {
  if(touchnode == nullptr)
    return nullptr;
  node_t *ret = touchnode_node;
  touchnode_clear();
  return ret;
}

void map_t::touchnode_clear() {
  delete touchnode;
  touchnode = nullptr;
  touchnode_node = nullptr;
}

map_state_t::map_state_t()
{
  reset();
}

void map_state_t::reset() {
  zoom = 0.25;
  detail = 1.0;

  scroll_offset.x = 0;
  scroll_offset.y = 0;
}

// vim:et:ts=8:sw=2:sts=2:ai
