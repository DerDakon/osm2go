#include <appdata.h>
#include <icon.h>
#include <osm.h>
#include <settings.h>

#include <misc.h>
#include <osm2go_cpp.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>

static bool find_aa(const tag_t &t)
{
  return strcmp(t.value, "aa") == 0;
}

static bool find_bb(const tag_t &t)
{
  return strcmp(t.value, "bb") == 0;
}

static std::vector<tag_t> ab_with_creator(void)
{
  std::vector<tag_t> ntags;

  tag_t cr_by(g_strdup("created_by"), g_strdup("test"));
  g_assert_true(cr_by.is_creator_tag());
  ntags.push_back(cr_by);
  ntags.push_back(tag_t(g_strdup("a"), g_strdup("aa")));
  ntags.push_back(tag_t(g_strdup("b"), g_strdup("bb")));

  return ntags;
}

static bool rtrue(const tag_t &) {
  return true;
}

static void nevercalled(const tag_t &) {
  g_assert_not_reached();
}

static void set_bounds(osm_t &o) {
  o.rbounds.ll_min.lat = 52.2692786;
  o.rbounds.ll_min.lon = 9.5750497;
  o.rbounds.ll_max.lat = 52.2695463;
  o.rbounds.ll_max.lon = 9.5755;

  pos_t center((o.rbounds.ll_max.lat + o.rbounds.ll_min.lat) / 2,
               (o.rbounds.ll_max.lon + o.rbounds.ll_min.lon) / 2);

  o.rbounds.center = center.toLpos();
  o.rbounds.scale = std::cos(DEG2RAD(center.lat));

  o.bounds = &o.rbounds;
}

/**
 * @brief collection of trivial tests to get some coverage
 */
static void test_trivial() {
  object_t obj;

  g_assert(obj == obj);

  tag_list_t tags;
  g_assert_false(tags.hasTagCollisions());
  tag_t cr_by(g_strdup("created_by"), g_strdup("test"));
  g_assert_true(cr_by.is_creator_tag());
  std::vector<tag_t> ntags(1, cr_by);
  tags.replace(ntags);
  g_assert_false(tags.hasRealTags());
  g_assert_false(tags.hasTagCollisions());

  icon_t icons;
  osm_t osm(icons);
  memset(&osm.rbounds, 0, sizeof(osm.rbounds));
  g_assert_cmpint(strcmp(osm.sanity_check(), _("Invalid data in OSM file:\nBoundary box missing!")), ==, 0);
  set_bounds(osm);
  g_assert_cmpint(strcmp(osm.sanity_check(), _("Invalid data in OSM file:\nNo drawable content found!")), ==, 0);

  g_assert_true(osm.position_within_bounds(0, 0));
  g_assert_false(osm.position_within_bounds(-1, 0));
  g_assert_false(osm.position_within_bounds(0, -1));

  way_t w(0);
  g_assert(w.first_node() == O2G_NULLPTR);
  g_assert(w.last_node() == O2G_NULLPTR);
}

static void test_taglist() {
  tag_list_t tags;
  std::vector<tag_t> ntags;

  // compare empty lists
  g_assert(tags == ntags);
  g_assert(!(tags != ntags));

  // a list with only created_by must still be considered empty
  tag_t cr_by(const_cast<char *>("created_by"), const_cast<char *>("test"));
  g_assert_true(cr_by.is_creator_tag());
  ntags.push_back(cr_by);
  g_assert(tags == ntags);
  g_assert(!(tags != ntags));
  ntags.clear();

  // check replacing the tag list from osm_t::TagMap::value_type
  osm_t::TagMap nstags;
  nstags.insert(osm_t::TagMap::value_type("a", "A"));
  nstags.insert(osm_t::TagMap::value_type("b", "B"));

  // check self intersection
  g_assert_true(osm_t::tagSubset(nstags, nstags));
  // real subsets
  osm_t::TagMap tmpTags;
  tmpTags.insert(osm_t::TagMap::value_type("a", "A"));
  g_assert_true(osm_t::tagSubset(tmpTags, nstags));
  tmpTags.clear();
  tmpTags.insert(osm_t::TagMap::value_type("b", "B"));
  g_assert_true(osm_t::tagSubset(tmpTags, nstags));
  // non-intersecting
  tmpTags.insert(osm_t::TagMap::value_type("c", "C"));
  g_assert_false(osm_t::tagSubset(tmpTags, nstags));
  g_assert_false(osm_t::tagSubset(nstags, tmpTags));

  tags.replace(nstags);

  g_assert_cmpuint(nstags.size(), ==, 2);
  g_assert_nonnull(tags.get_value("a"));
  g_assert_cmpint(strcmp(tags.get_value("a"), "A"), ==, 0);
  g_assert_nonnull(tags.get_value("b"));
  g_assert_cmpint(strcmp(tags.get_value("b"), "B"), ==, 0);
  g_assert_false(tags.hasTagCollisions());

  // check replacing the tag list from tag_t
  ntags.push_back(tag_t(g_strdup("a"), g_strdup("aa")));
  ntags.push_back(tag_t(g_strdup("b"), g_strdup("bb")));

  tags.replace(ntags);

  g_assert_true(ntags.empty());
  g_assert_nonnull(tags.get_value("a"));
  g_assert_cmpint(strcmp(tags.get_value("a"), "aa"), ==, 0);
  g_assert_nonnull(tags.get_value("b"));
  g_assert_cmpint(strcmp(tags.get_value("b"), "bb"), ==, 0);
  g_assert_false(tags.hasTagCollisions());

  osm_t::TagMap lowerTags = tags.asMap();

  // replace again
  tags.replace(nstags);

  g_assert_cmpuint(nstags.size(), ==, 2);
  g_assert_nonnull(tags.get_value("a"));
  g_assert_cmpint(strcmp(tags.get_value("a"), "A"), ==, 0);
  g_assert_nonnull(tags.get_value("b"));
  g_assert_cmpint(strcmp(tags.get_value("b"), "B"), ==, 0);
  g_assert_false(tags.hasTagCollisions());

  tag_list_t tags2;
  tags2.replace(nstags);

  // merging the same things shouldn't change anything
  bool collision = tags.merge(tags2);
  g_assert_false(collision);
  g_assert_false(tags.hasTagCollisions());

  g_assert_nonnull(tags.get_value("a"));
  g_assert_cmpint(strcmp(tags.get_value("a"), "A"), ==, 0);
  g_assert_nonnull(tags.get_value("b"));
  g_assert_cmpint(strcmp(tags.get_value("b"), "B"), ==, 0);

  g_assert_null(tags2.get_value("a"));
  g_assert_null(tags2.get_value("b"));

  tags2.replace(lowerTags);
  g_assert_cmpuint(tags2.asMap().size(), ==, 2);
  g_assert_false(lowerTags.empty());
  g_assert_nonnull(tags2.get_value("a"));
  g_assert_cmpint(strcmp(tags2.get_value("a"), "aa"), ==, 0);
  g_assert_nonnull(tags2.get_value("b"));
  g_assert_cmpint(strcmp(tags2.get_value("b"), "bb"), ==, 0);
  g_assert_false(osm_t::tagSubset(tags2.asMap(), tags.asMap()));
  g_assert_false(osm_t::tagSubset(tags.asMap(), tags2.asMap()));

  collision = tags.merge(tags2);
  g_assert_true(collision);
  // moving something back and forth shouldn't change anything
  collision = tags2.merge(tags);
  g_assert_false(collision);
  collision = tags.merge(tags2);
  g_assert_false(collision);
  // tags2 is now empty, merging shouldn't change anything
  g_assert_true(tags2.empty());
  collision = tags.merge(tags2);
  g_assert_false(collision);

  g_assert_true(tags.hasTagCollisions());
  g_assert_nonnull(tags.get_value("a"));
  g_assert_cmpint(strcmp(tags.get_value("a"), "A"), ==, 0);
  g_assert_nonnull(tags.get_value("b"));
  g_assert_cmpint(strcmp(tags.get_value("b"), "B"), ==, 0);
  g_assert_cmpuint(tags.asMap().size(), ==, 4);
  g_assert_true(tags.contains(find_aa));
  g_assert_true(tags.contains(find_bb));

  // check identity with permutations
  ntags = ab_with_creator();
  tags.replace(ntags);
  ntags = ab_with_creator();
  g_assert(tags == ntags);
  std::rotate(ntags.begin(), ntags.begin() + 1, ntags.end());
  g_assert(tags == ntags);
  std::rotate(ntags.begin(), ntags.begin() + 1, ntags.end());
  g_assert(tags == ntags);

  std::for_each(ntags.begin(), ntags.end(), tag_t::clear);
  ntags.clear();
  tags.clear();

  // check that all these methods work on empty objects, both newly created and cleared ones
  g_assert_true(tags.empty());
  g_assert_false(tags.hasRealTags());
  g_assert_null(tags.get_value("foo"));
  g_assert_false(tags.contains(rtrue));
  tags.for_each(nevercalled);
  g_assert_true(tags.asMap().empty());
  g_assert(tags == std::vector<tag_t>());
  g_assert(tags == osm_t::TagMap());
  tags.clear();

  tag_list_t virgin;
  g_assert_true(virgin.empty());
  g_assert_false(virgin.hasRealTags());
  g_assert_null(virgin.get_value("foo"));
  g_assert_false(virgin.contains(rtrue));
  virgin.for_each(nevercalled);
  g_assert_true(virgin.asMap().empty());
  g_assert(virgin == std::vector<tag_t>());
  g_assert(virgin == osm_t::TagMap());
  virgin.clear();

  ntags.push_back(tag_t(g_strdup("one"), g_strdup("1")));
  g_assert(tags != ntags);
  tags.replace(ntags);
  ntags.push_back(tag_t(g_strdup("one"), g_strdup("1")));
  g_assert(tags == ntags);
  g_assert(virgin != tags.asMap());

  std::for_each(ntags.begin(), ntags.end(), tag_t::clear);
}

static void test_replace() {
  node_t node;
  node.flags = 0;

  g_assert_true(node.tags.empty());

  osm_t::TagMap nstags;
  node.updateTags(nstags);
  g_assert_cmpuint(node.flags, ==, 0);
  g_assert_true(node.tags.empty());

  osm_t::TagMap::value_type cr_by("created_by", "test");
  g_assert_true(tag_t::is_creator_tag(cr_by.first.c_str()));
  nstags.insert(cr_by);
  node.updateTags(nstags);
  g_assert(node.flags == 0);
  g_assert_true(node.tags.empty());

  node.tags.replace(nstags);
  g_assert_cmpuint(node.flags, ==, 0);
  g_assert_true(node.tags.empty());

  osm_t::TagMap::value_type aA("a", "A");
  nstags.insert(aA);

  node.updateTags(nstags);
  g_assert_cmpuint(node.flags, ==, OSM_FLAG_DIRTY);
  g_assert_false(node.tags.empty());
  g_assert(node.tags == nstags);

  node.flags = 0;

  node.updateTags(nstags);
  g_assert_cmpuint(node.flags, ==, 0);
  g_assert_false(node.tags.empty());
  g_assert(node.tags == nstags);

  node.tags.clear();
  g_assert_true(node.tags.empty());

  // use the other replace() variant that is also used by diff_restore(),
  // which can also insert created_by tags
  std::vector<tag_t> ntags;
  ntags.push_back(tag_t(g_strdup("created_by"), g_strdup("foo")));
  ntags.push_back(tag_t(g_strdup("a"), g_strdup("A")));
  node.tags.replace(ntags);

  g_assert_cmpuint(node.flags, ==, 0);
  g_assert_false(node.tags.empty());
  g_assert(node.tags == nstags);

  // updating with the same "real" tag shouldn't change anything
  node.updateTags(nstags);
  g_assert_cmpuint(node.flags, ==, 0);
  g_assert_false(node.tags.empty());
  g_assert(node.tags == nstags);
}

static void test_split()
{
  icon_t icons;
  osm_t o(icons);
  way_t * const v = new way_t();
  way_t * const w = new way_t();
  relation_t * const r1 = new relation_t();
  relation_t * const r2 = new relation_t();
  relation_t * const r3 = new relation_t();

  std::vector<tag_t> otags;
  otags.push_back(tag_t(g_strdup("a"), g_strdup("b")));
  otags.push_back(tag_t(g_strdup("b"), g_strdup("c")));
  otags.push_back(tag_t(g_strdup("created_by"), g_strdup("test")));
  otags.push_back(tag_t(g_strdup("d"), g_strdup("e")));
  otags.push_back(tag_t(g_strdup("f"), g_strdup("g")));
  const size_t ocnt = otags.size();

  w->tags.replace(otags);
  v->tags.replace(w->tags.asMap());

  o.way_attach(v);
  o.way_attach(w);

  r1->members.push_back(member_t(object_t(w), O2G_NULLPTR));
  o.relation_attach(r1);
  r2->members.push_back(member_t(object_t(w), O2G_NULLPTR));
  r2->members.push_back(member_t(object_t(v), O2G_NULLPTR));
  o.relation_attach(r2);
  r3->members.push_back(member_t(object_t(v), O2G_NULLPTR));
  o.relation_attach(r3);

  std::vector<node_t *> nodes;
  for(int i = 0; i < 6; i++) {
    node_t *n = new node_t(3, lpos_t(), pos_t(52.25 + i / 0.001, 9.58 + i / 0.001), 1234500 + i);
    o.node_attach(n);
    v->node_chain.push_back(n);
    w->node_chain.push_back(n);
    n->ways += 2;
    nodes.push_back(n);
  }

  g_assert_cmpuint(o.ways.size(), ==, 2);
  way_t *neww = w->split(&o, w->node_chain.begin() + 2, false);
  g_assert_nonnull(neww);
  g_assert_cmpuint(o.ways.size(), ==, 3);
  g_assert(w->flags & OSM_FLAG_DIRTY);
  for(unsigned int i = 0; i < nodes.size(); i++)
    g_assert_cmpuint(nodes[i]->ways, ==, 2);

  g_assert_cmpuint(w->node_chain.size(), ==, 4);
  g_assert_cmpuint(neww->node_chain.size(), ==, 2);
  g_assert(neww->tags == w->tags.asMap());
  g_assert(neww->tags == v->tags.asMap());
  g_assert_cmpuint(neww->tags.asMap().size(), ==, ocnt - 1);
  g_assert_cmpuint(r1->members.size(), ==, 2);
  g_assert_cmpuint(r2->members.size(), ==, 3);
  g_assert_cmpuint(r3->members.size(), ==, 1);

  // now split the remaining way at a node
  way_t *neww2 = w->split(&o, w->node_chain.begin() + 2, true);
  g_assert_nonnull(neww2);
  g_assert_cmpuint(o.ways.size(), ==, 4);
  g_assert(w->flags & OSM_FLAG_DIRTY);
  for(unsigned int i = 0; i < nodes.size(); i++)
    if(i == 4)
      g_assert_cmpuint(nodes[4]->ways, ==, 3);
    else
      g_assert_cmpuint(nodes[i]->ways, ==, 2);

  g_assert_true(w->contains_node(nodes[4]));
  g_assert_true(w->ends_with_node(nodes[4]));
  g_assert_cmpuint(w->node_chain.size(), ==, 3);
  g_assert_cmpuint(neww->node_chain.size(), ==, 2);
  g_assert_cmpuint(neww2->node_chain.size(), ==, 2);
  g_assert(neww2->tags == w->tags.asMap());
  g_assert(neww2->tags == v->tags.asMap());
  g_assert_cmpuint(neww2->tags.asMap().size(), ==, ocnt - 1);
  g_assert_cmpuint(r1->members.size(), ==, 3);
  g_assert_cmpuint(r2->members.size(), ==, 4);
  g_assert_cmpuint(r3->members.size(), ==, 1);

  // just split the last node out of the way
  w->flags = 0;
  g_assert_null(w->split(&o, w->node_chain.begin() + 2, false));
  g_assert_cmpuint(o.ways.size(), ==, 4);
  g_assert(w->flags & OSM_FLAG_DIRTY);
  for(unsigned int i = 0; i < nodes.size(); i++)
    g_assert_cmpuint(nodes[i]->ways, ==, 2);

  g_assert_false(w->contains_node(nodes[4]));
  g_assert_false(w->ends_with_node(nodes[4]));
  g_assert_cmpuint(w->node_chain.size(), ==, 2);
  g_assert_cmpuint(neww->node_chain.size(), ==, 2);
  g_assert_cmpuint(neww2->node_chain.size(), ==, 2);
  g_assert_cmpuint(r1->members.size(), ==, 3);
  g_assert_cmpuint(r2->members.size(), ==, 4);
  g_assert_cmpuint(r3->members.size(), ==, 1);

  // now test a closed way
  way_t * const area = new way_t(0);
  for(unsigned int i = 0; i < nodes.size(); i++)
    area->append_node(nodes[i]);
  area->append_node(nodes[0]);
  g_assert_true(area->is_closed());
  o.way_attach(area);

  // drop the other ways to make reference counting easier
  o.way_delete(v);
  o.way_delete(w);
  o.way_delete(neww);
  o.way_delete(neww2);
  g_assert_cmpuint(o.ways.size(), ==, 1);
  for(unsigned int i = 1; i < nodes.size(); i++)
    g_assert_cmpuint(nodes[i]->ways, ==, 1);
  g_assert_cmpuint(nodes.front()->ways, ==, 2);

  g_assert_null(area->split(&o, area->node_chain.begin(), true));
  g_assert_cmpuint(area->node_chain.size(), ==, nodes.size());
  for(unsigned int i = 0; i < nodes.size(); i++) {
    g_assert(area->node_chain[i] == nodes[i]);
    g_assert_cmpuint(nodes[i]->ways, ==, 1);
  }

  // close the way again
  area->append_node(const_cast<node_t *>(area->first_node()));
  g_assert_null(area->split(&o, area->node_chain.begin() + 1, false));
  g_assert_cmpuint(area->node_chain.size(), ==, nodes.size());
  for(unsigned int i = 0; i < nodes.size(); i++) {
    g_assert(area->node_chain[i] == nodes[(i + 1) % nodes.size()]);
    g_assert_cmpuint(nodes[i]->ways, ==, 1);
  }

  // recreate old layout
  area->append_node(const_cast<node_t *>(area->first_node()));
  g_assert_null(area->split(&o, --area->node_chain.end(), true));
  g_assert_cmpuint(area->node_chain.size(), ==, nodes.size());
  for(unsigned int i = 0; i < nodes.size(); i++) {
    g_assert(area->node_chain[i] == nodes[(i + 1) % nodes.size()]);
    g_assert_cmpuint(nodes[i]->ways, ==, 1);
  }
}

static void test_changeset()
{
  const char message[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<osm>\n"
                         "  <changeset>\n"
                         "    <tag k=\"created_by\" v=\"osm2go v" VERSION "\"/>\n"
                         "    <tag k=\"comment\" v=\"&lt;&amp;&gt;\"/>\n"
                         "  </changeset>\n"
                         "</osm>\n";
  const char message_src[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                             "<osm>\n"
                             "  <changeset>\n"
                             "    <tag k=\"created_by\" v=\"osm2go v" VERSION "\"/>\n"
                             "    <tag k=\"comment\" v=\"testcase comment\"/>\n"
                             "    <tag k=\"source\" v=\"survey\"/>\n"
                             "  </changeset>\n"
                             "</osm>\n";
  xmlChar *cs = osm_generate_xml_changeset("<&>", std::string());

  g_assert_cmpint(memcmp(cs, message, strlen(message)), ==, 0);
  xmlFree(cs);

  cs = osm_generate_xml_changeset("testcase comment", "survey");

  g_assert_cmpint(memcmp(cs, message_src, strlen(message_src)), ==, 0);
  xmlFree(cs);
}

static void test_reverse()
{
  icon_t icons;
  osm_t o(icons);
  set_bounds(o);

  lpos_t l(10, 20);
  node_t *n1 = o.node_new(l);
  g_assert_cmpint(n1->version, ==, 0);
  g_assert_cmpint(n1->flags, ==, OSM_FLAG_DIRTY);
  o.node_attach(n1);
  l.y = 40;
  node_t *n2 = o.node_new(l);
  o.node_attach(n2);
  way_t *w = new way_t(0);
  w->append_node(n1);
  w->append_node(n2);
  o.way_attach(w);

  osm_t::TagMap tags;
  tags.insert(osm_t::TagMap::value_type("highway", "residential"));
  tags.insert(osm_t::TagMap::value_type("foo:forward", "yes"));
  tags.insert(osm_t::TagMap::value_type("foo:backward", "2"));
  tags.insert(osm_t::TagMap::value_type("bar:left", "3"));
  tags.insert(osm_t::TagMap::value_type("bar:right", "4"));
  tags.insert(osm_t::TagMap::value_type("oneway", "-1"));
  tags.insert(osm_t::TagMap::value_type("sidewalk", "left"));

  g_assert(w->first_node() == n1);
  g_assert(w->last_node() == n2);
  g_assert_true(w->isNew());

  w->flags = 0;

  // some relations the way is member in to see how the roles change
  std::vector<relation_t *> rels;
  for(unsigned int i = 0; i < 5; i++) {
    relation_t *r = new relation_t();
    rels.push_back(r);
    o.relation_attach(r);
    osm_t::TagMap rtags;
    rtags.insert(osm_t::TagMap::value_type("type", i == 0 ? "multipolygon" : "route"));
    r->tags.replace(rtags);
    if(i < 4) {
      const char *role = O2G_NULLPTR;
      switch(i) {
      case 0:
      case 1:
        role = "forward";
        break;
      case 2:
        role = "backward";
        break;
      }
      r->members.push_back(member_t(object_t(w), g_strdup(role)));
      r->members.push_back(member_t(object_t(n1), g_strdup(role)));
    }
  }

  w->tags.replace(tags);
  w->reverse();
  unsigned int r = w->reverse_direction_sensitive_tags();
  unsigned int rroles = w->reverse_direction_sensitive_roles(&o);

  g_assert_cmpuint(r, ==, 5);
  g_assert_cmpuint(w->flags, ==, OSM_FLAG_DIRTY);
  g_assert(w->node_chain.front() == n2);
  g_assert(w->node_chain.back() == n1);
  g_assert(w->tags != tags);
  osm_t::TagMap rtags;
  rtags.insert(osm_t::TagMap::value_type("highway", "residential"));
  rtags.insert(osm_t::TagMap::value_type("foo:backward", "yes"));
  rtags.insert(osm_t::TagMap::value_type("foo:forward", "2"));
  rtags.insert(osm_t::TagMap::value_type("bar:right", "3"));
  rtags.insert(osm_t::TagMap::value_type("bar:left", "4"));
  rtags.insert(osm_t::TagMap::value_type("oneway", "yes"));
  rtags.insert(osm_t::TagMap::value_type("sidewalk", "right"));

  g_assert(w->tags == rtags);

  // check relations and their roles
  g_assert_cmpuint(rroles, ==, 2);
  // rels[0] has wrong type, roles should not be modified
  g_assert_cmpuint(rels[0]->members.size(), ==, 2);
  g_assert_cmpint(g_strcmp0(rels[0]->members.front().role, "forward"), ==, 0);
  g_assert_cmpint(g_strcmp0(rels[0]->members.back().role, "forward"), ==, 0);
  // rels[1] has matching type, first member role should be changed
  g_assert_cmpuint(rels[1]->members.size(), ==, 2);
  g_assert_cmpint(g_strcmp0(rels[1]->members.front().role, "backward"), ==, 0);
  g_assert(rels[1]->members.front().object == w);
  g_assert_cmpint(g_strcmp0(rels[1]->members.back().role, "forward"), ==, 0);
  // rels[2] has matching type, first member role should be changed (other direction)
  g_assert_cmpuint(rels[1]->members.size(), ==, 2);
  g_assert_cmpint(g_strcmp0(rels[2]->members.front().role, "forward"), ==, 0);
  g_assert(rels[2]->members.front().object == w);
  g_assert_cmpint(g_strcmp0(rels[2]->members.back().role, "backward"), ==, 0);
  // rels[3] has matching type, but roles are empty
  g_assert_cmpuint(rels[1]->members.size(), ==, 2);
  g_assert(rels[3]->members.front().role == O2G_NULLPTR);
  g_assert(rels[3]->members.front().object == w);
  g_assert(rels[3]->members.back().role == O2G_NULLPTR);

  // go back
  w->reverse();
  r = w->reverse_direction_sensitive_tags();
  rroles = w->reverse_direction_sensitive_roles(&o);

  g_assert_cmpuint(r, ==, 5);
  g_assert_cmpuint(rroles, ==, 2);
  g_assert(w->tags == tags);
}

static void test_way_delete()
{
  icon_t icons;
  osm_t o(icons);
  set_bounds(o);

  // delete a simple way
  lpos_t l(10, 20);
  node_t *n1 = o.node_new(l);
  o.node_attach(n1);
  l.y = 40;
  node_t *n2 = o.node_new(l);
  o.node_attach(n2);
  way_t *w = new way_t(0);
  w->append_node(n1);
  w->append_node(n2);
  o.way_attach(w);

  o.way_delete(w);

  g_assert_cmpuint(o.nodes.size(), ==, 0);
  g_assert_cmpuint(o.ways.size(), ==, 0);

  // delete a closed way
  n1 = o.node_new(l);
  o.node_attach(n1);
  l.y = 20;
  n2 = o.node_new(l);
  o.node_attach(n2);
  w = new way_t(0);
  w->append_node(n1);
  w->append_node(n2);
  o.way_attach(w);
  l.x = 20;
  n2 = o.node_new(l);
  o.node_attach(n2);
  w->append_node(n2);
  g_assert_false(w->is_closed());
  w->append_node(n1);
  g_assert_true(w->is_closed());

  o.way_delete(w);

  g_assert_cmpuint(o.nodes.size(), ==, 0);
  g_assert_cmpuint(o.ways.size(), ==, 0);

  // test way deletion with nodes that should be preserved
  l.x = 10;
  l.y = 20;
  n1 = o.node_new(l);
  o.node_attach(n1);

  // this node will be removed when the way is removed
  l.y = 40;
  n2 = o.node_new(l);
  o.node_attach(n2);

  w = new way_t(0);
  w->append_node(n1);
  w->append_node(n2);
  o.way_attach(w);

  // this instance will persist
  l.x = 20;
  n2 = o.node_new(l);
  o.node_attach(n2);
  w->append_node(n2);

  relation_t *r = new relation_t(0);
  o.relation_attach(r);
  r->members.push_back(member_t(object_t(n2), O2G_NULLPTR));

  osm_t::TagMap nstags;
  nstags.insert(osm_t::TagMap::value_type("a", "A"));
  n1->tags.replace(nstags);

  l.x = 5;
  node_t *n3 = o.node_new(l);
  o.node_attach(n3);
  l.y = 25;
  node_t *n4 = o.node_new(l);
  o.node_attach(n4);

  way_t *w2 = new way_t(0);
  o.way_attach(w2);
  w2->append_node(n3);
  w2->append_node(n4);

  w->append_node(n3);

  // now delete the way, which would reduce the use counter of all nodes
  // n1 should be preserved as it has tags on it's own
  // n2 should be preserved as it is still referenced by a relation
  // n3 should be preserved as it is used in another way
  o.way_delete(w);

  g_assert_cmpuint(o.nodes.size(), ==, 4);
  g_assert_cmpuint(o.ways.size(), ==, 1);
  g_assert_cmpuint(o.relations.size(), ==, 1);
  g_assert(o.node_by_id(n1->id) == n1);
  g_assert(o.node_by_id(n2->id) == n2);
  g_assert(o.node_by_id(n3->id) == n3);
  g_assert(o.node_by_id(n4->id) == n4);
  g_assert_cmpuint(r->members.size(), ==, 1);
}

static void test_member_delete()
{
  icon_t icons;
  osm_t o(icons);
  set_bounds(o);

  // a way with 3 points
  lpos_t l(10, 20);
  node_t *n1 = o.node_new(l);
  o.node_attach(n1);
  l.y = 40;
  node_t *n2 = o.node_new(l);
  o.node_attach(n2);
  way_t *w = new way_t(0);
  w->append_node(n1);
  w->append_node(n2);
  o.way_attach(w);

  l.x = 20;
  n2 = o.node_new(l);
  n2->id = 42;
  o.nodes[n2->id] = n2;
  w->append_node(n2);

  // a relation containing both the way as well as the node
  relation_t * const r = new relation_t(0);
  r->members.push_back(member_t(object_t(w), O2G_NULLPTR));
  r->members.push_back(member_t(object_t(n2), O2G_NULLPTR));
  o.relation_attach(r);

  guint nodes = 0, ways = 0, relations = 0;
  r->members_by_type(nodes, ways, relations);
  g_assert_cmpuint(nodes, ==, 1);
  g_assert_cmpuint(ways, ==, 1);
  g_assert_cmpuint(relations, ==, 0);

  // now delete the node that is member of both other objects
  o.node_delete(n2, true);
  fflush(stdout);
  // since the object had a valid id it should still be there, but unreferenced
  g_assert_cmpuint(o.nodes.size(), ==, 3);
  g_assert_cmpuint(o.ways.size(), ==, 1);
  g_assert_cmpuint(o.relations.size(), ==, 1);
  g_assert_true(n2->tags.empty());
  g_assert_cmpuint(n2->flags, ==, OSM_FLAG_DELETED | OSM_FLAG_DIRTY);

  nodes = 0;
  ways = 0;
  relations = 0;
  r->members_by_type(nodes, ways, relations);
  g_assert_cmpuint(nodes, ==, 0);
  g_assert_cmpuint(ways, ==, 1);
  g_assert_cmpuint(relations, ==, 0);
}

static void test_merge_nodes()
{
  icon_t icons;
  osm_t o(icons);
  set_bounds(o);

  // join 2 new nodes
  lpos_t oldpos(10, 10);
  lpos_t newpos(20, 20);
  node_t *n1 = o.node_new(oldpos);
  node_t *n2 = o.node_new(newpos);
  o.node_attach(n1);
  o.node_attach(n2);

  bool conflict = true;

  node_t *n = o.mergeNodes(n1, n2, conflict);
  g_assert(n == n1);
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);

  // join a new and an old node, the old one should be preserved
  n2 = o.node_new(oldpos);
  n2->id = 1234;
  n2->flags = 0;
  o.nodes[n2->id] = n2;

  conflict = true;
  n = o.mergeNodes(n2, n1, conflict);
  g_assert(n == n2);
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);

  // do the same join again, but with swapped arguments
  n2->lpos = newpos;
  n2->flags = 0;
  n1 = o.node_new(oldpos);
  o.node_attach(n1);

  conflict = true;
  n = o.mergeNodes(n1, n2, conflict);
  g_assert(n == n2);
  // order is important for the position, but nothing else
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);

  o.node_free(n);
  g_assert_cmpuint(o.nodes.size(), ==, 0);

  // start new
  n1 = o.node_new(oldpos);
  n2 = o.node_new(newpos);
  o.node_attach(n1);
  o.node_attach(n2);

  conflict = true;

  // attach one node to a way, that one should be preserved
  way_t *w = new way_t(0);
  o.way_attach(w);
  w->append_node(n2);

  n = o.mergeNodes(n1, n2, conflict);
  g_assert(n == n2);
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);
  g_assert_cmpuint(w->node_chain.size(), ==, 1);
  g_assert(w->node_chain.front() == n2);

  o.way_delete(w);
  g_assert_cmpuint(o.nodes.size(), ==, 0);
  g_assert_cmpuint(o.ways.size(), ==, 0);

  // now check with relation membership
  relation_t *r = new relation_t(0);
  o.relation_attach(r);
  n1 = o.node_new(oldpos);
  n2 = o.node_new(newpos);
  o.node_attach(n1);
  o.node_attach(n2);

  conflict = true;
  r->members.push_back(member_t(object_t(n2), O2G_NULLPTR));

  n = o.mergeNodes(n1, n2, conflict);
  g_assert(n == n2);
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);
  g_assert_cmpuint(r->members.size(), ==, 1);
  g_assert(r->members.front().object == n2);

  o.relation_delete(r);
  g_assert_cmpuint(o.nodes.size(), ==, 1);
  g_assert_cmpuint(o.ways.size(), ==, 0);
  g_assert_cmpuint(o.relations.size(), ==, 0);
  o.node_delete(o.nodes.begin()->second);
  g_assert_cmpuint(o.nodes.size(), ==, 0);

  // now put both into a way, the way of the second node should be updated
  for(int i = 0; i < 2; i++) {
    w = new way_t(0);
    o.way_attach(w);
    lpos_t pos(i + 4, i + 4);
    n1 = o.node_new(pos);
    o.node_attach(n1);
    w->append_node(n1);
    r = new relation_t(0);
    o.relation_attach(r);
  }

  n1 = o.node_new(oldpos);
  n2 = o.node_new(newpos);
  o.node_attach(n1);
  o.node_attach(n2);

  o.ways.begin()->second->append_node(n1);
  o.ways.begin()->second->reverse();
  w = (++o.ways.begin())->second;
  w->append_node(n2);
  w->flags = 0;
  o.relations.begin()->second->members.push_back(member_t(object_t(n1), O2G_NULLPTR));
  r = (++o.relations.begin())->second;
  r->members.push_back(member_t(object_t(n2), O2G_NULLPTR));
  r->flags = 0;
  g_assert_cmpuint(o.ways.begin()->second->node_chain.size(), ==, 2);
  g_assert_cmpuint(w->node_chain.size(), ==, 2);
  g_assert_true(o.ways.begin()->second->node_chain.front() == n1);
  g_assert_true(o.ways.begin()->second->ends_with_node(n1));
  g_assert(w->node_chain.back() == n2);
  g_assert_true(w->ends_with_node(n2));
  g_assert_cmpuint(n1->ways, ==, 1);
  g_assert(o.relations.begin()->second->members.front().object == n1);
  g_assert(r->members.front().object == n2);

  conflict = true;
  n = o.mergeNodes(n1, n2, conflict);
  g_assert(n == n1);
  g_assert(n->lpos == newpos);
  g_assert_false(conflict);
  g_assert_cmpuint(o.nodes.size(), ==, 3);
  g_assert_cmpuint(n->flags, ==, OSM_FLAG_DIRTY);
  g_assert_cmpuint(r->members.size(), ==, 1);
  g_assert(o.ways.begin()->second->first_node() == n1);
  g_assert_true(o.ways.begin()->second->ends_with_node(n1));
  g_assert(w->last_node() == n1);
  g_assert_true(w->ends_with_node(n1));
  g_assert_cmpuint(w->flags, ==, OSM_FLAG_DIRTY);
  g_assert_cmpuint(n1->ways, ==, 2);
  g_assert(o.relations.begin()->second->members.front().object == n1);
  g_assert(r->members.front().object == n1);
  g_assert_cmpuint(r->flags, ==, OSM_FLAG_DIRTY);

  // while at it: test backwards mapping to containing objects
  const relation_chain_t &rchain = o.to_relation(object_t(n1));
  g_assert_cmpuint(rchain.size(), ==, 2);
  g_assert(std::find(rchain.begin(), rchain.end(), o.relations.begin()->second) != rchain.end());
  g_assert(std::find(rchain.begin(), rchain.end(), r) != rchain.end());

  const relation_chain_t &rchain2 = o.to_relation(object_t(NODE_ID, n1->id));
  g_assert_cmpuint(rchain2.size(), ==, rchain.size());
  g_assert(std::find(rchain2.begin(), rchain2.end(), o.relations.begin()->second) != rchain2.end());
  g_assert(std::find(rchain2.begin(), rchain2.end(), r) != rchain2.end());
  g_assert(rchain == rchain2);

  const way_chain_t &wchain = o.node_to_way(n1);
  g_assert_cmpuint(wchain.size(), ==, 2);
  g_assert(std::find(wchain.begin(), wchain.end(), o.ways.begin()->second) != wchain.end());
  g_assert(std::find(wchain.begin(), wchain.end(), w) != wchain.end());

  // the relation with the highest id (since all are negative)
  g_assert(r->descriptive_name() == "<ID #-1>");
}

static void test_merge_ways()
{
  icon_t icons;
  osm_t o(icons);
  set_bounds(o);

  node_chain_t nodes;
  for(int i = 0; i < 8; i++) {
    nodes.push_back(o.node_new(lpos_t(i * 3, i * 3)));
    o.node_attach(nodes.back());
  }

  // test all 4 combinations how the ways can be oriented
  for(unsigned int i = 0; i < 4; i++) {
    node_chain_t expect;

    way_t *w0 = new way_t(0);
    if(i < 2) {
      for(unsigned int j = 0; j < nodes.size() / 2; j++)
        w0->append_node(nodes[j]);
    } else {
      for(int j = nodes.size() / 2 - 1; j >= 0; j--)
        w0->append_node(nodes[j]);
    }
    o.way_attach(w0);

    way_t *w1 = new way_t(0);
    if(i % 2) {
      for(unsigned int j = nodes.size() / 2 - 1; j < nodes.size(); j++)
        w1->append_node(nodes[j]);
      expect = nodes;
    } else {
      for(unsigned int j = nodes.size() - 1; j >= nodes.size() / 2 - 1; j--)
        w1->append_node(nodes[j]);
      expect = nodes;
      std::reverse(expect.begin(), expect.end());
    }
    o.way_attach(w1);

    g_assert_false(w1->merge(w0, &o, false));
    g_assert_cmpuint(w1->node_chain.size(), ==, nodes.size());
    g_assert_cmpuint(o.ways.size(), ==, 1);
    g_assert_cmpuint(o.nodes.size(), ==, nodes.size());
    for(unsigned int i = 0; i < nodes.size(); i++) {
      w1->contains_node(nodes[i]);
      g_assert_cmpuint(nodes[i]->ways, ==, 1);
    }
    g_assert(expect == w1->node_chain);

    o.way_free(w1);

    g_assert_cmpuint(o.ways.size(), ==, 0);
    g_assert_cmpuint(o.nodes.size(), ==, nodes.size());
    for(unsigned int i = 0; i < nodes.size(); i++)
      g_assert_cmpuint(nodes[i]->ways, ==, 0);
  }
}

static void test_api_adjust()
{
 const std::string api06https = "https://api.openstreetmap.org/api/0.6";
 const std::string apihttp = "http://api.openstreetmap.org/api/0.";
 const std::string apidev = "http://master.apis.dev.openstreetmap.org/api/0.6";
 std::string server;

 g_assert_false(api_adjust(server));
 g_assert_true(server.empty());

 server = apihttp + '5';
 g_assert_true(api_adjust(server));
 g_assert(server == api06https);

 g_assert_false(api_adjust(server));
 g_assert(server == api06https);

 server = apihttp + '6';
 g_assert_true(api_adjust(server));
 g_assert(server == api06https);

 server = apihttp + '7';
 g_assert_false(api_adjust(server));
 g_assert(server != api06https);

 server = apidev;
 g_assert_false(api_adjust(server));
 g_assert(server == apidev);
}

int main()
{
  xmlInitParser();

  test_trivial();
  test_taglist();
  test_replace();
  test_split();
  test_changeset();
  test_reverse();
  test_way_delete();
  test_member_delete();
  test_merge_nodes();
  test_merge_ways();
  test_api_adjust();

  xmlCleanupParser();

  return 0;
}
