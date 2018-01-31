/*
 * Copyright (C) 2008-2009 Till Harbaum <till@harbaum.org>.
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

#include <appdata.h>
#include <canvas.h>
#include <diff.h>
#include <gps.h>
#include <iconbar.h>
#include <josm_presets.h>
#include <map.h>
#include <misc.h>
#include <osm_api.h>
#include "osm2go_platform.h"
#include "osm2go_platform_gtk.h"
#include <project.h>
#include <relation_edit.h>
#include <settings.h>
#include <statusbar.h>
#include <style.h>
#include <style_widgets.h>
#include <track.h>
#include <wms.h>

#ifdef FREMANTLE
#include <hildon/hildon-button.h>
#include <hildon/hildon-check-button.h>
#include <hildon/hildon-defines.h>
#include <hildon/hildon-file-chooser-dialog.h>
#include <hildon/hildon-gtk.h>
#include <hildon/hildon-program.h>
#include <hildon/hildon-window-stack.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <libosso.h>      /* required for screen saver timeout */
#define GTK_FM_OK  GTK_RESPONSE_OK
#define MENU_CHECK_ITEM HildonCheckButton
#define MENU_CHECK_ITEM_ACTIVE(a) hildon_check_button_get_active(a)
#else
#define GTK_FM_OK  GTK_RESPONSE_ACCEPT
#define MENU_CHECK_ITEM GtkCheckMenuItem
#define MENU_CHECK_ITEM_ACTIVE(a) gtk_check_menu_item_get_active(a)
#endif

// Maemo/Hildon builds

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <gdk/gdkkeysyms.h>

#include <osm2go_annotations.h>
#include "osm2go_i18n.h"

using namespace osm2go_platform;

#define LOCALEDIR PREFIX "/locale"

#ifndef FREMANTLE
/* these size defaults are used in the non-hildonized version only */
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#endif

struct appdata_internal : public appdata_t {
  appdata_internal(map_state_t &mstate);
  ~appdata_internal();

#ifdef FREMANTLE
  HildonProgram *program;
  /* submenues are seperate menues under fremantle */
  g_widget app_menu_view, app_menu_wms, app_menu_track, app_menu_map;
#else
  GtkCheckMenuItem *menu_item_view_fullscreen;
#endif

  GtkWidget *btn_zoom_in, *btn_zoom_out;
};

/* disable/enable main screen control dependant on presence of open project */
void appdata_t::main_ui_enable() {
  bool project_valid = (project != O2G_NULLPTR);
  gboolean osm_valid = (osm != O2G_NULLPTR) ? TRUE : FALSE;

  if(unlikely(window == O2G_NULLPTR)) {
    printf("%s: main window gone\n", __PRETTY_FUNCTION__);
    return;
  }

  /* cancel any action in progress */
  if(iconbar->isCancelEnabled())
    map_action_cancel(map);

  /* ---- set project name as window title ----- */
  g_string str;
  const char *cstr = "OSM2go";

  if(project_valid) {
#ifdef FREMANTLE
    str.reset(g_markup_printf_escaped(_("<b>%s</b> - OSM2Go"),
                                      project->name.c_str()));
    cstr = str.get();
  }

  hildon_window_set_markup(HILDON_WINDOW(window), cstr);
#else
    str.reset(g_strdup_printf(_("%s - OSM2Go"), project->name.c_str()));
    cstr = str.get();
  }

  gtk_window_set_title(GTK_WINDOW(window), cstr);
#endif
  str.reset();

  iconbar->setToolbarEnable(osm_valid == TRUE);
  /* disable all menu entries related to map */
  uicontrol->setActionEnable(MainUi::SUBMENU_MAP, project_valid);

  // those icons that get enabled or disabled depending on OSM data being loaded
#ifndef FREMANTLE
  std::array<MainUi::menu_items, 7> osm_active_items = { {
    MainUi::MENU_ITEM_MAP_SAVE_CHANGES,
#else
  std::array<MainUi::menu_items, 6> osm_active_items = { {
#endif
    MainUi::MENU_ITEM_MAP_UPLOAD,
    MainUi::MENU_ITEM_MAP_UNDO_CHANGES,
    MainUi::MENU_ITEM_MAP_RELATIONS,
    MainUi::SUBMENU_TRACK,
    MainUi::SUBMENU_VIEW,
    MainUi::SUBMENU_WMS
  } };
  for(unsigned int i = 0; i < osm_active_items.size(); i++)
    uicontrol->setActionEnable(osm_active_items[i], osm_valid);

  gtk_widget_set_sensitive(static_cast<appdata_internal *>(this)->btn_zoom_in, osm_valid);
  gtk_widget_set_sensitive(static_cast<appdata_internal *>(this)->btn_zoom_out, osm_valid);

  if(!project_valid)
    uicontrol->showNotification(_("Please load or create a project"));
}

/******************** begin of menu *********************/

static void
cb_menu_project_open(appdata_t *appdata) {
  const std::string &proj_name = project_select(*appdata);
  if(!proj_name.empty())
    project_load(*appdata, proj_name);
  appdata->main_ui_enable();
}

#ifndef FREMANTLE
static void
cb_menu_quit(appdata_t *appdata) {
  gtk_widget_destroy(appdata->window);
}
#endif

static void
cb_menu_upload(appdata_t *appdata) {
  if(!appdata->osm || !appdata->project) return;

  if(appdata->project->check_demo(appdata->window))
    return;

  osm_upload(*appdata, appdata->osm, appdata->project);
}

static void
cb_menu_download(appdata_t *appdata) {
  if(!appdata->project) return;

  if(appdata->project->check_demo(appdata->window))
    return;

  appdata->map->set_autosave(false);

  /* if we have valid osm data loaded: save state first */
  if(appdata->osm)
    diff_save(appdata->project, appdata->osm);

  // download
  if(osm_download(appdata->window, appdata->settings,
		  appdata->project)) {
    if(appdata->osm) {
      /* redraw the entire map by destroying all map items and redrawing them */
      appdata->map->clear(map_t::MAP_LAYER_OBJECTS_ONLY);

      delete appdata->osm;
    }

    appdata->uicontrol->showNotification(_("Drawing"), MainUi::Busy);
    appdata->osm = appdata->project->parse_osm();
    diff_restore(*appdata);
    appdata->map->paint();
    appdata->uicontrol->showNotification(O2G_NULLPTR, MainUi::Busy);
  }

  appdata->map->set_autosave(true);
  appdata->main_ui_enable();
}

static void
cb_menu_wms_adjust(appdata_t *appdata) {
  appdata->map->set_action(MAP_ACTION_BG_ADJUST);
}

/* ----------- hide objects for performance reasons ----------- */

static void
cb_menu_map_hide_sel(appdata_t *appdata) {
  appdata->map->hide_selected();
}

static void
cb_menu_map_show_all(appdata_t *appdata) {
  appdata->map->show_all();
}

/* ---------------------------------------------------------- */

GtkWidget *track_vis_select_widget(TrackVisibility current) {
  std::vector<std::string> labels;
  labels.push_back(_("Hide tracks"));
  labels.push_back(_("Show current position"));
  labels.push_back(_("Show current segment"));
  labels.push_back(_("Show all segments"));

  return osm2go_platform::string_select_widget(_("Track visibility"), labels,
                                               static_cast<int>(current));
}

#ifndef FREMANTLE
/* in fremantle this happens inside the submenu handling since this button */
/* is actually placed inside the submenu there */
static bool track_visibility_select(GtkWidget *parent, appdata_t &appdata) {
  g_widget dialog(gtk_dialog_new_with_buttons(_("Select track visibility"),
                                              GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                              GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                              GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                              O2G_NULLPTR));

  gtk_dialog_set_default_response(GTK_DIALOG(dialog.get()), GTK_RESPONSE_ACCEPT);

  GtkWidget *cbox = track_vis_select_widget(appdata.settings->trackVisibility);

  GtkWidget *hbox = gtk_hbox_new(FALSE, 8);
  gtk_box_pack_start_defaults(GTK_BOX(hbox), gtk_label_new(_("Track visibility:")));

  gtk_box_pack_start_defaults(GTK_BOX(hbox), cbox);
  gtk_box_pack_start_defaults(GTK_BOX(GTK_DIALOG(dialog.get())->vbox), hbox);

  gtk_widget_show_all(dialog.get());

  bool ret = false;
  if(GTK_RESPONSE_ACCEPT != gtk_dialog_run(GTK_DIALOG(dialog.get()))) {
    printf("user clicked cancel\n");
  } else {
    int index = combo_box_get_active(cbox);
    printf("user clicked ok on %i\n", index);

    TrackVisibility tv = static_cast<TrackVisibility>(index);
    ret = (tv != appdata.settings->trackVisibility);
    appdata.settings->trackVisibility = tv;
  }

  return ret;
}

static void
cb_menu_style(appdata_t *appdata) {
  style_select(appdata->window, *appdata);
}

static void
cb_menu_track_vis(appdata_t *appdata) {
  if(track_visibility_select(appdata->window, *appdata) && appdata->track.track)
    appdata->map->track_draw(appdata->settings->trackVisibility, *appdata->track.track);
}

static void
cb_menu_save_changes(appdata_t *appdata) {
  if(likely(appdata->project && appdata->osm))
    diff_save(appdata->project, appdata->osm);
  appdata->uicontrol->showNotification(_("Saved local changes"), MainUi::Brief);
}
#endif

static void
cb_menu_undo_changes(appdata_t *appdata) {
  // if there is nothing to clean then don't ask
  if (!diff_present(appdata->project) && appdata->osm->is_clean(true))
    return;

  if(!yes_no_f(appdata->window, 0, _("Undo all changes?"),
	       _("Throw away all the changes you've not "
		 "uploaded yet? This cannot be undone.")))
    return;

  appdata->map->clear(map_t::MAP_LAYER_OBJECTS_ONLY);

  delete appdata->osm;
  appdata->osm = O2G_NULLPTR;

  diff_remove(appdata->project);
  appdata->osm = appdata->project->parse_osm();
  appdata->map->paint();

  appdata->uicontrol->showNotification(_("Undo all changes"), MainUi::Brief);
}

static void
cb_menu_osm_relations(appdata_t *appdata) {
  /* list relations of all objects */
  relation_list(appdata->window, appdata->map, appdata->osm, appdata->presets);
}

#ifndef FREMANTLE
static void
cb_menu_fullscreen(appdata_t *appdata, MENU_CHECK_ITEM *item) {
  if(MENU_CHECK_ITEM_ACTIVE(item))
    gtk_window_fullscreen(GTK_WINDOW(appdata->window));
  else
    gtk_window_unfullscreen(GTK_WINDOW(appdata->window));
}
#endif

static void
cb_menu_zoomin(appdata_t *appdata) {
  if(!appdata->map) return;

  appdata->map->set_zoom(appdata->map->state.zoom * ZOOM_FACTOR_MENU, true);
  printf("zoom is now %f\n", appdata->map->state.zoom);
}

static void
cb_menu_zoomout(appdata_t *appdata) {
  if(!appdata->map) return;

  appdata->map->set_zoom(appdata->map->state.zoom / ZOOM_FACTOR_MENU, true);
  printf("zoom is now %f\n", appdata->map->state.zoom);
}

static void
cb_menu_view_detail_inc(appdata_t *appdata) {
  printf("detail level increase\n");
  appdata->map->detail_increase();
}

#ifndef FREMANTLE
static void
cb_menu_view_detail_normal(appdata_t *appdata) {
  printf("detail level normal\n");
  appdata->map->detail_normal();
}
#endif

static void
cb_menu_view_detail_dec(appdata_t *appdata) {
  printf("detail level decrease\n");
  appdata->map->detail_decrease();
}

static void
cb_menu_track_import(appdata_t *appdata) {
  assert(appdata->settings != O2G_NULLPTR);

  /* open a file selector */
  g_widget dialog(
#ifdef FREMANTLE
                  hildon_file_chooser_dialog_new(GTK_WINDOW(appdata->window),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN)
#else
                  gtk_file_chooser_dialog_new (_("Import track file"),
                                               GTK_WINDOW(appdata->window),
                                               GTK_FILE_CHOOSER_ACTION_OPEN,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                               GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                               O2G_NULLPTR)
#endif
           );

  if(!appdata->settings->track_path.empty()) {
    if(!g_file_test(appdata->settings->track_path.c_str(), G_FILE_TEST_EXISTS)) {
      std::string::size_type slashpos = appdata->settings->track_path.rfind('/');
      if(slashpos != std::string::npos) {
        appdata->settings->track_path[slashpos] = '\0';  // seperate path from file

	/* the user just created a new document */
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog.get()),
                                            appdata->settings->track_path.c_str());
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog.get()),
                                          appdata->settings->track_path.c_str() + slashpos + 1);

	/* restore full filename */
        appdata->settings->track_path[slashpos] = '/';
      }
    } else
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog.get()),
                                    appdata->settings->track_path.c_str());
  }

  gtk_widget_show_all(GTK_WIDGET(dialog.get()));
  if (gtk_dialog_run(GTK_DIALOG(dialog.get())) == GTK_FM_OK) {
    g_string filename(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog.get())));

    /* remove any existing track */
    appdata->track_clear();

    /* load a track */
    appdata->track.track = track_import(filename.get());
    if(appdata->track.track) {
      appdata->map->track_draw(appdata->settings->trackVisibility, *appdata->track.track);

      appdata->settings->track_path = filename.get();
    }
    track_menu_set(*appdata);
  }
}

static void
cb_menu_track_enable_gps(appdata_t *appdata, MENU_CHECK_ITEM *item) {
  track_enable_gps(*appdata, MENU_CHECK_ITEM_ACTIVE(item));
}


static void
cb_menu_track_follow_gps(appdata_t *appdata, MENU_CHECK_ITEM *item) {
  appdata->settings->follow_gps = MENU_CHECK_ITEM_ACTIVE(item);
}


static void
cb_menu_track_export(appdata_t *appdata) {
  assert(appdata->settings != O2G_NULLPTR);

  /* open a file selector */
  g_widget dialog(
#ifdef FREMANTLE
                  hildon_file_chooser_dialog_new(GTK_WINDOW(appdata->window),
                                                 GTK_FILE_CHOOSER_ACTION_SAVE)
#else
                  gtk_file_chooser_dialog_new(_("Export track file"),
                                              GTK_WINDOW(appdata->window),
                                              GTK_FILE_CHOOSER_ACTION_SAVE,
                                              GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                              GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
                                              O2G_NULLPTR)
#endif
           );

  printf("set filename <%s>\n", appdata->settings->track_path.c_str());

  if(!appdata->settings->track_path.empty()) {
    if(!g_file_test(appdata->settings->track_path.c_str(), G_FILE_TEST_EXISTS)) {
      std::string::size_type slashpos = appdata->settings->track_path.rfind('/');
      if(slashpos != std::string::npos) {
        appdata->settings->track_path[slashpos] = '\0';  // seperate path from file

	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog.get()),
                                            appdata->settings->track_path.c_str());
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog.get()),
                                          appdata->settings->track_path.c_str() + slashpos + 1);

	/* restore full filename */
        appdata->settings->track_path[slashpos] = '/';
      }
    } else
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog.get()),
                                    appdata->settings->track_path.c_str());
  }

  if(gtk_dialog_run(GTK_DIALOG(dialog.get())) == GTK_FM_OK) {
    g_string filename(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog.get())));
    if(filename) {
      printf("export to %s\n", filename.get());

      if(!g_file_test(filename.get(), G_FILE_TEST_EXISTS) ||
         yes_no_f(dialog.get(), MISC_AGAIN_ID_EXPORT_OVERWRITE | MISC_AGAIN_FLAG_DONT_SAVE_NO,
                  _("Overwrite existing file"),
                  _("The file already exists. "
                    "Do you really want to replace it?"))) {
        appdata->settings->track_path = filename.get();

        assert(appdata->track.track != O2G_NULLPTR);
        track_export(appdata->track.track, filename.get());
      }
    }
  }
}

/*
 *  Platform-specific UI tweaks.
 */

static void track_clear_cb(appdata_t *appdata) {
  appdata->track_clear();
}

static void about_box(appdata_t *appdata)
{
  appdata->uicontrol->about_box();
}

#ifndef FREMANTLE
// Half-arsed slapdash common menu item constructor. Let's use GtkBuilder
// instead so we have some flexibility.

/**
 * @brief create a new submenu entry
 * @param appdata the appdata object as callback reference
 * @param menu_shell the menu to attach to
 * @param activate_cb the function to be called on selection
 * @param label the label to show (may be nullptr in case of item being set)
 * @param icon_name stock id or name for icon_load
 * @param accel_path accel database key (must be a static string)
 * @param accel_key key setting from gdk/gdkkeysyms.h
 * @param accel_mods accel modifiers
 * @param enabled if the new item is enabled or disabled
 * @param is_check if the new item should be a checkable item
 * @param check_status the initial status of the check item
 */
static GtkWidget *
menu_append_new_item(appdata_t &appdata, GtkWidget *menu_shell,
                     GCallback activate_cb, const char *label,
                     const gchar *icon_name,
                     const gchar *accel_path,
                     guint accel_key = 0,
                     GdkModifierType accel_mods = static_cast<GdkModifierType>(0),
                     bool enabled = true,
                     bool is_check = false, gboolean check_status = FALSE)
{
  GtkWidget *item = O2G_NULLPTR;

  bool stock_item_known = false;
  GtkStockItem stock_item;
  if (icon_name != O2G_NULLPTR) {
    stock_item_known = gtk_stock_lookup(icon_name, &stock_item) == TRUE;
  }

  // Icons
  if (is_check) {
    item = gtk_check_menu_item_new_with_mnemonic (label);
  }
  else if (!stock_item_known) {
    GtkWidget *image = O2G_NULLPTR;
    if(icon_name)
      image = appdata.icons.widget_load(icon_name);
    if (image) {
      item = gtk_image_menu_item_new_with_mnemonic(label);
      gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), image);
    }
    else {
      item = gtk_menu_item_new_with_mnemonic(label);
    }
  }
  else {
    item = gtk_image_menu_item_new_with_mnemonic(label);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item),
                                  gtk_image_new_from_stock(icon_name, GTK_ICON_SIZE_MENU));
  }

  // Accelerators
  // Default
  if (accel_path != O2G_NULLPTR) {
    accel_path = g_intern_static_string(accel_path);
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(item), accel_path);
    if (accel_key != 0) {
      gtk_accel_map_add_entry( accel_path, accel_key, accel_mods );
    }
    else if (stock_item_known) {
      gtk_accel_map_add_entry( accel_path, stock_item.keyval,
                               stock_item.modifier );
    }
  }

  gtk_menu_shell_append(GTK_MENU_SHELL(menu_shell), GTK_WIDGET(item));
  if(!enabled)
    gtk_widget_set_sensitive(GTK_WIDGET(item), FALSE);
  if (is_check)
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), check_status);

  g_signal_connect_swapped(item, "activate", activate_cb, &appdata);
  return item;
}

static void menu_create(appdata_internal &appdata, GtkBox *mainvbox) {
  GtkWidget *menu, *item, *submenu;
  GtkWidget *about_quit_items_menu;

  menu = gtk_menu_bar_new();

  /* -------------------- Project submenu -------------------- */

  GtkAccelGroup *accel_grp = gtk_accel_group_new();

  item = gtk_menu_item_new_with_mnemonic( _("_Project") );
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_grp);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
  about_quit_items_menu = submenu;

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_project_open), _("_Open"),
    GTK_STOCK_OPEN, "<OSM2Go-Main>/Project/Open");

  /* --------------- view menu ------------------- */

  appdata.menuitems[MainUi::SUBMENU_VIEW] = item = gtk_menu_item_new_with_mnemonic( _("_View") );
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_grp);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

  appdata.menu_item_view_fullscreen = GTK_CHECK_MENU_ITEM(menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_fullscreen), _("_Fullscreen"),
    GTK_STOCK_FULLSCREEN, "<OSM2Go-Main>/View/Fullscreen"));

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_zoomin), _("Zoom _in"),
    GTK_STOCK_ZOOM_IN, "<OSM2Go-Main>/View/ZoomIn",
    GDK_comma, GDK_CONTROL_MASK);

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_zoomout), _("Zoom _out"),
    GTK_STOCK_ZOOM_OUT, "<OSM2Go-Main>/View/ZoomOut",
    GDK_period, GDK_CONTROL_MASK);

  gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_view_detail_inc), _("More details"),
    O2G_NULLPTR, "<OSM2Go-Main>/View/DetailInc", GDK_period, GDK_MOD1_MASK);

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_view_detail_normal), _("Normal details"),
    O2G_NULLPTR, "<OSM2Go-Main>/View/DetailNormal");

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_view_detail_dec), _("Less details"),
    O2G_NULLPTR, "<OSM2Go-Main>/View/DetailDec", GDK_comma, GDK_MOD1_MASK);

  gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

  appdata.menuitems[MainUi::MENU_ITEM_MAP_HIDE_SEL] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_map_hide_sel), _("_Hide selected"),
    GTK_STOCK_REMOVE, "<OSM2Go-Main>/View/HideSelected",
    0, static_cast<GdkModifierType>(0), false);

  appdata.menuitems[MainUi::MENU_ITEM_MAP_SHOW_ALL] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_map_show_all), _("_Show all"),
    GTK_STOCK_ADD, "<OSM2Go-Main>/View/ShowAll",
    0, static_cast<GdkModifierType>(0), false);

  gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_style), _("St_yle"),
    GTK_STOCK_SELECT_COLOR, "<OSM2Go-Main>/View/Style");

  /* -------------------- map submenu -------------------- */

  appdata.menuitems[MainUi::SUBMENU_MAP] = item = gtk_menu_item_new_with_mnemonic( _("_Map") );
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_grp);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

  appdata.menuitems[MainUi::MENU_ITEM_MAP_UPLOAD] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_upload), _("_Upload"),
    "upload.16", "<OSM2Go-Main>/Map/Upload",
    GDK_u, static_cast<GdkModifierType>(GDK_SHIFT_MASK|GDK_CONTROL_MASK));

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_download), _("_Download"),
    "download.16", "<OSM2Go-Main>/Map/Download",
    GDK_d, static_cast<GdkModifierType>(GDK_SHIFT_MASK|GDK_CONTROL_MASK));

  gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

  appdata.menuitems[MainUi::MENU_ITEM_MAP_SAVE_CHANGES] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_save_changes), _("_Save local changes"),
    GTK_STOCK_SAVE, "<OSM2Go-Main>/Map/SaveChanges",
    GDK_s, static_cast<GdkModifierType>(GDK_SHIFT_MASK|GDK_CONTROL_MASK));

  appdata.menuitems[MainUi::MENU_ITEM_MAP_UNDO_CHANGES] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_undo_changes), _("Undo _all"),
    GTK_STOCK_DELETE, "<OSM2Go-Main>/Map/UndoAll");

  gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());
  appdata.menuitems[MainUi::MENU_ITEM_MAP_RELATIONS] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_osm_relations), _("_Relations"),
    O2G_NULLPTR, "<OSM2Go-Main>/Map/Relations",
    GDK_r, static_cast<GdkModifierType>(GDK_SHIFT_MASK|GDK_CONTROL_MASK));

  /* -------------------- wms submenu -------------------- */

  appdata.menuitems[MainUi::SUBMENU_WMS] = item = gtk_menu_item_new_with_mnemonic( _("_WMS") );
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_grp);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(wms_import), _("_Import"),
    GTK_STOCK_INDEX, "<OSM2Go-Main>/WMS/Import");

  appdata.menuitems[MainUi::MENU_ITEM_WMS_CLEAR] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(wms_remove), _("_Clear"),
    GTK_STOCK_CLEAR, "<OSM2Go-Main>/WMS/Clear",
    0, static_cast<GdkModifierType>(0), false);

  appdata.menuitems[MainUi::MENU_ITEM_WMS_ADJUST] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_wms_adjust), _("_Adjust"),
    O2G_NULLPTR, "<OSM2Go-Main>/WMS/Adjust",
    0, static_cast<GdkModifierType>(0), false);

  /* -------------------- track submenu -------------------- */

  appdata.menuitems[MainUi::SUBMENU_TRACK] = item =
    gtk_menu_item_new_with_mnemonic(_("_Track"));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  submenu = gtk_menu_new();
  gtk_menu_set_accel_group(GTK_MENU(submenu), accel_grp);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);

  appdata.menuitems[MainUi::MENU_ITEM_TRACK_IMPORT] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_track_import), _("_Import"),
    O2G_NULLPTR, "<OSM2Go-Main>/Track/Import");

  appdata.menuitems[MainUi::MENU_ITEM_TRACK_EXPORT] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_track_export), _("_Export"),
    O2G_NULLPTR, "<OSM2Go-Main>/Track/Export",
    0, static_cast<GdkModifierType>(0), false);

  appdata.menuitems[MainUi::MENU_ITEM_TRACK_CLEAR] = item = menu_append_new_item(
    appdata, submenu, G_CALLBACK(track_clear_cb), _("_Clear"),
    GTK_STOCK_CLEAR, "<OSM2Go-Main>/Track/Clear",
    0, static_cast<GdkModifierType>(0), false);

  appdata.menuitems[MainUi::MENU_ITEM_TRACK_ENABLE_GPS] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_track_enable_gps),_("_GPS enable"),
    O2G_NULLPTR, "<OSM2Go-Main>/Track/GPS",
    GDK_g, static_cast<GdkModifierType>(GDK_CONTROL_MASK|GDK_SHIFT_MASK), true, true,
    appdata.settings->enable_gps
  );

  appdata.menuitems[MainUi::MENU_ITEM_TRACK_FOLLOW_GPS] = menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_track_follow_gps), _("GPS follow"),
    O2G_NULLPTR, "<OSM2Go-Main>/Track/Follow",
    0, static_cast<GdkModifierType>(0), appdata.settings->enable_gps, true,
    appdata.settings->follow_gps
  );

  menu_append_new_item(
    appdata, submenu, G_CALLBACK(cb_menu_track_vis), _("Track _visibility"),
    O2G_NULLPTR, "<OSM2Go-Main>/Track/GPS",
    GDK_g, static_cast<GdkModifierType>(GDK_CONTROL_MASK|GDK_SHIFT_MASK));

  /* ------------------------------------------------------- */

  gtk_menu_shell_append(GTK_MENU_SHELL(about_quit_items_menu),
                        gtk_separator_menu_item_new());

  menu_append_new_item(
    appdata, about_quit_items_menu, G_CALLBACK(about_box), _("_About"),
    GTK_STOCK_ABOUT, "<OSM2Go-Main>/About");

  menu_append_new_item(
    appdata, about_quit_items_menu, G_CALLBACK(cb_menu_quit), _("_Quit"),
    GTK_STOCK_QUIT, "<OSM2Go-Main>/Quit");

  gtk_window_add_accel_group(GTK_WINDOW(appdata.window), accel_grp);

  gtk_box_pack_start(mainvbox, menu, 0, 0, 0);

}

#else // !FREMANTLE

struct menu_entry_t {
  typedef gboolean (*toggle_cb)(appdata_t &appdata);
  explicit menu_entry_t(const char *l, GCallback cb = O2G_NULLPTR,
                        int idx = -1, gboolean en = TRUE, toggle_cb tg = O2G_NULLPTR)
    : label(l), enabled(en), toggle(tg), menuindex(idx), activate_cb(cb) {}
  const char *label;
  gboolean enabled;
  toggle_cb toggle;
  int menuindex;
  GCallback activate_cb;
};

static gboolean enable_gps_get_toggle(appdata_t &appdata) {
  return appdata.settings->enable_gps;
}

static gboolean follow_gps_get_toggle(appdata_t &appdata) {
  return appdata.settings->follow_gps;
}

#define COLUMNS  2

static void on_submenu_entry_clicked(GtkWidget *menu)
{
  /* force closing of submenu dialog */
  gtk_dialog_response(GTK_DIALOG(menu), GTK_RESPONSE_NONE);
  gtk_widget_hide(menu);

  /* let gtk clean up */
  osm2go_platform::process_events();
}

/* use standard dialog boxes for fremantle submenues */
static GtkWidget *app_submenu_create(appdata_t &appdata, MainUi::menu_items submenu,
                                     const menu_entry_t *menu, const unsigned int rows) {
  const char *title = hildon_button_get_title(HILDON_BUTTON(appdata.menuitems[submenu]));
  /* create a oridinary dialog box */
  GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(appdata.window),
                                                  GTK_DIALOG_MODAL, O2G_NULLPTR);

  osm2go_platform::dialog_size_hint(GTK_WINDOW(dialog), MISC_DIALOG_SMALL);
  gtk_dialog_set_has_separator(GTK_DIALOG(dialog), FALSE);

  GtkWidget *table = gtk_table_new(rows / COLUMNS, COLUMNS, TRUE);

  for(unsigned int idx = 0; idx < rows; idx++) {
    const menu_entry_t *menu_entries = menu + idx;
    GtkWidget *button;

    /* the "Style" menu entry is very special */
    /* and is being handled seperately */
    if(!strcmp(_("Style"), menu_entries->label)) {
      button = style_select_widget(appdata.settings->style);
      g_object_set_data(G_OBJECT(dialog), "style_widget", button);
    } else if(!strcmp(_("Track visibility"), menu_entries->label)) {
      button = track_vis_select_widget(appdata.settings->trackVisibility);
      g_object_set_data(G_OBJECT(dialog), "track_widget", button);
    } else if(!menu_entries->toggle) {
      button = hildon_button_new_with_text(
                 static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH),
                                           HILDON_BUTTON_ARRANGEMENT_VERTICAL,
                                           _(menu_entries->label), O2G_NULLPTR);

      g_signal_connect_swapped(button, "clicked",
                               G_CALLBACK(on_submenu_entry_clicked), dialog);

      g_signal_connect_swapped(button, "clicked",
                               menu_entries->activate_cb, &appdata);

      hildon_button_set_title_alignment(HILDON_BUTTON(button), 0.5, 0.5);
      hildon_button_set_value_alignment(HILDON_BUTTON(button), 0.5, 0.5);
    } else {
      button = hildon_check_button_new(HILDON_SIZE_AUTO);
      gtk_button_set_label(GTK_BUTTON(button), _(menu_entries->label));
      printf("requesting check for %s: %p\n", menu_entries->label, menu_entries->toggle);
      hildon_check_button_set_active(HILDON_CHECK_BUTTON(button), menu_entries->toggle(appdata));

      g_signal_connect_swapped(button, "clicked",
                               G_CALLBACK(on_submenu_entry_clicked), dialog);

      g_signal_connect_swapped(button, "toggled",
                               menu_entries->activate_cb, &appdata);

      gtk_button_set_alignment(GTK_BUTTON(button), 0.5, 0.5);
    }

    /* index to GtkWidget pointer array was given -> store pointer */
    if(menu_entries->menuindex >= 0)
      appdata.menuitems[menu_entries->menuindex] = button;

    gtk_widget_set_sensitive(button, menu_entries->enabled);

    const guint x = idx % COLUMNS, y = idx / COLUMNS;
    gtk_table_attach_defaults(GTK_TABLE(table),  button, x, x+1, y, y+1);
  }


  gtk_box_pack_start_defaults(GTK_BOX(GTK_DIALOG(dialog)->vbox), table);

  g_object_ref(dialog);
  return dialog;
}

/* popup the dialog shaped submenu */
static void submenu_popup(appdata_t &appdata, GtkWidget *menu) {
  gtk_widget_show_all(menu);
  gtk_dialog_run(GTK_DIALOG(menu));
  gtk_widget_hide(menu);

  /* check if the style menu was in here */
  GtkWidget *combo_widget = GTK_WIDGET(g_object_get_data(G_OBJECT(menu), "style_widget"));
  if(combo_widget) {
    style_change(appdata, combo_widget);
  } else if((combo_widget = GTK_WIDGET(g_object_get_data(G_OBJECT(menu), "track_widget"))) != O2G_NULLPTR) {
    TrackVisibility tv = static_cast<TrackVisibility>(combo_box_get_active(combo_widget));
    if(tv != appdata.settings->trackVisibility && appdata.track.track)
      appdata.map->track_draw(tv, *appdata.track.track);
    appdata.settings->trackVisibility = tv;
  }
}

/* the view submenu */
static void on_submenu_view_clicked(appdata_internal *appdata)
{
  submenu_popup(*appdata, appdata->app_menu_view.get());
}

static void on_submenu_map_clicked(appdata_internal *appdata)
{
  submenu_popup(*appdata, appdata->app_menu_map.get());
}

static void on_submenu_wms_clicked(appdata_internal *appdata)
{
  submenu_popup(*appdata, appdata->app_menu_wms.get());
}

static void on_submenu_track_clicked(appdata_internal *appdata)
{
  submenu_popup(*appdata, appdata->app_menu_track.get());
}

/* create a HildonAppMenu */
static HildonAppMenu *app_menu_create(appdata_t &appdata) {
  HildonAppMenu *menu = HILDON_APP_MENU(hildon_app_menu_new());
  /* -- the applications main menu -- */
  std::array<menu_entry_t, 7> main_menu = { {
    menu_entry_t(_("About"),     G_CALLBACK(about_box)),
    menu_entry_t(_("Project"),   G_CALLBACK(cb_menu_project_open)),
    menu_entry_t(_("View"),      G_CALLBACK(on_submenu_view_clicked),  MainUi::SUBMENU_VIEW),
    menu_entry_t(_("OSM"),       G_CALLBACK(on_submenu_map_clicked),   MainUi::SUBMENU_MAP),
    menu_entry_t(_("Relations"), G_CALLBACK(cb_menu_osm_relations),    MainUi::MENU_ITEM_MAP_RELATIONS),
    menu_entry_t(_("WMS"),       G_CALLBACK(on_submenu_wms_clicked),   MainUi::SUBMENU_WMS),
    menu_entry_t(_("Track"),     G_CALLBACK(on_submenu_track_clicked), MainUi::SUBMENU_TRACK)
  } };

  for(unsigned int i = 0; i < main_menu.size(); i++) {
    const menu_entry_t &entry = main_menu[i];
    GtkWidget *button = O2G_NULLPTR;

    assert_null(entry.toggle);
    button = hildon_button_new_with_text(
                static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH),
                HILDON_BUTTON_ARRANGEMENT_VERTICAL,
                entry.label, O2G_NULLPTR);

    g_signal_connect_data(button, "clicked",
                          entry.activate_cb, &appdata, O2G_NULLPTR,
                          static_cast<GConnectFlags>(G_CONNECT_AFTER | G_CONNECT_SWAPPED));
    hildon_button_set_title_alignment(HILDON_BUTTON(button), 0.5, 0.5);
    hildon_button_set_value_alignment(HILDON_BUTTON(button), 0.5, 0.5);

    /* index to GtkWidget pointer array was given -> store pointer */
    if(entry.menuindex >= 0)
      appdata.menuitems[entry.menuindex] = button;

    gtk_widget_set_sensitive(button, entry.enabled);

    hildon_app_menu_append(menu, GTK_BUTTON(button));
  }

  gtk_widget_show_all(GTK_WIDGET(menu));
  return menu;
}

static void menu_create(appdata_internal &appdata, GtkBox *) {
  /* -- the view submenu -- */
  const std::array<menu_entry_t, 3> sm_view_entries = { {
    /* --- */
    menu_entry_t(_("Style")),
    /* --- */
    menu_entry_t(_("Hide selected"), G_CALLBACK(cb_menu_map_hide_sel), MainUi::MENU_ITEM_MAP_HIDE_SEL, FALSE),
    menu_entry_t(_("Show all"),      G_CALLBACK(cb_menu_map_show_all), MainUi::MENU_ITEM_MAP_SHOW_ALL, FALSE),
  } };

  /* -- the map submenu -- */
  const std::array<menu_entry_t, 3> sm_map_entries = { {
    menu_entry_t(_("Upload"),   G_CALLBACK(cb_menu_upload), MainUi::MENU_ITEM_MAP_UPLOAD),
    menu_entry_t(_("Download"), G_CALLBACK(cb_menu_download)),
    menu_entry_t(_("Undo all"), G_CALLBACK(cb_menu_undo_changes), MainUi::MENU_ITEM_MAP_UNDO_CHANGES),
  } };

  /* -- the wms submenu -- */
  const std::array<menu_entry_t, 3> sm_wms_entries = { {
    menu_entry_t(_("Import"), G_CALLBACK(wms_import)),
    menu_entry_t(_("Clear"),  G_CALLBACK(wms_remove), MainUi::MENU_ITEM_WMS_CLEAR, FALSE),
    menu_entry_t(_("Adjust"), G_CALLBACK(cb_menu_wms_adjust), MainUi::MENU_ITEM_WMS_ADJUST, FALSE),
  } };

  /* -- the track submenu -- */
  const std::array<menu_entry_t, 6> sm_track_entries = { {
    menu_entry_t(_("Import"),     G_CALLBACK(cb_menu_track_import), MainUi::MENU_ITEM_TRACK_IMPORT),
    menu_entry_t(_("Export"),     G_CALLBACK(cb_menu_track_export), MainUi::MENU_ITEM_TRACK_EXPORT, FALSE),
    menu_entry_t(_("Clear"),      G_CALLBACK(track_clear_cb), MainUi::MENU_ITEM_TRACK_CLEAR, FALSE),
    menu_entry_t(_("GPS enable"), G_CALLBACK(cb_menu_track_enable_gps),
                 MainUi::MENU_ITEM_TRACK_ENABLE_GPS, TRUE,
                 enable_gps_get_toggle),
    menu_entry_t(_("GPS follow"), G_CALLBACK(cb_menu_track_follow_gps),
                 MainUi::MENU_ITEM_TRACK_FOLLOW_GPS, appdata.settings->enable_gps ? TRUE : FALSE,
                 follow_gps_get_toggle),
    menu_entry_t(_("Track visibility")),
  } };

  /* build menu/submenus */
  HildonAppMenu *menu = app_menu_create(appdata);
  appdata.app_menu_wms.reset(app_submenu_create(appdata, MainUi::SUBMENU_WMS,
                                                sm_wms_entries.data(), sm_wms_entries.size()));
  appdata.app_menu_map.reset(app_submenu_create(appdata, MainUi::SUBMENU_MAP,
                                                sm_map_entries.data(), sm_map_entries.size()));
  appdata.app_menu_view.reset(app_submenu_create(appdata, MainUi::SUBMENU_VIEW,
                                                 sm_view_entries.data(), sm_view_entries.size()));
  appdata.app_menu_track.reset(app_submenu_create(appdata, MainUi::SUBMENU_TRACK,
                                                  sm_track_entries.data(), sm_track_entries.size()));

  hildon_window_set_app_menu(HILDON_WINDOW(appdata.window), menu);
}
#endif

/********************* end of menu **********************/


static void menu_accels_load(appdata_t *appdata) {
#ifndef FREMANTLE
#define ACCELS_FILE "accels"

  const std::string &accels_file = appdata->settings->base_path + ACCELS_FILE;
  gtk_accel_map_load(accels_file.c_str());
#else
  (void) appdata;
#endif
}

appdata_t::appdata_t(map_state_t &mstate)
  : uicontrol(MainUi::instance(*this))
  , window(O2G_NULLPTR)
  , statusbar(statusbar_t::create())
  , project(O2G_NULLPTR)
  , iconbar(O2G_NULLPTR)
  , presets(O2G_NULLPTR)
  , map_state(mstate)
  , map(O2G_NULLPTR)
  , osm(O2G_NULLPTR)
  , settings(settings_t::load())
  , icons(icon_t::instance())
  , style(style_load(settings->style))
  , gps_state(gps_state_t::create())
{
  // the TR1 header has assign() for what is later called fill()
#if __cplusplus >= 201103L
  menuitems.fill(O2G_NULLPTR);
#else
  menuitems.assign(O2G_NULLPTR);
#endif
  memset(&track, 0, sizeof(track));
}

appdata_t::~appdata_t() {
  printf("cleaning up ...\n");

#ifndef FREMANTLE
  const std::string &accels_file = settings->base_path + ACCELS_FILE;
  gtk_accel_map_save(accels_file.c_str());
#endif

  settings->save();

  if(likely(map))
    map->set_autosave(false);

  printf("waiting for gtk to shut down ");

  /* let gtk clean up first */
  osm2go_platform::process_events(true);

  printf(" ok\n");

  /* save project file */
  if(project)
    project->save(O2G_NULLPTR);

  delete osm;
  osm = O2G_NULLPTR;

  josm_presets_free(presets);

  delete gps_state;
  delete settings;
  delete style;
  delete statusbar;
  delete iconbar;
  delete project;

  puts("everything is gone");
}

void appdata_t::track_clear()
{
  track_t *tr = track.track;
  if (!tr)
    return;

  printf("clearing track\n");

  if(likely(map != O2G_NULLPTR))
    map_track_remove(*tr);

  track.track = O2G_NULLPTR;
  track_menu_set(*this);

  delete tr;
}

appdata_internal::appdata_internal(map_state_t &mstate)
  : appdata_t(mstate)
#ifdef FREMANTLE
  , program(O2G_NULLPTR)
  , app_menu_view(O2G_NULLPTR)
  , app_menu_wms(O2G_NULLPTR)
  , app_menu_track(O2G_NULLPTR)
  , app_menu_map(O2G_NULLPTR)
#else
  , menu_item_view_fullscreen(O2G_NULLPTR)
#endif
  , btn_zoom_in(O2G_NULLPTR)
  , btn_zoom_out(O2G_NULLPTR)
{
}

appdata_internal::~appdata_internal()
{
#ifdef FREMANTLE
  program = O2G_NULLPTR;
#endif
}

static void on_window_destroy(appdata_t *appdata) {
  puts("main window destroy");

  gtk_main_quit();
  appdata->window = O2G_NULLPTR;
}

static gboolean on_window_key_press(appdata_internal *appdata, GdkEventKey *event) {
  gboolean handled = FALSE;

  //  printf("key event with keyval %x\n", event->keyval);

  // the map handles some keys on its own ...
  switch(event->keyval) {

#ifndef FREMANTLE
  case GDK_F11:
    if(!gtk_check_menu_item_get_active(appdata->menu_item_view_fullscreen)) {
      gtk_window_fullscreen(GTK_WINDOW(appdata->window));
      gtk_check_menu_item_set_active(appdata->menu_item_view_fullscreen, TRUE);
    } else {
      gtk_window_unfullscreen(GTK_WINDOW(appdata->window));
      gtk_check_menu_item_set_active(appdata->menu_item_view_fullscreen, FALSE);
    }

    handled = TRUE;
    break;
#endif // !FREMANTLE
  }

  /* forward unprocessed key presses to map */
  if(!handled && appdata->project && appdata->osm && event->type == GDK_KEY_PRESS)
    handled = appdata->map->key_press_event(event->keyval) ? TRUE : FALSE;

  return handled;
}

#if defined(FREMANTLE) && !defined(__i386__)
/* get access to zoom buttons */
static void
on_window_realize(GtkWidget *widget, gpointer) {
  if (widget->window) {
    unsigned char value = 1;
    Atom hildon_zoom_key_atom =
      gdk_x11_get_xatom_by_name("_HILDON_ZOOM_KEY_ATOM"),
      integer_atom = gdk_x11_get_xatom_by_name("INTEGER");
    Display *dpy =
      GDK_DISPLAY_XDISPLAY(gdk_drawable_get_display(widget->window));
    Window w = GDK_WINDOW_XID(widget->window);

    XChangeProperty(dpy, w, hildon_zoom_key_atom,
		    integer_atom, 8, PropModeReplace, &value, 1);
  }
}
#endif

static GtkWidget *  __attribute__((nonnull(1,2,4)))
                  icon_button(appdata_t *appdata, const char *icon, GCallback cb,
			      GtkWidget *box) {
  GtkWidget *but = gtk_button_new();
  const int icon_scale =
#ifdef FREMANTLE
    -1;
#else
    24;
#endif
  GtkWidget *iconw = appdata->icons.widget_load(icon, icon_scale);
#ifndef FREMANTLE
  // explicitely assign image so the button does not show the action text
  if(iconw == O2G_NULLPTR)
    // gtk_image_new_from_stock() can't be used first, as it will return non-null even if nothing is found
    iconw = gtk_image_new_from_stock(icon, GTK_ICON_SIZE_MENU);
#endif
  gtk_button_set_image(GTK_BUTTON(but), iconw);
#ifdef FREMANTLE
  //  gtk_button_set_relief(GTK_BUTTON(but), GTK_RELIEF_NONE);
  hildon_gtk_widget_set_theme_size(but,
            static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH));

  if(cb)
#endif
    g_signal_connect_swapped(but, "clicked", cb, appdata);

  gtk_box_pack_start(GTK_BOX(box), but, FALSE, FALSE, 0);
  return but;
}

static int application_run(const char *proj)
{
  /* user specific init */
  map_state_t map_state;
  appdata_internal appdata(map_state);

  if(unlikely(!appdata.style)) {
    errorf(O2G_NULLPTR, _("Unable to load valid style %s, terminating."),
           appdata.settings->style.c_str());
    return -1;
  }

#ifdef FREMANTLE
  /* Create the hildon program and setup the title */
  appdata.program = HILDON_PROGRAM(hildon_program_get_instance());
  g_set_application_name("OSM2Go");

  /* Create HildonWindow and set it to HildonProgram */
  HildonWindow *wnd = HILDON_WINDOW(hildon_stackable_window_new());
  appdata.window = GTK_WIDGET(wnd);
  hildon_program_add_window(appdata.program, wnd);

  /* try to enable the zoom buttons. don't do this on x86 as it breaks */
  /* at runtime with cygwin x */
#if !defined(__i386__)
  g_signal_connect(appdata.window, "realize", G_CALLBACK(on_window_realize), O2G_NULLPTR);
#endif // FREMANTLE

#else
  /* Create a Window. */
  appdata.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(appdata.window), "OSM2Go");
  /* Set a decent default size for the window. */
  gtk_window_set_default_size(GTK_WINDOW(appdata.window),
			      DEFAULT_WIDTH, DEFAULT_HEIGHT);
  gtk_window_set_icon(GTK_WINDOW(appdata.window),
		      appdata.icons.load(PACKAGE)->buffer());
#endif

  g_signal_connect_swapped(appdata.window, "key_press_event",
                           G_CALLBACK(on_window_key_press), &appdata);
  g_signal_connect_swapped(appdata.window, "destroy",
                           G_CALLBACK(on_window_destroy), &appdata);

  GtkBox *mainvbox = GTK_BOX(gtk_vbox_new(FALSE, 0));

  /* unconditionally enable the GPS */
  appdata.settings->enable_gps = TRUE;
  menu_create(appdata, mainvbox);

  menu_accels_load(&appdata);

  /* ----------------------- setup main window ---------------- */

  /* generate main map view */
  appdata.map = map_t::create(appdata);
  if(unlikely(!appdata.map))
    return -1;

  /* if tracking is enable, start it now */
  track_enable_gps(appdata, appdata.settings->enable_gps);

  GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
  GtkWidget *hbox = gtk_hbox_new(FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hbox), iconbar_t::create(appdata), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), appdata.map->canvas->widget, TRUE, TRUE, 0);

  /* fremantle has seperate zoom/details buttons on the right screen side */
#ifndef FREMANTLE
  GtkWidget *zhbox = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start_defaults(GTK_BOX(zhbox), appdata.statusbar->widget);

  icon_button(&appdata, "detailup_thumb",   G_CALLBACK(cb_menu_view_detail_inc), zhbox);
  icon_button(&appdata, "detaildown_thumb", G_CALLBACK(cb_menu_view_detail_dec), zhbox);
  appdata.btn_zoom_out = icon_button(&appdata, GTK_STOCK_ZOOM_OUT, G_CALLBACK(cb_menu_zoomout), zhbox);
  appdata.btn_zoom_in = icon_button(&appdata, GTK_STOCK_ZOOM_IN, G_CALLBACK(cb_menu_zoomin), zhbox);

  gtk_box_pack_start(GTK_BOX(vbox), zhbox, FALSE, FALSE, 0);
#else
  gtk_box_pack_start(GTK_BOX(vbox), appdata.statusbar->widget, FALSE, FALSE, 0);
#endif

  gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

#ifdef FREMANTLE
  /* fremantle has a set of buttons on the right screen side as well */
  vbox = gtk_vbox_new(FALSE, 0);

  GtkWidget *ivbox = gtk_vbox_new(FALSE, 0);
  appdata.btn_zoom_in =
    icon_button(&appdata, "zoomin_thumb",   G_CALLBACK(cb_menu_zoomin), ivbox);
  appdata.btn_zoom_out =
    icon_button(&appdata, "zoomout_thumb",  G_CALLBACK(cb_menu_zoomout), ivbox);
  gtk_box_pack_start(GTK_BOX(vbox), ivbox, FALSE, FALSE, 0);

  ivbox = gtk_vbox_new(FALSE, 0);
  icon_button(&appdata, "detailup_thumb",   G_CALLBACK(cb_menu_view_detail_inc), ivbox);
  icon_button(&appdata, "detaildown_thumb", G_CALLBACK(cb_menu_view_detail_dec), ivbox);
  gtk_box_pack_start(GTK_BOX(vbox), ivbox, TRUE, FALSE, 0);

  ivbox = gtk_vbox_new(FALSE, 0);
  GtkWidget *ok = icon_button(&appdata, "ok_thumb", O2G_NULLPTR, ivbox);
  GtkWidget *cancel = icon_button(&appdata, "cancel_thumb", O2G_NULLPTR, ivbox);
  iconbar_register_buttons(appdata, ok, cancel);
  gtk_box_pack_start(GTK_BOX(vbox), ivbox, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);
#endif // FREMANTLE

  gtk_box_pack_start(mainvbox, hbox, TRUE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(appdata.window), GTK_WIDGET(mainvbox));

  gtk_widget_show_all(appdata.window);

  appdata.presets = josm_presets_load();

  /* let gtk do its thing before loading the data, */
  /* so the user sees something */
  osm2go_platform::process_events();
  if(unlikely(!appdata.window)) {
    printf("shutdown while starting up (1)\n");
    return -1;
  }

  if(proj) {
    if(strcmp(proj, "-p") == 0) {
      cb_menu_project_open(&appdata);
    } else if(!project_load(appdata, proj)) {
      messagef(appdata.window, _("Command line arguments"),
               _("You passed '%s' on the command line, but it was neither"
                 "recognized as option nor could it be loaded as project."),
               proj);
    }
  }
  /* load project if one is specified in the settings */
  if(!appdata.project && !appdata.settings->project.empty())
    project_load(appdata, appdata.settings->project);

  appdata.map->set_autosave(true);
  appdata.main_ui_enable();

  /* start GPS if enabled by config */
  if(appdata.settings->enable_gps)
    track_enable_gps(appdata, TRUE);

  /* again let the ui do its thing */
  osm2go_platform::process_events();
  if(unlikely(!appdata.window)) {
    printf("shutdown while starting up (2)\n");
    return -1;
  }

  /* start to interact with the user now that the gui is running */
  if(unlikely(appdata.project && appdata.project->isDemo && appdata.settings->first_run_demo)) {
    messagef(appdata.window, _("Welcome to OSM2Go"),
	     _("This is the first time you run OSM2Go. "
	       "A demo project has been loaded to get you "
	       "started. You can play around with this demo as much "
	       "as you like. However, you cannot upload or download "
	       "the demo project.\n\n"
	       "In order to start working on real data you'll have "
	       "to setup a new project and enter your OSM user name "
	       "and password. You'll then be able to download the "
	       "latest data from OSM and upload your changes into "
	       "the OSM main database."
	       ));
  }

  puts("main up");

  /* ------------ jump into main loop ---------------- */
  gtk_main();

  puts("gtk_main() left");

  track_save(appdata.project, appdata.track.track);
  appdata.track_clear();

  /* save a diff if there are dirty entries */
  if(appdata.project && appdata.osm)
    diff_save(appdata.project, appdata.osm);

  return 0;
}

int main(int argc, char *argv[]) {
  // library init
  LIBXML_TEST_VERSION;

  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE, LOCALEDIR);
  bind_textdomain_codeset(PACKAGE, "UTF-8");
  textdomain(PACKAGE);

  /* Must initialize libcurl before any threads are started */
  curl_global_init(CURL_GLOBAL_ALL);

  /* Same for libxml2 */
  xmlInitParser();

  /* whitespace between tags has no meaning in any of the XML files used here */
  xmlKeepBlanksDefault(0);

#if !GLIB_CHECK_VERSION(2,32,0)
  g_thread_init(O2G_NULLPTR);
#endif

  gtk_init(&argc, &argv);
  int ret = osm2go_platform::init() ? 0 : 1;
  if (ret == 0) {
    ret = application_run(argc > 1 ? argv[1] : O2G_NULLPTR);

    osm2go_platform::cleanup();
  }

  // library cleanups
  xmlCleanupParser();
  curl_global_cleanup();

  return ret;
}