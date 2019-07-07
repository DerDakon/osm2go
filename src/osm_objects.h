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

#pragma once

#include "osm.h"
#include "pos.h"

#include <algorithm>
#include <string>
#include <vector>

class tag_t {
  friend struct elemstyle_condition_t;

  tag_t() O2G_DELETED_FUNCTION;
  inline explicit tag_t(const char *k, const char *v, bool)
    : key(k), value(v) {}

  static const char *mapToCache(const char *v);
public:
  const char *key, *value;
  tag_t(const char *k, const char *v);

  /**
   * @brief return a tag_t where key and value are not backed by the value cache
   */
  static inline tag_t uncached(const char *k, const char *v)
  {
    return tag_t(k, v, true);
  }

  inline bool is_creator_tag() const noexcept
  { return is_creator_tag(key); }

  static bool is_creator_tag(const char *key) noexcept;
  static inline bool is_no_creator(const tag_t &tag) noexcept
  { return !is_creator_tag(tag.key); }

  /**
   * @brief compare if the keys are identical
   *
   * This is intended to compare to values already mapped to the value cache
   * so a simple pointer compare is enough.
   */
  inline bool key_compare(const char *k) const noexcept
  {
    return key == k;
  }

  /**
   * @brief compare if the values are identical
   *
   * This is intended to compare to values already mapped to the value cache
   * so a simple pointer compare is enough.
   */
  inline bool value_compare(const char *k) const noexcept
  {
    return value == k;
  }
};

class tag_list_t {
public:
  inline tag_list_t() noexcept : contents(nullptr) {}
  ~tag_list_t();

  /**
   * @brief check if any tags are present
   */
  bool empty() const noexcept;

  /**
   * @brief check if any tag that is not "created_by" is present
   */
  bool hasNonCreatorTags() const noexcept;

  /**
   * @brief check if any tag that is not "created_by" or "source" is present
   */
  bool hasRealTags() const noexcept;

  /**
   * @brief scan for the only non-trivial tag of this object
   * @returns tag if there is only one tag present that satisfies hasRealTags()
   * @retval nullptr either hasRealTags() is false or there are multiple tags on this object
   */
  const tag_t *singleTag() const noexcept;

  const char *get_value(const char *key) const;

  template<typename _Predicate>
  bool contains(_Predicate pred) const {
    if(!contents)
      return false;
    const std::vector<tag_t>::const_iterator itEnd = contents->end();
    return itEnd != std::find_if(std::cbegin(*contents), itEnd, pred);
  }

  template<typename _Predicate>
  void for_each(_Predicate pred) const {
    if(contents)
      std::for_each(contents->begin(), contents->end(), pred);
  }

  /**
   * @brief remove all elements and free their memory
   */
  void clear();

  /**
   * @brief copy the contained tags
   */
  osm_t::TagMap asMap() const;

  void copy(const tag_list_t &other);

  /**
   * @brief replace the current tags with the given ones
   * @param ntags array of new tags
   *
   * The contents of ntags are undefined afterwards for C++98.
   */
#if __cplusplus < 201103L
  void replace(std::vector<tag_t> &ntags);
#else
  void replace(std::vector<tag_t> &&ntags);
#endif

  /**
   * @brief replace the current tags with the given ones
   * @param ntags new tags
   */
  void replace(const osm_t::TagMap &ntags);

  /**
   * @brief combine tags from both lists in a useful manner
   * @return if there were any tag collisions
   *
   * other will be empty afterwards.
   */
  bool merge(tag_list_t &other);

  inline bool operator==(const std::vector<tag_t> &t2) const
  { return !operator!=(t2); }
  bool operator!=(const std::vector<tag_t> &t2) const;
  inline bool operator==(const osm_t::TagMap &t2) const
  { return !operator!=(t2); }
  bool operator!=(const osm_t::TagMap &t2) const;

  /**
   * @brief check if 2 tags with the same key exist
   */
  bool hasTagCollisions() const;

private:
  // do not directly use a vector here as many objects do not have
  // any tags and that would waste too much memory
  std::vector<tag_t> *contents;
};

class base_object_t {
public:
  explicit base_object_t(unsigned int ver = 0, item_id_t i = ID_ILLEGAL) noexcept;

  item_id_t id;
  tag_list_t tags;
  time_t time;
  unsigned int flags;
  int user;
  unsigned int version;

  /**
   * @brief replace the tags and set dirty flag if they were actually different
   * @param ntags the new tags
   *
   * "created_by" tags are ignored when considering if the list needs to be
   * changed or not.
   */
  void updateTags(const osm_t::TagMap &ntags);

  xmlChar *generate_xml(const std::string &changeset) const;

  /**
   * @brief get the API string for this object type
   * @return the string used for this kind of object in the OSM API
   */
  virtual const char *apiString() const noexcept = 0;

  std::string id_string() const;

  inline bool isNew() const noexcept
  { return id <= ID_ILLEGAL; }

  inline bool isDirty() const noexcept
  { return flags != 0; }

  inline bool isDeleted() const noexcept
  { return flags & OSM_FLAG_DELETED; }

  /**
   * @brief generate the xml elements for an osmChange delete section
   * @param parent_node the "delete" node of the osmChange document
   * @param changeset a string for the changeset attribute
   *
   * May only be called if this element is marked as deleted
   */
  void osmchange_delete(xmlNodePtr parent_node, const char *changeset) const;

  void markDeleted();
protected:
  virtual void generate_xml_custom(xmlNodePtr xml_node) const = 0;
};

class visible_item_t : public base_object_t {
protected:
  inline visible_item_t(unsigned int ver = 0, item_id_t i = ID_ILLEGAL) noexcept
    : base_object_t(ver, i)
    , map_item(nullptr)
    , zoom_max(0.0f)
  {
  }

public:
  /* a link to the visual representation on screen */
  struct map_item_t *map_item;

  float zoom_max;

  /**
   * @brief destroy the visible items
   * @param map the map pointer needed to release additional items
   *
   * It is known that there are no additional items the map pointer
   * may be nullptr.
   */
  void item_chain_destroy(map_t *map);
};

class node_t : public visible_item_t {
public:
  node_t(unsigned int ver, const lpos_t lp, const pos_t &p) noexcept;
  node_t(unsigned int ver, const pos_t &p, item_id_t i) noexcept;
  virtual ~node_t() {}

  unsigned int ways;
  pos_t pos;
  lpos_t lpos;

  const char *apiString() const noexcept override {
    return api_string();
  }
  static const char *api_string() noexcept {
    return "node";
  }
protected:
  void generate_xml_custom(xmlNodePtr xml_node) const override;
};

typedef std::vector<node_t *> node_chain_t;

#define OSM_DRAW_FLAG_AREA  (1<<0)
#define OSM_DRAW_FLAG_BG    (1<<1)

class way_t : public visible_item_t {
  friend class osm_t;
public:
  explicit way_t();
  explicit way_t(unsigned int ver, item_id_t i = ID_ILLEGAL);
  virtual ~way_t() {}

  /* visual representation from elemstyle */
  struct {
    color_t color;
    unsigned int flags : 8;
    int width : 8;
    unsigned int dash_length_on: 8;
    unsigned int dash_length_off: 8;

    union {
      struct {
        unsigned int color;
        int width;
      } bg;

      struct {
        unsigned int color;
      } area;
    };
  } draw;

  node_chain_t node_chain;

  bool contains_node(const node_t *node) const;
  void append_node(node_t *node);
  bool ends_with_node(const node_t *node) const noexcept;
  bool is_closed() const noexcept;
  bool is_area() const;

  void reverse(osm_t::ref osm, unsigned int &tags_flipped, unsigned int &roles_flipped);

  /**
   * @brief split the way into 2
   * @param osm parent osm object
   * @param cut_at position to split at
   * @param cut_at_node if split should happen before or at the given node
   * @returns the new way
   * @retval nullptr the new way would have only one node
   *
   * The returned way will be the shorter of the 2 new ways.
   *
   * @cut_at denotes the first node that is part of the second way. In case
   * @cut_at_node is true this is also the last node of the first way.
   *
   * In case the way is closed @cut_at denotes the first way of the node
   * after splitting. @cut_at_node has no effect in this case.
   */
  way_t *split(osm_t::ref osm, node_chain_t::iterator cut_at, bool cut_at_node);
  const node_t *last_node() const noexcept;
  const node_t *first_node() const noexcept;
  void write_node_chain(xmlNodePtr way_node) const;

  void cleanup();

  const char *apiString() const noexcept override {
    return api_string();
  }
  static const char *api_string() noexcept {
    return "way";
  }

  /**
   * @brief create a node and insert it into this way
   * @param osm the OSM object database
   * @param position the index in the node chain to insert the node
   * @param coords the coordinates of the new node
   * @returns the new node, already attached to osm
   */
  node_t *insert_node(osm_t::ref osm, int position, lpos_t coords);

private:
  bool merge(way_t *other, osm_t *osm, map_t *map, const std::vector<relation_t *> &rels = std::vector<relation_t *>());
public:
  /**
   * @brief merge this way with the other one
   * @param other the way to take the nodes from
   * @param osm map database
   * @param rels the relations that need to be adjusted
   * @returns if merging the tags caused collisions
   *
   * @other will be removed.
   */
  inline bool merge(way_t *other, osm_t::ref osm, map_t *map, const std::vector<relation_t *> &rels = std::vector<relation_t *>())
  { return merge(other, osm.get(), map, rels); }

protected:
  void generate_xml_custom(xmlNodePtr xml_node) const override {
    write_node_chain(xml_node);
  }
};

class relation_t : public base_object_t {
public:
  explicit relation_t();
  explicit relation_t(unsigned int ver, item_id_t i = ID_ILLEGAL);
  virtual ~relation_t() {}

  std::vector<member_t> members;

  std::vector<member_t>::iterator find_member_object(const object_t &o);
  inline std::vector<member_t>::const_iterator find_member_object(const object_t &o) const
  { return const_cast<relation_t *>(this)->find_member_object(o); }

  void members_by_type(unsigned int &nodes, unsigned int &ways, unsigned int &relations) const;
  std::string descriptive_name() const;
  void generate_member_xml(xmlNodePtr xml_node) const;

  bool is_multipolygon() const;

  void cleanup();

  const char *apiString() const noexcept override {
    return api_string();
  }
  static const char *api_string() noexcept {
    return "relation";
  }
  void remove_member(std::vector<member_t>::iterator it);
protected:
  void generate_xml_custom(xmlNodePtr xml_node) const override {
    generate_member_xml(xml_node);
  }
};

void osm_node_chain_free(node_chain_t &node_chain);