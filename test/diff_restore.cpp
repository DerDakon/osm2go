#include "test_osmdb.h"

#include <appdata.h>
#include <diff.h>
#include "dummy_map.h"
#include <icon.h>
#include <map.h>
#include <misc.h>
#include <osm.h>
#include <project.h>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#include <osm2go_annotations.h>
#include <osm2go_cpp.h>
#include <osm2go_platform.h>

namespace {

void verify_diff(osm_t::ref osm)
{
  assert_cmpnum(12, osm->nodes.size());
  assert_cmpnum(3, osm->ways.size());
  assert_cmpnum(5, osm->relations.size());

  // new tag added in diff
  const node_t * const n72 = osm->object_by_id<node_t>(638499572);
  assert(n72 != nullptr);
  assert_cmpnum(n72->flags, OSM_FLAG_DIRTY);
  assert(n72->tags.get_value("testtag") != nullptr);
  assert_cmpnum(n72->tags.asMap().size(), 5);
  // deleted, but the way is contained is only modified
  const node_t * const n21 = osm->object_by_id<node_t>(3577031221LL);
  assert(n21 != nullptr);
  assert(n21->isDeleted());
  assert_cmpnum(n21->flags, OSM_FLAG_DELETED);
  assert(n21->tags.empty());
  assert_cmpnum(n21->ways, 0);
  assert(osm->originalObject(n21) != nullptr);
  // in diff, but the same as in .osm
  const node_t * const n23 = osm->object_by_id<node_t>(3577031223LL);
  assert(n23 != nullptr);
  assert_cmpnum(n23->flags, 0);
  assert(n23->tags.empty());
  // deleted in diff, the way that contained it is also gone
  const node_t * const n26 = osm->object_by_id<node_t>(3577031226LL);
  assert(n26 != nullptr);
  assert(n26->isDeleted());
  assert_cmpnum(n26->flags, OSM_FLAG_DELETED);
  assert(n26->tags.empty());
  assert_cmpnum(n26->ways, 0);
  assert(osm->originalObject(n26) != nullptr);
  const way_t * const w = osm->object_by_id<way_t>(351899455);
  assert(w != nullptr);
  assert(w->isDeleted());
  assert_cmpnum(w->user, 53064);
  assert(osm->users.find(53064) != osm->users.end());
  assert(osm->users[53064] == "Dakon");
  // added in diff
  const node_t * const nn1 = osm->object_by_id<node_t>(-1);
  assert(nn1 != nullptr);
  assert_cmpnum(nn1->pos.lat, 52.2693518);
  assert_cmpnum(nn1->pos.lon, 9.576014);
  assert(nn1->tags.empty());
  // added in diff, same position as existing node
  const node_t * const nn2 = osm->object_by_id<node_t>(-2);
  assert(nn2 != nullptr);
  assert_cmpnum(nn2->pos.lat, 52.269497);
  assert_cmpnum(nn2->pos.lon, 9.5752223);
  assert(nn2->tags.empty());
  // which is this one
  const node_t * const n27 = osm->object_by_id<node_t>(3577031227LL);
  assert(n27 != nullptr);
  assert_cmpnum(n27->flags, 0);
  assert_cmpnum(nn2->pos.lat, n27->pos.lat);
  assert_cmpnum(nn2->pos.lon, n27->pos.lon);
  // the node was part of the deleted way 351899455 and nothing else, the reference count must now be 0
  assert_cmpnum(n27->ways, 0);
  const node_t * const n29 = osm->object_by_id<node_t>(3577031229LL);
  assert(n29 != nullptr);
  assert_cmpnum(n27->flags, 0);
  // this node is references in the original data by way 351899453
  // it is also referenced by way 351899452 in the diff
  assert_cmpnum(n29->ways, 2);
  // the upstream version has "wheelchair", we have "source"
  // our modification must survive
  const way_t * const w55 = osm->object_by_id<way_t>(351899455);
  assert(w55 != nullptr);
  assert(w55->isDeleted());
  assert(osm->originalObject(w55) != nullptr);
  assert(w55->tags.empty());
  assert(w55->node_chain.empty());
  const way_t * const w452 = osm->object_by_id<way_t>(351899452);
  assert(w452 != nullptr);
  assert(w452->tags.get_value("source") != nullptr);
  assert_null(w452->tags.get_value("wheelchair"));
  assert_cmpnum(w452->tags.asMap().size(), 3);
  const way_t * const w453 = osm->object_by_id<way_t>(351899453);
  assert(w453 != nullptr);
  assert_cmpnum(w453->flags, 0);
  const relation_t * const r66316 = osm->object_by_id<relation_t>(66316);
  assert(r66316 != nullptr);
  assert(r66316->isDeleted());
  assert_cmpnum(r66316->flags, OSM_FLAG_DELETED);
  assert(r66316->tags.empty());
  assert(r66316->members.empty());
  assert(osm->originalObject(r66316) != nullptr);
  const relation_t * const r255 = osm->object_by_id<relation_t>(296255);
  assert(r255 != nullptr);
  assert_cmpnum(r255->flags, OSM_FLAG_DIRTY);
  assert_cmpnum(r255->members.size(), 164);

  // diff is the same as original
  const relation_t * const r716 = osm->object_by_id<relation_t>(1939716);
  assert(r716 != nullptr);
  assert_cmpnum(r716->flags, 0);

  const relation_t * const r091 = osm->object_by_id<relation_t>(1947091);
  assert(r091 != nullptr);
  const relation_t * const or091 = osm->originalObject(r091);
  assert(or091 != nullptr);
  assert_cmpnum(r091->flags, OSM_FLAG_DIRTY);
  assert(r091->members == or091->members);
  assert_cmpstr(r091->tags.get_value("note"), "tags changed");

  const object_t r255m572(const_cast<node_t *>(n72));
  std::vector<member_t>::const_iterator r255it = r255->find_member_object(r255m572);
  r255it = r255->find_member_object(r255m572);
  assert(r255it != r255->members.end());
  assert(r255it->role != nullptr);
  assert_cmpstr(r255it->role, "forward_stop");
  assert_cmpnum(r255->tags.asMap().size(), 8);

  const relation_t * const r853 = osm->object_by_id<relation_t>(5827853);
  assert(r853 != nullptr);
  assert_cmpnum(r853->flags, OSM_FLAG_DIRTY);
  for(std::vector<member_t>::const_iterator it = r853->members.begin(); it != r853->members.end(); it++)
    assert_cmpnum(it->object.type, object_t::RELATION_ID);

  assert(!osm->is_clean(true));
}

void compare_with_file(const void *buf, size_t len, const char *fn)
{
  osm2go_platform::MappedFile fdata(fn);

  assert(fdata);
  assert_cmpnum(fdata.length(), len);

  assert_cmpmem(fdata.data(), fdata.length(), buf, len);
}

void test_osmChange(osm_t::ref osm, const char *fn)
{
   xmlDocGuard doc(osmchange_init());
  const char *changeset = "42";

  osmchange_delete(osm->modified(), xmlDocGetRootElement(doc.get()), changeset);

  xmlChar *result;
  int len;
  xmlDocDumpFormatMemoryEnc(doc.get(), &result, &len, "UTF-8", 1);

  compare_with_file(result, len, fn);
  xmlFree(result);
}

project_t *setup_for_restore(const char *argv2, const std::string &osm_path)
{
  std::unique_ptr<project_t> project(std::make_unique<project_t>(argv2, osm_path));
  project->osmFile = argv2 + std::string(".osm");

  if (!project->parse_osm())
    return nullptr;

  osm_t::ref osm = project->osm;
  assert(osm);

  assert_cmpnum(osm->uploadPolicy, osm_t::Upload_Blocked);
  assert(osm->sanity_check().isEmpty());

  const relation_t * const r255 = osm->object_by_id<relation_t>(296255);
  assert(r255 != nullptr);
  assert_cmpnum(r255->flags, 0);
  assert_cmpnum(r255->members.size(), 165);
  assert_cmpnum(r255->tags.asMap().size(), 8);
  const node_t * const n72 = osm->object_by_id<node_t>(638499572);
  assert_cmpnum(n72->tags.asMap().size(), 4);
  const object_t r255m572(const_cast<node_t *>(n72));
  std::vector<member_t>::const_iterator r255it = r255->find_member_object(r255m572);
  assert(r255it != r255->members.end());
  assert(r255it->role != nullptr);
  assert_cmpstr(r255it->role, "stop");
  const relation_t * const r66316 = osm->object_by_id<relation_t>(66316);
  assert(r66316 != nullptr);
  assert(!r66316->tags.empty());
  object_t rmember(object_t::RELATION_ID, 296255);
  assert(!rmember.is_real());
  const std::vector<member_t>::const_iterator r66316it = r66316->find_member_object(rmember);
  assert(r66316it != r66316->members.end());
  // the child relation exists, so it should be stored as real ref
  assert(r66316it->object.is_real());

  // the node is part of way 351899455 and referenced there twice
  const node_t * const n27 = osm->object_by_id<node_t>(3577031227LL);
  assert(n27 != nullptr);
  assert_cmpnum(n27->ways, 2);

  // the node is part of way 351899453
  const node_t * const n29 = osm->object_by_id<node_t>(3577031229LL);
  assert(n29 != nullptr);
  assert_cmpnum(n29->ways, 1);

  assert_cmpnum(10, osm->nodes.size());
  assert_cmpnum(3, osm->ways.size());
  assert_cmpnum(5, osm->relations.size());

  assert(osm->is_clean(true));
  verify_osm_db::run(osm);
  return project.release();
}

} // namespace

int main(int argc, char **argv)
{
  int result = 0;

  if(argc != 4)
    return EINVAL;

  xmlInitParser();

  const std::string osm_path = argv[1];
  assert(ends_with(osm_path, '/'));

  std::unique_ptr<project_t> project(setup_for_restore(argv[2], osm_path));

  if(!project) {
    std::cerr << "cannot open " << argv[1] << argv[2] << ": " << strerror(errno) << std::endl;
    return 1;
  }

  assert(project->diff_file_present());
  unsigned int flags = project->diff_restore();
  assert_cmpnum(flags, DIFF_RESTORED | DIFF_HAS_HIDDEN | DIFF_ELEMENTS_IGNORED);

  project.reset(setup_for_restore(argv[2], osm_path));

  if(!project) {
    std::cerr << "cannot open " << argv[1] << argv[2] << ": " << strerror(errno) << std::endl;
    return 1;
  }

  {
    MainUiDummy dummy;
    assert(project->diff_file_present());
    dummy.m_statusTexts.push_back(trstring("Some objects are hidden"));
    dummy.m_actions.insert(std::make_pair(MainUi::MENU_ITEM_MAP_SHOW_ALL, true));
    diff_restore(project, &dummy);
  }

  osm_t::ref osm = project->osm;

  verify_diff(osm);
  verify_osm_db::run(osm);

  const relation_t * const r255 = osm->object_by_id<relation_t>(296255);
  xmlString rel_str(r255->generate_xml("42"));
  printf("%s\n", rel_str.get());
  // make sure this test doesn't suddenly fail only because libxml2 decided to use the other type of quotes
  assert((strstr(reinterpret_cast<const char *>(rel_str.get()), "<relation id=\"296255\" version=\"54\" changeset=\"42\">") != nullptr) !=
         (strstr(reinterpret_cast<const char *>(rel_str.get()), "<relation id='296255' version='54' changeset='42'>") != nullptr));

  const way_t * const w55 = osm->object_by_id<way_t>(351899455);
  rel_str.reset(w55->generate_xml("47"));
  printf("%s\n", rel_str.get());
  assert((strstr(reinterpret_cast<const char *>(rel_str.get()), "<way id=\"351899455\" version=\"1\" changeset=\"47\"/>") != nullptr) !=
         (strstr(reinterpret_cast<const char *>(rel_str.get()), "<way id='351899455' version='1' changeset='47'/>") != nullptr));

  const node_t * const n72 = osm->object_by_id<node_t>(638499572);
  rel_str.reset(n72->generate_xml("42"));
  printf("%s\n", rel_str.get());
  assert((strstr(reinterpret_cast<const char *>(rel_str.get()), "<node id=\"638499572\" version=\"12\" changeset=\"42\" lat=\"52.26") != nullptr) !=
         (strstr(reinterpret_cast<const char *>(rel_str.get()), "<node id='638499572' version='12' changeset='42' lat='52.26") != nullptr));

  char tmpdir[] = "/tmp/osm2go-diff_restore-XXXXXX";

  if(mkdtemp(tmpdir) == nullptr) {
    std::cerr << "cannot create temporary directory" << std::endl;
    result = 1;
  } else {
    // create an empty directoy
    std::string bpath = tmpdir + std::string("/") + argv[2];
    const std::string osmpath = bpath + '/' + argv[2] + ".osm";
    mkdir(bpath.c_str(), S_IRWXU);
    bpath.erase(bpath.rfind('/') + 1);
    // and create a new project from that
    std::unique_ptr<project_t> sproject(std::make_unique<project_t>(argv[2], bpath));
    // CAUTION: osm is shared between the projects now
    sproject->osm.reset(osm.get());

    // the directory is empty, there can't be any diff
    flags = sproject->diff_restore();
    assert_cmpnum(flags, DIFF_NONE_PRESENT);
    // should not do anything bad
    diff_restore(sproject, nullptr);

    sproject->diff_save();
    bpath += argv[2];
    std::string bdiff = bpath;
    std::string no_diff = bpath;
    bpath += '/';
    bpath += argv[2];
    bpath += '.';
    bpath += "diff";

    bdiff += "/backup.diff";
    assert(sproject->diff_file_present());
    assert_cmpnum(rename(bpath.c_str(), bdiff.c_str()), 0);
    // having backup.diff should still count as being present
    assert(sproject->diff_file_present());
    no_diff += "/no.diff";
    assert_cmpnum(rename(bdiff.c_str(), no_diff.c_str()), 0);
    assert(!sproject->diff_file_present());

    // saving without OSM data should just do nothing
    sproject->osm.release();
    // CAUTION: end of sharing
    sproject->diff_save();
    assert(!sproject->diff_file_present());

    // put the OSM data into this directory
    const std::string origosmpath = project->path + project->osmFile;
    symlink(origosmpath.c_str(), osmpath.c_str());
    sproject->osmFile = project->osmFile;
    bool pvalid = sproject->parse_osm();
    assert(pvalid);
    assert(sproject->osm);

    // now create a diff file dummy
    fdguard fd(open(bpath.c_str(), O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR));
    assert(fd.valid());
    {
      fdguard none(-1);
      none.swap(fd);
    }
    assert(sproject->diff_file_present());
    sproject->diff_save();
    assert(!sproject->diff_file_present());

    assert_cmpnum(rename(no_diff.c_str(), bdiff.c_str()), 0);
    flags = sproject->diff_restore();
    assert_cmpnum(flags, DIFF_RESTORED | DIFF_HAS_HIDDEN);

    verify_diff(osm);

    unlink(osmpath.c_str());
    unlink(bdiff.c_str());
    bpath.erase(bpath.rfind('/'));
    rmdir(bpath.c_str());
    bpath.erase(bpath.rfind('/'));
    rmdir(bpath.c_str());
  }

  test_osmChange(osm, argv[3]);

  xmlCleanupParser();

  return result;
}

#include "dummy_appdata.h"
