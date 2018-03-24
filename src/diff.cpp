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

/**
 * @file diff.cpp generate and restore changes on the current data set
 */

#include "diff.h"

#include "appdata.h"
#include "misc.h"
#include "statusbar.h"

#include <algorithm>
#include <libxml/parser.h>
#include <libxml/tree.h>

#if !defined(LIBXML_TREE_ENABLED) || !defined(LIBXML_OUTPUT_ENABLED)
#error "libxml doesn't support required tree or output"
#endif

static std::string diff_filename(const project_t *project) {
  std::string ret = project->path;
  ret += project->name;
  ret += ".diff";
  return ret;
}

struct diff_save_tags_functor {
  xmlNodePtr const node;
  diff_save_tags_functor(xmlNodePtr n) : node(n) {}
  void operator()(const tag_t *tag) {
    xmlNodePtr tag_node = xmlNewChild(node, NULL,
                                      BAD_CAST "tag", NULL);
    xmlNewProp(tag_node, BAD_CAST "k", BAD_CAST tag->key);
    xmlNewProp(tag_node, BAD_CAST "v", BAD_CAST tag->value);
  }
};

static void diff_save_tags(const base_object_t *obj, xmlNodePtr node) {
  diff_save_tags_functor fc(node);
  obj->tags.for_each(fc);
}

/**
 * @brief save the common OSM object information
 * @param obj the object to save
 * @param root_node the XML root node to append to
 * @param tname the name of the XML node
 * @return XML node object
 */
static xmlNodePtr diff_save_state_n_id(const base_object_t *obj,
                                       xmlNodePtr root_node,
                                       const char *tname) {
  xmlNodePtr node = xmlNewChild(root_node, NULL, BAD_CAST tname, NULL);

  if(obj->flags & OSM_FLAG_DELETED)
    xmlNewProp(node, BAD_CAST "state", BAD_CAST "deleted");
  else if(obj->flags & OSM_FLAG_NEW)
    xmlNewProp(node, BAD_CAST "state", BAD_CAST "new");

  /* all items need an id */
  gchar id_str[G_ASCII_DTOSTR_BUF_SIZE];
  g_snprintf(id_str, sizeof(id_str), ITEM_ID_FORMAT, obj->id);
  xmlNewProp(node, BAD_CAST "id", BAD_CAST id_str);

  return node;
}

static bool object_needs_save(const base_object_t *obj)
{
  const int flags = obj->flags;

  /* objects that have been restored (i.e. were deleted upstream) and
   * have been deleted afterwards do not need to be preserved. */
  return (flags && (flags != (OSM_FLAG_DELETED | OSM_FLAG_RESTORED)));
}

struct diff_save_nodes {
  xmlNodePtr const root_node;
  diff_save_nodes(xmlNodePtr r) : root_node(r) {}
  void operator()(std::pair<item_id_t, node_t *> pair);
};

void diff_save_nodes::operator()(std::pair<item_id_t, node_t *> pair)
{
  const node_t * const node = pair.second;
  if(!object_needs_save(node))
    return;

  xmlNodePtr node_node = diff_save_state_n_id(node, root_node, "node");

  if(node->flags & OSM_FLAG_DELETED)
    return;

  char str[32];

  /* additional info is only required if the node hasn't been deleted */
  xml_set_prop_pos(node_node, &node->pos);
  snprintf(str, sizeof(str), "%lu", node->time);
  xmlNewProp(node_node, BAD_CAST "time", BAD_CAST str);

  diff_save_tags(node, node_node);
}

struct diff_save_ways {
  xmlNodePtr const root_node;
  diff_save_ways(xmlNodePtr r) : root_node(r) {}
  void operator()(std::pair<item_id_t, way_t *> pair);
};

void diff_save_ways::operator()(std::pair<item_id_t, way_t *> pair)
{
  const way_t * const way = pair.second;
  if(!object_needs_save(way))
    return;

  xmlNodePtr node_way = diff_save_state_n_id(way, root_node, "way");

  if(way->flags & OSM_FLAG_HIDDEN)
    xmlNewProp(node_way, BAD_CAST "hidden", BAD_CAST "true");

  /* additional info is only required if the way hasn't been deleted */
  /* and of the dirty or new flags are set. (otherwise e.g. only */
  /* the hidden flag may be set) */
  if((!(way->flags & OSM_FLAG_DELETED)) &&
     (way->flags & (OSM_FLAG_DIRTY | OSM_FLAG_NEW))) {
    way->write_node_chain(node_way);
    diff_save_tags(way, node_way);
  }
}

struct diff_save_rel {
  xmlNodePtr const node_rel;
  diff_save_rel(xmlNodePtr n) : node_rel(n) {}
  void operator()(const member_t &member);
};

void diff_save_rel::operator()(const member_t &member)
{
  xmlNodePtr node_member = xmlNewChild(node_rel, NULL,
                                       BAD_CAST "member", NULL);

  gchar ref[G_ASCII_DTOSTR_BUF_SIZE];
  switch(member.object.type) {
  case NODE:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "node");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.obj->id);
    break;
  case WAY:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "way");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.obj->id);
    break;
  case RELATION:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "relation");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.obj->id);
    break;

    /* XXX_ID's are used if this is a reference to an item not */
    /* stored in this xml data set */
  case NODE_ID:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "node");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.id);
    break;
  case WAY_ID:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "way");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.id);
    break;
  case RELATION_ID:
    xmlNewProp(node_member, BAD_CAST "type", BAD_CAST "relation");
    g_snprintf(ref, sizeof(ref), ITEM_ID_FORMAT, member.object.id);
    break;

  default:
    printf("unexpected member type %d\n", member.object.type);
    g_assert_not_reached();
    break;
  }

  xmlNewProp(node_member, BAD_CAST "ref", BAD_CAST ref);

  if(member.role)
    xmlNewProp(node_member, BAD_CAST "role", BAD_CAST member.role);
}

struct diff_save_relations {
  xmlNodePtr const root_node;
  diff_save_relations(xmlNodePtr r) : root_node(r) {}
  void operator()(std::pair<item_id_t, relation_t *> pair);
};

/* store all modfied relations */
void diff_save_relations::operator()(std::pair<item_id_t, relation_t *> pair)
{
  const relation_t * const relation = pair.second;
  if(!object_needs_save(relation))
    return;

  xmlNodePtr node_rel = diff_save_state_n_id(relation, root_node, "relation");

  if(relation->flags & OSM_FLAG_DELETED)
    return;

  /* additional info is only required if the relation */
  /* hasn't been deleted */
  std::for_each(relation->members.begin(), relation->members.end(),
                diff_save_rel(node_rel));

  diff_save_tags(relation, node_rel);
}

struct find_object_by_flags {
  int flagmask;
  find_object_by_flags(int f = ~0) : flagmask(f) {}
  bool operator()(std::pair<item_id_t, base_object_t *> pair) {
    return pair.second->flags & flagmask;
  }
};

/* return true if no diff needs to be saved */
gboolean diff_is_clean(const osm_t *osm, gboolean honor_hidden_flags) {
  /* check if a diff is necessary */
  std::map<item_id_t, node_t *>::const_iterator nit =
    std::find_if(osm->nodes.begin(), osm->nodes.end(), find_object_by_flags());
  if(nit != osm->nodes.end())
    return false;

  int flagmask = honor_hidden_flags ? ~0 : ~OSM_FLAG_HIDDEN;
  std::map<item_id_t, way_t *>::const_iterator wit =
    std::find_if(osm->ways.begin(), osm->ways.end(), find_object_by_flags(flagmask));
  if(wit != osm->ways.end())
    return FALSE;

  std::map<item_id_t, relation_t *>::const_iterator it =
    std::find_if(osm->relations.begin(), osm->relations.end(), find_object_by_flags());
  return (it == osm->relations.end()) ? TRUE : FALSE;
}

void diff_save(const project_t *project, const osm_t *osm) {
  if(!project || !osm) return;

  const std::string &diff_name = diff_filename(project);

  if(diff_is_clean(osm, TRUE)) {
    printf("data set is clean, removing diff if present\n");
    g_remove(diff_name.c_str());
    return;
  }

  printf("data set is dirty, generating diff\n");

  /* write the diff to a new file so the original one needs intact until
   * saving is completed */
  char *ndiff = g_strconcat(project->path, "save.diff", NULL);

  xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
  xmlNodePtr root_node = xmlNewNode(NULL, BAD_CAST "diff");
  xmlNewProp(root_node, BAD_CAST "name", BAD_CAST project->name);
  xmlDocSetRootElement(doc, root_node);

  std::for_each(osm->nodes.begin(), osm->nodes.end(), diff_save_nodes(root_node));
  std::for_each(osm->ways.begin(), osm->ways.end(), diff_save_ways(root_node));
  std::for_each(osm->relations.begin(), osm->relations.end(), diff_save_relations(root_node));

  xmlSaveFormatFileEnc(ndiff, doc, "UTF-8", 1);
  xmlFreeDoc(doc);

  /* if we reach this point writing the new file worked and we */
  /* can delete the backup */
  g_rename(ndiff, diff_name.c_str());

  g_free(ndiff);
}

static item_id_t xml_get_prop_int(xmlNode *node, const char *prop, item_id_t def) {
  xmlChar *str = xmlGetProp(node, BAD_CAST prop);
  item_id_t value = def;

  if(str) {
    value = strtoll((char*)str, NULL, 10);
    xmlFree(str);
  }

  return value;
}

static int xml_get_prop_state(xmlNode *node) {
  xmlChar *str = xmlGetProp(node, BAD_CAST "state");

  if(str) {
    if(strcmp((char*)str, "new") == 0) {
      xmlFree(str);
      return OSM_FLAG_NEW;
    } else if(G_LIKELY(strcmp((char*)str, "deleted") == 0)) {
      xmlFree(str);
      return OSM_FLAG_DELETED;
    } else {
      xmlFree(str);
    }

    g_assert_not_reached();
  }

  return OSM_FLAG_DIRTY;
}

static std::vector<tag_t *> xml_scan_tags(xmlNodePtr node) {
  /* scan for tags */
  std::vector<tag_t *> ret;

  while(node) {
    if(node->type == XML_ELEMENT_NODE) {
      if(G_LIKELY(strcmp((char*)node->name, "tag") == 0)) {
        tag_t *tag = osm_parse_osm_tag(node);
        if(tag)
          ret.push_back(tag);
      }
    }
    node = node->next;
  }
  return ret;
}

/*
 * @brief check if all local modifications of a node are already in the upstream node
 * @param node upstream node
 * @param pos new position
 * @param ntags new tags
 * @return if changes are redundant
 * @retval TRUE the changes are the same as the upstream node
 * @retval FALSE local changes are real
 */
static gboolean
node_compare_changes(const node_t *node, const pos_t *pos, const std::vector<tag_t *> &ntags)
{
  if (node->pos.lat != pos->lat || node->pos.lon != pos->lon)
    return FALSE;

  return !node->tag_lists_diff(ntags);
}

static void diff_restore_node(xmlNodePtr node_node, osm_t *osm) {
  printf("Restoring node");

  /* read properties */
  item_id_t id = xml_get_prop_int(node_node, "id", ID_ILLEGAL);
  if(G_UNLIKELY(id == ID_ILLEGAL)) {
    printf("\n  Node entry missing id\n");
    return;
  }

  printf(" " ITEM_ID_FORMAT "\n", id);

  int state = xml_get_prop_state(node_node);
  pos_t pos;
  gboolean pos_diff = xml_get_prop_pos(node_node, &pos);

  if(G_UNLIKELY(!(state & OSM_FLAG_DELETED) && !pos_diff)) {
    printf("  Node not deleted, but no valid position\n");
    return;
  }

  /* evaluate properties */
  node_t *node = NULL;

  switch(state) {
  case OSM_FLAG_NEW: {
    printf("  Restoring NEW node\n");

    node = new node_t();
    node->id = id;
    node->visible = TRUE;
    if (id > 0)
      node->flags = OSM_FLAG_RESTORED;
    else
      node->flags = OSM_FLAG_NEW;
    node->time = xml_get_prop_int(node_node, "time", 0);
    if(!node->time) node->time = time(NULL);

    /* attach to end of node list */
    osm->nodes[id] = node;
    break;
  }

  case OSM_FLAG_DELETED:
    printf("  Restoring DELETE flag\n");

    if((node = osm->node_by_id(id)) != 0)
      node->flags |= OSM_FLAG_DELETED;
    else
      printf("  WARNING: no node with that id found\n");
    break;

  case OSM_FLAG_DIRTY:
    printf("  Valid id/position (DIRTY)\n");

    if((node = osm->node_by_id(id)) != 0)
      node->flags |= OSM_FLAG_DIRTY;
    else
      printf("  WARNING: no node with that id found\n");
    break;

  default:
    printf("  Illegal node entry\n");
    return;
    break;
  }

  if(G_UNLIKELY(!node)) {
    printf("  no valid node\n");
    return;
  }

  std::vector<tag_t *> ntags = xml_scan_tags(node_node->children);
  /* check if the same changes have been done upstream */
  if(state == OSM_FLAG_DIRTY && node_compare_changes(node, &pos, ntags)) {
    printf("node " ITEM_ID_FORMAT " has the same values and position as upstream, discarding diff\n", id);
    std::for_each(ntags.begin(), ntags.end(), osm_tag_free);
    node->flags &= ~OSM_FLAG_DIRTY;
    return;
  }

  node->replace_tags(ntags);

  /* update position from diff */
  if(pos_diff) {
    node->pos = pos;

    pos2lpos(osm->bounds, &node->pos, &node->lpos);
  }
}

static void diff_restore_way(xmlNodePtr node_node, osm_t *osm) {
  printf("Restoring way");

  item_id_t id = xml_get_prop_int(node_node, "id", ID_ILLEGAL);
  if(G_UNLIKELY(id == ID_ILLEGAL)) {
    printf("\n  entry missing id\n");
    return;
  }

  printf(" " ITEM_ID_FORMAT "\n", id);

  int state = xml_get_prop_state(node_node);

  /* handle hidden flag */
  gboolean hidden = xml_get_prop_is(node_node, "hidden", "true");

  /* evaluate properties */
  way_t *way = NULL;
  switch(state) {
  case OSM_FLAG_NEW: {
    printf("  Restoring NEW way\n");

    way = new way_t();
    way->id = id;
    way->visible = TRUE;
    way->flags = OSM_FLAG_NEW;
    way->time = xml_get_prop_int(node_node, "time", 0);
    if(!way->time) way->time = time(NULL);

    /* attach to end of way list */
    osm->ways[id] = way;
    break;
  }

  case OSM_FLAG_DELETED:
    printf("  Restoring DELETE flag\n");

    if((way = osm->way_by_id(id)) != 0)
      way->flags |= OSM_FLAG_DELETED;
    else
      printf("  WARNING: no way with that id found\n");
    break;

  case OSM_FLAG_DIRTY:
    printf("  Valid id (DIRTY)\n");

    if((way = osm->way_by_id(id)) != 0)
      way->flags |= OSM_FLAG_DIRTY;
    else
      printf("  WARNING: no way with that id found\n");
    break;

  default:
    printf("  Illegal way entry\n");
    return;
  }

  if(!way) {
    printf("  no valid way\n");
    return;
  }

  /* update node_chain */
  if(hidden)
    way->flags |= OSM_FLAG_HIDDEN;

  /* scan for nodes */
  node_chain_t new_chain;
  xmlNode *nd_node = NULL;
  for(nd_node = node_node->children; nd_node; nd_node = nd_node->next) {
    if(nd_node->type == XML_ELEMENT_NODE) {
      if(G_LIKELY(strcmp((char*)nd_node->name, "nd") == 0)) {
	/* attach node to node_chain */
	node_t *tmp = osm_parse_osm_way_nd(osm, nd_node);
	if(tmp)
	  new_chain.push_back(tmp);
      }
    }
  }

  /* only replace the original nodes if new nodes have actually been */
  /* found. */
  if(!new_chain.empty()) {
    if(way->node_chain == new_chain) {
      osm_node_chain_free(new_chain);
      new_chain.clear(); /* indicate that waypoints did not change */
    } else {
      /* way may be an existing way, so remove nodes to */
      /* make space for new ones */
      if(!way->node_chain.empty()) {
        printf("  removing existing nodes for diff nodes\n");
        osm_node_chain_free(way->node_chain);
      }
      way->node_chain.swap(new_chain);
    }

    /* only replace tags if nodes have been found before. if no nodes */
    /* were found this wasn't a dirty entry but e.g. only the hidden */
    /* flag had been set */

    std::vector<tag_t *> ntags = xml_scan_tags(node_node->children);
    if (way->tag_lists_diff(ntags)) {
      way->replace_tags(ntags);
    } else {
      std::for_each(ntags.begin(), ntags.end(), osm_tag_free);
      if (new_chain.empty()) {
        printf("way " ITEM_ID_FORMAT " has the same nodes and tags as upstream, discarding diff\n", id);
        way->flags &= ~OSM_FLAG_DIRTY;
      }
    }
  } else {
    printf("  no nodes restored, way isn't dirty!\n");
    way->flags &= ~OSM_FLAG_DIRTY;
  }
}

static void diff_restore_relation(xmlNodePtr node_rel, osm_t *osm) {
  printf("Restoring relation");

  item_id_t id = xml_get_prop_int(node_rel, "id", ID_ILLEGAL);
  if(G_UNLIKELY(id == ID_ILLEGAL)) {
    printf("\n  entry missing id\n");
    return;
  }

  printf(" " ITEM_ID_FORMAT "\n", id);

  int state = xml_get_prop_state(node_rel);

  /* evaluate properties */
  relation_t *relation = NULL;
  switch(state) {
  case OSM_FLAG_NEW:
    printf("  Restoring NEW relation\n");

    relation = new relation_t();
    relation->id = id;
    relation->visible = TRUE;
    relation->flags = OSM_FLAG_NEW;
    relation->time = xml_get_prop_int(node_rel, "time", 0);
    if(!relation->time) relation->time = time(NULL);

    /* attach to end of relation list */
    osm->relations[id] = relation;
    break;

  case OSM_FLAG_DELETED:
    printf("  Restoring DELETE flag\n");

    if((relation = osm->relation_by_id(id)) != 0)
      relation->flags |= OSM_FLAG_DELETED;
    else
      printf("  WARNING: no relation with that id found\n");
    break;

  case OSM_FLAG_DIRTY:
    printf("  Valid id (DIRTY)\n");

    if((relation = osm->relation_by_id(id)) != 0)
      relation->flags |= OSM_FLAG_DIRTY;
    else
      printf("  WARNING: no relation with that id found\n");
    break;

  default:
    printf("  Illegal relation entry\n");
    return;
  }

  if(G_UNLIKELY(!relation)) {
    printf("  no valid relation\n");
    return;
  }

  bool was_changed = false;
  std::vector<tag_t *> ntags = xml_scan_tags(node_rel->children);
  if (relation->tag_lists_diff(ntags)) {
    relation->replace_tags(ntags);
    was_changed = true;
  } else {
    std::for_each(ntags.begin(), ntags.end(), osm_tag_free);
  }

  /* update members */

  /* scan for members */
  std::vector<member_t> members;
  xmlNode *member_node = NULL;
  for(member_node = node_rel->children; member_node;
      member_node = member_node->next) {
    if(member_node->type == XML_ELEMENT_NODE) {
      if(G_LIKELY(strcmp((char*)member_node->name, "member") == 0)) {
	/* attach member to member_chain */
	member_t member = osm_parse_osm_relation_member(osm, member_node);
	if(member.object.type != ILLEGAL)
          members.push_back(member);
      }
    }
  }

  if(relation->members != members) {
    /* this may be an existing relation, so remove members to */
    /* make space for new ones */
    relation->members.swap(members);
    was_changed = true;
  }
  osm_members_free(members);

  if(!was_changed && (relation->flags & OSM_FLAG_DIRTY)) {
    printf("relation " ITEM_ID_FORMAT " has the same members and tags as upstream, discarding diff\n", id);
    relation->flags &= ~OSM_FLAG_DIRTY;
  }
}

void diff_restore(appdata_t *appdata, project_t *project, osm_t *osm) {
  if(!project || !osm) return;

  /* first try to open a backup which is only present if saving the */
  /* actual diff didn't succeed */
  std::string diff_name = project->path;
  diff_name += "backup.diff";
  if(G_UNLIKELY(g_file_test(diff_name.c_str(), G_FILE_TEST_EXISTS))) {
    printf("diff backup present, loading it instead of real diff ...\n");
  } else {
    diff_name = diff_filename(project);

    if(!g_file_test(diff_name.c_str(), G_FILE_TEST_EXISTS)) {
      printf("no diff present!\n");
      return;
    }
    printf("diff found, applying ...\n");
  }

  xmlDoc *doc = NULL;
  xmlNode *root_element = NULL;

  /* parse the file and get the DOM */
  if(G_UNLIKELY((doc = xmlReadFile(diff_name.c_str(), NULL, 0)) == NULL)) {
    errorf(GTK_WIDGET(appdata->window),
	   "Error: could not parse file %s\n", diff_name.c_str());
    return;
  }

  /* Get the root element node */
  root_element = xmlDocGetRootElement(doc);

  xmlNode *cur_node = NULL;
  for (cur_node = root_element; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      if(strcmp((char*)cur_node->name, "diff") == 0) {
	xmlChar *str = xmlGetProp(cur_node, BAD_CAST "name");
	if(str) {
	  const char *cstr = (const char*)str;
	  printf("diff for project %s\n", cstr);
	  if(G_UNLIKELY(strcmp(project->name, cstr) != 0)) {
	    messagef(GTK_WIDGET(appdata->window), _("Warning"),
		     "Diff name (%s) does not match project name (%s)",
		     cstr, project->name);
	  }
	  xmlFree(str);
	}

	xmlNodePtr node_node = cur_node->children;
	while(node_node) {
	  if(node_node->type == XML_ELEMENT_NODE) {

	    if(strcmp((char*)node_node->name, "node") == 0)
	      diff_restore_node(node_node, osm);

	    else if(strcmp((char*)node_node->name, "way") == 0)
	      diff_restore_way(node_node, osm);

	    else if(G_LIKELY(strcmp((char*)node_node->name, "relation") == 0))
	      diff_restore_relation(node_node, osm);

	    else
	      printf("WARNING: item %s not restored\n", node_node->name);
	  }
	  node_node = node_node->next;
	}
      }
    }
  }

  xmlFreeDoc(doc);

  /* check for hidden ways and update menu accordingly */
  const std::map<item_id_t, way_t *>::const_iterator it =
      std::find_if(osm->ways.begin(), osm->ways.end(), find_object_by_flags(OSM_FLAG_HIDDEN));

  if(it != osm->ways.end()) {
    printf("hidden flags have been restored, enable show_add menu\n");

    statusbar_set(appdata, _("Some objects are hidden"), TRUE);
    gtk_widget_set_sensitive(appdata->menu_item_map_show_all, TRUE);
  }
}

gboolean diff_present(const project_t *project) {
  const std::string &diff_name = diff_filename(project);

  return g_file_test(diff_name.c_str(), G_FILE_TEST_EXISTS);
}

void diff_remove(const project_t *project) {
  const std::string &diff_name = diff_filename(project);
  g_remove(diff_name.c_str());
}
