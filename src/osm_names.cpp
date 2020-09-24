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

#define _DEFAULT_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "osm.h"
#include "osm_p.h"

#include "osm_objects.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "osm2go_annotations.h"
#include <osm2go_cpp.h>
#include <osm2go_i18n.h>
#include <osm2go_platform.h>

namespace {

class typed_relation_member_functor {
  const member_t member;
  const char * const type;
public:
  inline typed_relation_member_functor(const char *t, const char *r, const object_t &o)
    : member(o, r), type(value_cache.insert(t)) {}
  bool operator()(const std::pair<item_id_t, relation_t *> &it) const
  { return it.second->tags.get_value("type") == type &&
           std::find(it.second->members.begin(), it.second->members.end(), member) != it.second->members.end(); }
};

class pt_relation_member_functor {
  const member_t member;
  const char * const type;
  const char * const stop_area;
public:
  inline pt_relation_member_functor(const char *r, const object_t &o)
    : member(o, r), type(value_cache.insert("public_transport"))
    , stop_area(value_cache.insert("stop_area")) {}
  bool operator()(const std::pair<item_id_t, relation_t *> &it) const
  { return it.second->tags.get_value("type") == type &&
           it.second->tags.get_value("public_transport") == stop_area &&
           std::find(it.second->members.begin(), it.second->members.end(), member) != it.second->members.end(); }
};

/**
 * @brief remove underscores from string and replace them by spaces
 *
 * Tags usually have underscores in them, but to display this to the user a version
 * with spaces looks nicer.
 */
inline void clean_underscores_inplace(std::string &s)
{
  std::replace(s.begin(), s.end(), '_', ' ');
}

inline std::string __attribute__((nonnull(1))) __attribute__ ((__warn_unused_result__)) clean_underscores(const char *s)
{
  std::string ret = s;
  clean_underscores_inplace(ret);
  return ret;
}

/**
 * @brief the parts we already have found to construct the final description from
 */
struct nameParts {
  nameParts() : name(nullptr) {}

  const char *name; ///< the value of a "name" key
  class typeWrapper {
#ifndef TRSTRING_NATIVE_TYPE_IS_TRSTRING
    trstring::native_type nt;
#else
    struct {
      inline bool isEmpty() const { return true; };
      inline operator trstring() const __attribute__((noreturn))
      {
        // should always be optimized away
        assert_unreachable();
      }
    } nt;
#endif

  public:
    trstring tr;      ///< an already translated type description
    const char *key;  ///< the raw value of a specific key used as description

    explicit typeWrapper() : key(nullptr) {}
#if __cplusplus >= 201103L
    typeWrapper &operator=(trstring &&n) { tr = std::move(n); assert(nt.isEmpty()); return *this; }
#else
    typeWrapper &operator=(trstring n) { tr.swap(n); assert(nt.isEmpty()); return *this; }
#endif

#ifndef TRSTRING_NATIVE_TYPE_IS_TRSTRING
    typeWrapper &operator=(trstring::native_type_arg n) { assert(tr.isEmpty()); nt = n; return *this; }

    O2G_OPERATOR_EXPLICIT inline operator trstring::native_type() const
    {
      assert(key == nullptr);
      assert(tr.isEmpty());
      assert(isNative());
      return nt;
    }

    inline bool isNative() const
    {
      return !nt.isEmpty();
    }
#endif

    trstring::any_type toTrstring() const
    {
      assert(key == nullptr);
      if (!nt.isEmpty())
        return trstring::any_type(nt);
      assert(!tr.isEmpty());
      return tr;
    }

    inline bool isTranslated() const
    {
      return !tr.isEmpty() || !nt.isEmpty();
    }

    inline bool isEmpty() const
    {
      return key == nullptr && tr.isEmpty() && nt.isEmpty();
    }
  } type;
};

nameParts nameElements(const osm_t &osm, const object_t &obj)
{
  nameParts ret;

  /* try to figure out _what_ this is */
  ret.name = obj.obj->tags.get_value("name");

  /* search for some kind of "type" */
  const std::array<const char *, 9> type_tags =
                          { { "amenity", "place", "historic",
                              "tourism", "landuse", "waterway", "railway",
                              "natural", "man_made" } };

  for(unsigned int i = 0; i < type_tags.size(); i++) {
    ret.type.key = obj.obj->tags.get_value(type_tags[i]);
    if (ret.type.key)
      return ret;
  }

  // ### LEISURE
  const char *rawValue = obj.obj->tags.get_value("leisure");
  if (rawValue != nullptr) {
    // these leisure values will get an extra description from sport=*
    const std::array<const char *, 4> sport_leisure = { {
      "pitch", "sports_centre", "stadium", "track"
    } };
    for (unsigned int i = 0; i < sport_leisure.size(); i++) {
      if (strcmp(rawValue, sport_leisure[i]) == 0) {
        const char *sp = obj.obj->tags.get_value("sport");
        if (sp != nullptr) {
          ret.type = trstring("%1 %2").arg(clean_underscores(sp)).arg(clean_underscores(rawValue));
          return ret;
        }
        break;
      }
    }

    ret.type.key = rawValue;
    return ret;
  }

  // ### BUILDINGS
  rawValue = obj.obj->tags.get_value("building");
  if (rawValue != nullptr && strcmp(rawValue, "no") != 0) {
    const char *street = obj.obj->tags.get_value("addr:street");
    const char *hn = obj.obj->tags.get_value("addr:housenumber");

    // simplify further checks
    if (strcmp(rawValue, "yes") == 0)
      rawValue = nullptr;

    if(street == nullptr) {
      // check if there is an "associatedStreet" relation where this is a "house" member
      const relation_t *astreet = osm.find_relation(typed_relation_member_functor("associatedStreet", "house", obj));
      if(astreet != nullptr)
        street = astreet->tags.get_value("name");
    }

    if(hn != nullptr) {
      trstring dsc = street != nullptr ?
                        rawValue != nullptr ?
                            trstring("%1 building %2 %3").arg(clean_underscores(rawValue)).arg(street) :
                            trstring("building %1 %2").arg(street) :
                        rawValue != nullptr ?
                            trstring("%1 building housenumber %2").arg(clean_underscores(rawValue)) :
                            trstring("building housenumber %1");
      ret.type = dsc.arg(hn);
    } else if (street != nullptr) {
      ret.type = rawValue != nullptr ?
                          trstring("%1 building in %2").arg(clean_underscores(rawValue)).arg(street) :
                          trstring("building in %1").arg(street);
    } else {
      if (rawValue == nullptr)
        ret.type = _("building");
      else
        ret.type = trstring("%1 building").arg(clean_underscores(rawValue));
      if(ret.name == nullptr)
        ret.name = obj.obj->tags.get_value("addr:housename");
    }

    return ret;
  }

  // ### HIGHWAYS
  rawValue = obj.obj->tags.get_value("highway");
  if(rawValue != nullptr) {
    /* highways are a little bit difficult */
    if(!strcmp(rawValue, "primary")     || !strcmp(rawValue, "secondary") ||
        !strcmp(rawValue, "tertiary")    || !strcmp(rawValue, "unclassified") ||
        !strcmp(rawValue, "residential") || !strcmp(rawValue, "service")) {
      // no underscores replacement here because the whitelisted flags above don't have them
      assert(strchr(rawValue, '_') == nullptr);
      ret.type = trstring("%1 road").arg(rawValue);
    }

    else if(obj.type == object_t::WAY && strcmp(rawValue, "pedestrian") == 0) {
      if(obj.way->is_area())
        ret.type = _("pedestrian area");
      else
        ret.type = _("pedestrian way");
    }

    else if(!strcmp(rawValue, "construction")) {
      const char *cstr = obj.obj->tags.get_value("construction:highway");
      if(cstr == nullptr)
        cstr = obj.obj->tags.get_value("construction");
      if(cstr == nullptr) {
        ret.type = _("road/street under construction");
      } else {
        ret.type = trstring("%1 road under construction").arg(cstr);
      }
    }

    else
      ret.type.key = rawValue;

    return ret;
  }

  // ### EMERGENCY
  rawValue = obj.obj->tags.get_value("emergency");
  if (rawValue != nullptr) {
    ret.type.key = rawValue;
    return ret;
  }

  // ### PUBLIC TRANSORT
  rawValue = obj.obj->tags.get_value("public_transport");
  if (rawValue != nullptr) {
    ret.type.key = rawValue;

    // for PT objects without name that are part of another PT relation use the name of that one
    if(ret.name == nullptr) {
      const char *ptkey = strcmp(rawValue, "stop_position") == 0 ? "stop" :
                          strcmp(rawValue, "platform") == 0 ? rawValue :
                          nullptr;
      if(ptkey != nullptr) {
        const relation_t *stoparea = osm.find_relation(pt_relation_member_functor(ptkey, obj));
        if(stoparea != nullptr)
          ret.name = stoparea->tags.get_value("name");
      }
    }

    return ret;
  }

  // ### BARRIER
  rawValue = obj.obj->tags.get_value("barrier");
  if(rawValue != nullptr) {
    if(strcmp("yes", rawValue) == 0)
      ret.type = _("barrier");
    else
      ret.type.key = rawValue;
    return ret;
  }

  // look if this has only one real tag and use that one
  const tag_t *stag = obj.obj->tags.singleTag();
  if (stag != nullptr && strcmp(stag->value, "no") != 0) {
    // rule out a single name tag first
    if (ret.name == nullptr)
      ret.type.key = stag->key;
    return ret;
  }

  // ### last chance
  rawValue = obj.obj->tags.get_value("building:part");
  trstring tret;
  if(rawValue != nullptr && strcmp(rawValue, "yes") == 0)
    ret.type = trstring("building part");
  else
    ret.type = osm.unspecified_name(obj);

  return ret;
}

} // namespace

trstring osm_t::unspecified_name(const object_t &obj) const
{
  const std::map<item_id_t, relation_t *>::const_iterator itEnd = relations.end();
  const char *bmrole = nullptr; // the role "obj" has in the "best" relation
  int rtype = -1; // type of the relation: 3 mp with name, 2 mp, 1 name, 0 anything else
  std::map<item_id_t, relation_t *>::const_iterator best = itEnd;
  std::string bname;

  for (std::map<item_id_t, relation_t *>::const_iterator it = relations.begin(); it != itEnd && rtype < 3; it++) {
    // ignore all relations where obj is no member
    const std::vector<member_t>::const_iterator mit = it->second->find_member_object(obj);
    if (mit == it->second->members.end())
      continue;

    int nrtype = 0;
    if(it->second->is_multipolygon())
      nrtype += 2;
    std::string nname = it->second->descriptive_name();
    assert(!nname.empty());
    if(nname[0] != '<')
      nrtype += 1;

    if(nrtype > rtype) {
      rtype = nrtype;
      best = it;
      bname.swap(nname);
      clean_underscores_inplace(bname);
      bmrole = mit->role;
    }
  }

  if(best == itEnd)
    return trstring("unspecified %1").arg(obj.type_string());

  std::string brole;
  if (bmrole != nullptr)
    brole = clean_underscores(bmrole);

  if(best->second->is_multipolygon() && !brole.empty())
    return trstring("%1: '%2' of multipolygon '%3'").arg(obj.type_string()).arg(brole).arg(bname);

  const char *type = best->second->tags.get_value("type");
  std::string reltype;
  if (type != nullptr)
    reltype = clean_underscores(type);
  else
    reltype = trstring("relation").toStdString();
  if(!brole.empty())
    return trstring("%1: '%2' in %3 '%4'").arg(obj.type_string()).arg(brole).arg(reltype).arg(bname);
  else
    return trstring("%1: member of %2 '%3'").arg(obj.type_string()).arg(reltype).arg(bname);
}

/* try to get an as "speaking" description of the object as possible */
trstring
object_t::get_name(const osm_t &osm) const
{
  assert(is_real());

  /* worst case: we have no tags at all. return techincal info then */
  if(!obj->tags.hasRealTags())
    return osm.unspecified_name(*this);

  /* try to figure out _what_ this is */
  nameParts np = nameElements(osm, *this);

  // no good name was found so far, just look into some other tags to get a useful description
  const std::array<const char *, 3> name_tags = { { "ref", "note", "fix" "me" } };
  for(unsigned int i = 0; np.name == nullptr && i < name_tags.size(); i++)
    np.name = obj->tags.get_value(name_tags[i]);

  if(np.name != nullptr) {
    if (np.type.isEmpty())
      np.type = type_string();

    trstring r;
    if (np.type.isTranslated())
      r = trstring("%1: \"%2\"").arg(np.type.toTrstring());
    else
      r = trstring("%1: \"%2\"").arg(clean_underscores(np.type.key));
    return r.arg(np.name);
  }

  if (!np.type.tr.isEmpty()) {
    return np.type.tr;
#ifndef TRSTRING_NATIVE_TYPE_IS_TRSTRING
  } else if (np.type.isNative()) {
    return trstring(static_cast<trstring::native_type>(np.type));
#endif
  } else {
    trstring ret;
    assert(!np.type.isTranslated());
    ret.assign(clean_underscores(np.type.key));
    return ret;
  }
}
