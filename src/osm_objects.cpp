/*
 * SPDX-FileCopyrightText: 2008 Till Harbaum <till@harbaum.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _DEFAULT_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "osm_objects.h"

#include "discarded.h"
#include "osm_p.h"

#include "osm2go_annotations.h"
#include <osm2go_cpp.h>
#include <osm2go_i18n.h>
#include <osm2go_platform.h>

static_assert(sizeof(tag_list_t) == sizeof(tag_t *), "tag_list_t is not exactly as big as a pointer");

const char *tag_t::mapToCache(const char *v)
{
  return value_cache.insert(v);
}

tag_t::tag_t(const char *k, const char *v)
  : key(mapToCache(k))
  , value(mapToCache(v))
{
}

namespace {

struct find_discardable_key {
  explicit find_discardable_key(const char *k) : key(k) {}
  const char * const key;

  inline bool operator()(const char *otherkey) const
  {
    return strcmp(key, otherkey) == 0;
  }
};

} // namespace

bool tag_t::is_discardable(const char *key) noexcept
{
  return std::any_of(discardable_tags.begin(), discardable_tags.end(), find_discardable_key(key));
}

bool
tag_list_t::operator==(const tag_list_t &other) const
{
  if (other.empty())
    return empty();

  // now it is safe to dereference as the vector must exist and can't be empty
  return operator==(*other.contents);
}

bool tag_list_t::empty() const noexcept
{
  return !contents || contents->empty();
}

bool tag_list_t::hasNonDiscardableTags() const noexcept
{
  if(empty())
    return false;

  const std::vector<tag_t>::const_iterator itEnd = contents->end();
  return std::any_of(std::cbegin(*contents), itEnd, tag_t::is_non_discardable);
}

static bool isRealTag(const tag_t &tag)
{
  return !tag.is_discardable() && strcmp(tag.key, "source") != 0;
}

bool tag_list_t::hasRealTags() const noexcept
{
  if(empty())
    return false;

  const std::vector<tag_t>::const_iterator itEnd = contents->end();
  return std::any_of(std::cbegin(*contents), itEnd, isRealTag);
}

const tag_t *tag_list_t::singleTag() const noexcept
{
  if(unlikely(empty()))
    return nullptr;

  const std::vector<tag_t>::const_iterator itEnd = contents->end();
  const std::vector<tag_t>::const_iterator it = std::find_if(std::cbegin(*contents), itEnd, isRealTag);
  if(unlikely(it == itEnd))
    return nullptr;
  if (std::any_of(std::next(it), itEnd, isRealTag))
    return nullptr;

  return &(*it);
}

namespace {

class key_match_functor {
  const char * const key;
public:
  explicit inline key_match_functor(const char *k) : key(k) {}
  inline bool operator()(const tag_t &tag) const {
    return tag.key_compare(key);
  }
};

} // namespace

const char* tag_list_t::get_value(const char *key) const
{
  if(empty())
    return nullptr;

  const char *cacheKey = value_cache.getValue(key);
  // if the key is not in the cache then it is used nowhere
  if(unlikely(cacheKey == nullptr))
    return nullptr;

  const std::vector<tag_t>::const_iterator itEnd = contents->end();
  const std::vector<tag_t>::const_iterator it = std::find_if(std::cbegin(*contents),
                                                             itEnd, key_match_functor(cacheKey));
  if(it != itEnd)
    return it->value;

  return nullptr;
}

#if __cplusplus < 201103L
// workaround for the fact that the default constructor is unavailable
template<>
inline void shrink_to_fit(std::vector<tag_t> &v)
{
  size_t sz = v.size();
  if(v.capacity() == sz)
    return;
  std::vector<tag_t> tmp(sz, tag_t::uncached(nullptr, nullptr));
  tmp = v;
  tmp.swap(v);
}

void tag_list_t::replace(std::vector<tag_t> &ntags)
#else
void tag_list_t::replace(std::vector<tag_t> &&ntags)
#endif
{
  if(ntags.empty()) {
    clear();
    return;
  }

  if(!contents) {
#if __cplusplus >= 201103L
    contents.reset(new std::vector<tag_t>(std::move(ntags)));
#else
    contents.reset(new std::vector<tag_t>(ntags.size(), tag_t::uncached(nullptr, nullptr)));
    contents->swap(ntags);
#endif
  } else {
#if __cplusplus >= 201103L
    *contents = std::move(ntags);
#else
    contents->swap(ntags);
#endif
  }

  shrink_to_fit(*contents);
}

namespace {

class tag_fill_functor {
  std::vector<tag_t> &tags;
public:
  explicit inline tag_fill_functor(std::vector<tag_t> &t) : tags(t) {}
  void operator()(const osm_t::TagMap::value_type &p) {
    if(unlikely(tag_t::is_discardable(p.first.c_str())))
      return;

    tags.push_back(tag_t(p.first.c_str(), p.second.c_str()));
  }
};

} // namespace

void tag_list_t::replace(const osm_t::TagMap &ntags)
{
  clear();
  if(ntags.empty())
    return;

  contents.reset(new std::vector<tag_t>());
  contents->reserve(ntags.size());
  std::for_each(ntags.begin(), ntags.end(), tag_fill_functor(*contents));
}

base_object_t::base_object_t(const base_attributes &attr) noexcept
  : base_attributes(attr)
  , flags(version == 0 ? OSM_FLAG_DIRTY : 0)
{
  assert((version == 0) == (id <= ID_ILLEGAL));
}

base_object_t::base_object_t(const base_object_t &other)
  : base_attributes(other)
  , flags(other.flags)
{
  tags.copy(other.tags);
}

std::string base_object_t::id_string() const {
  return std::to_string(id);
}

void base_object_t::osmchange_delete(xmlNodePtr parent_node, const char *changeset) const
{
  assert(isDeleted());

  xmlNodePtr obj_node = xmlNewChild(parent_node, nullptr, BAD_CAST apiString(), nullptr);

  xmlNewProp(obj_node, BAD_CAST "id", BAD_CAST id_string().c_str());

  char buf[32];
  snprintf(buf, sizeof(buf), "%u", version);

  xmlNewProp(obj_node, BAD_CAST "version", BAD_CAST buf);
  xmlNewProp(obj_node, BAD_CAST "changeset", BAD_CAST changeset);
}

bool way_t::operator==(const way_t &other) const
{
  if (!visible_item_t::operator==(other))
    return false;

  if (node_chain.size() != other.node_chain.size())
    return false;

  const node_chain_t::const_iterator itEnd = node_chain.end();
  node_chain_t::const_iterator it = node_chain.begin();
  node_chain_t::const_iterator oit = other.node_chain.begin();

  // comparing the ids is enough to see if the ways as such are identical
  for (; it != itEnd; it++, oit++)
    if ((*it)->id != (*oit)->id)
      return false;

  return true;
}

bool way_t::contains_node(const node_t *node) const
{
  /* return true if node is part of way */
  return std::find(node_chain.begin(), node_chain.end(), node) != node_chain.end();
}

void way_t::append_node(node_t *node) {
  node_chain.push_back(node);
  node->ways++;
}

bool way_t::ends_with_node(const node_t *node) const noexcept
{
  /* and deleted way may even not contain any nodes at all */
  /* so ignore it */
  if(isDeleted())
    return false;

  /* any valid way must have at least two nodes */
  assert(!node_chain.empty());

  return node_chain.front() == node ||
         node_chain.back()  == node;
}

node_t *way_t::insert_node(osm_t::ref osm, unsigned int position, lpos_t coords)
{
  node_t *node = osm->node_new(coords);
  osm->attach(node);

  osm->mark_dirty(this);

  /* search correct position */
  node_chain.insert(std::next(node_chain.begin(), position), node);

  /* remember that this node is contained in one way */
  node->ways = 1;

  return node;
}

bool way_t::merge(way_t *other, osm_t *osm, map_t *map, const std::vector<relation_t *> &rels)
{
  printf("  request to extend way #" ITEM_ID_FORMAT "\n", other->id);

  // drop the visible items
  other->item_chain_destroy(map);

  assert(ends_with_node(other->node_chain.front()) ||
           ends_with_node(other->node_chain.back()));

  osm->mark_dirty(this);
  osm->mark_dirty(other);

  const bool collision = tags.merge(other->tags);

  /* make enough room for all nodes */
  node_chain.reserve(node_chain.size() + other->node_chain.size() - 1);

  if(other->node_chain.front() == node_chain.front()) {
    printf("  need to prepend\n");
    node_chain.insert(node_chain.begin(), other->node_chain.rbegin(), std::prev(other->node_chain.rend()));

    other->node_chain.resize(1);
  } else if(other->node_chain.back() == node_chain.front()) {
    printf("  need to prepend\n");
    node_chain.insert(node_chain.begin(), other->node_chain.begin(), std::prev(other->node_chain.end()));

    other->node_chain.erase(other->node_chain.begin(), std::prev(other->node_chain.end()));
  } else if(other->node_chain.back() == node_chain.back()) {
    printf("  need to append\n");
    node_chain.insert(node_chain.end(), std::next(other->node_chain.rbegin()), other->node_chain.rend());

    other->node_chain.erase(other->node_chain.begin(), std::prev(other->node_chain.end()));
  } else {
    printf("  need to append\n");
    node_chain.insert(node_chain.end(), std::next(other->node_chain.begin()), other->node_chain.end());

    other->node_chain.resize(1);
  }

  /* replace "other" in relations */
  std::for_each(rels.begin(), rels.end(),
                relation_object_replacer(osm, object_t(other), object_t(this)));

  /* erase and free other way (now only containing the overlapping node anymore) */
  osm->way_delete(other, map);

  return collision;
}

std::vector<member_t>::const_iterator
relation_t::find_member_object(const object_t &o, std::vector<member_t>::const_iterator it) const
{
  return std::find_if(it, members.end(), find_member_object_functor(o));
}

unsigned int
relation_t::objectMembershipState(const object_t &obj, const relation_t *orig) const
{
  const std::vector<member_t>::const_iterator newMembership = find_member_object(obj);
  const std::vector<member_t>::const_iterator newEnd = members.end();
  const bool isNewMember = (newMembership != newEnd);

  if (orig == nullptr) {
    if (!isNewMember)
      return MembershipUnmodified;

    // consider new relations as always modified
    if (isNew())
      return newMembership->role == nullptr ?
             MembershipChanged :
             MembershipChanged | RoleChanged;

    return MembershipUnmodified;
  }

  const std::vector<member_t>::const_iterator oldMembership = orig->find_member_object(obj);
  const std::vector<member_t>::const_iterator oldEnd = orig->members.end();
  const bool isOldMember = (oldMembership != oldEnd);

  // not in any, everything is fine
  if (!isNewMember && !isOldMember)
    return MembershipUnmodified;

  // membership changed
  if (isNewMember != isOldMember) {
    // the role changed if either new or old role is checked
    if ((isOldMember && oldMembership->role != nullptr) ||
        (isNewMember && newMembership->role != nullptr))
      return MembershipChanged | RoleChanged;
    return MembershipChanged;
  }

  unsigned int ret = (newMembership->role != oldMembership->role) ? RoleChanged : MembershipUnmodified;

  // Now things get nasty. If one has a relation containing object O as this:
  //    O, role forward
  //    O, role forward
  // If you delete the first membership nothing seems to have changed. So now
  // the remaining entries have to be checked, and if their number has changed
  // this is considered a membership change.
  const find_member_object_functor fc(obj);
  if (std::count_if(std::next(newMembership), newEnd, fc) !=
      std::count_if(std::next(oldMembership), oldEnd, fc))
    ret |= MembershipChanged;

  return ret;
}

void relation_t::updateMembers(std::vector<member_t> &newMembers, osm_t::ref osm)
{
  if (newMembers == members)
    return;

  assert(std::find(newMembers.begin(), newMembers.end(), member_t(object_t())) == newMembers.end());

  const relation_t * const orig = osm->originalObject(this);
  if (orig == nullptr) {
    // members have changed and it wasn't dirty before, so it must be dirty now
    osm->mark_dirty(this);
    members.swap(newMembers);
    return;
  }

  // the object is already marked dirty, so we can modify at will
  members.swap(newMembers);

  // everything back to normal
  if (*this == *orig)
    osm->unmark_dirty(this);
}

namespace {

class member_counter {
  relation_t::memberCounts &counts;
public:
  inline member_counter(relation_t::memberCounts &c) : counts(c) {}
  void operator()(const member_t &member) noexcept;
};

void member_counter::operator()(const member_t &member) noexcept
{
  switch(member.object.type) {
  case object_t::NODE:
  case object_t::NODE_ID:
    counts.nodes++;
    break;
  case object_t::WAY:
  case object_t::WAY_ID:
    counts.ways++;
    break;
  case object_t::RELATION:
  case object_t::RELATION_ID:
    counts.relations++;
    break;
  default:
    assert_unreachable();
  }
}

} // namespace

relation_t::memberCounts relation_t::members_by_type() const
{
  memberCounts ret;
  std::for_each(members.begin(), members.end(),
                member_counter(ret));
  return ret;
}
