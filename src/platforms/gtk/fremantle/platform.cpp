/*
 * SPDX-FileCopyrightText: 2008 Till Harbaum <till@harbaum.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <osm2go_platform.h>
#include <osm2go_platform_gtk.h>

#include "dbus.h"

#include <algorithm>
#include <hildon/hildon-check-button.h>
#include <hildon/hildon-entry.h>
#include <hildon/hildon-pannable-area.h>
#include <hildon/hildon-picker-button.h>
#include <hildon/hildon-picker-dialog.h>
#include <hildon/hildon-touch-selector-entry.h>
#include <libosso.h>
#include <tablet-browser-interface.h>

#include <osm2go_annotations.h>
#include <osm2go_cpp.h>

static osso_context_t *osso_context;

bool osm2go_platform::init(bool &startGps)
{
  g_signal_new("changed", HILDON_TYPE_PICKER_BUTTON,
               G_SIGNAL_RUN_FIRST, 0, nullptr, nullptr,
               g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  osso_context = osso_initialize("org.harbaum." PACKAGE, VERSION, TRUE, nullptr);

  if(G_UNLIKELY(osso_context == nullptr))
    return false;

  startGps = true;

  if(G_UNLIKELY(dbus_register(osso_context) != TRUE)) {
    osso_deinitialize(osso_context);
    return false;
  } else {
    return true;
  }
}

void osm2go_platform::cleanup()
{
  osso_deinitialize(osso_context);
}

void osm2go_platform::open_url(const char* url)
{
  osso_rpc_run_with_defaults(osso_context, "osso_browser",
                             OSSO_BROWSER_OPEN_NEW_WINDOW_REQ, nullptr,
                             DBUS_TYPE_STRING, url,
                             DBUS_TYPE_BOOLEAN, FALSE, DBUS_TYPE_INVALID);
}

GtkWidget *osm2go_platform::notebook_new(void) {
  GtkWidget *vbox = gtk_vbox_new(FALSE, 0);

  GtkWidget *notebook =  gtk_notebook_new();

  /* solution for fremantle: we use a row of ordinary buttons instead */
  /* of regular tabs */

  /* hide the regular tabs */
  gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);

  gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

  /* store a reference to the notebook in the vbox */
  g_object_set_data(G_OBJECT(vbox), "notebook", notebook);

  /* create a hbox for the buttons */
  GtkWidget *hbox = gtk_hbox_new(TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
  g_object_set_data(G_OBJECT(vbox), "hbox", hbox);

  return vbox;
}

GtkNotebook *osm2go_platform::notebook_get_gtk_notebook(GtkWidget *notebook) {
  return GTK_NOTEBOOK(g_object_get_data(G_OBJECT(notebook), "notebook"));
}

static void on_notebook_button_clicked(GtkWidget *button, gpointer data) {
  GtkNotebook *nb = GTK_NOTEBOOK(g_object_get_data(G_OBJECT(data), "notebook"));

  gint page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "page"));
  gtk_notebook_set_current_page(nb, page);
}

void
osm2go_platform::notebook_append_page(GtkWidget *notebook, GtkWidget *page, trstring::native_type_arg label)
{
  GtkNotebook *nb = notebook_get_gtk_notebook(notebook);
  gint page_num = gtk_notebook_append_page(nb, page, gtk_label_new(label));

  GtkWidget *button;

  /* select button for page 0 by default */
  if(!page_num) {
    button = gtk_radio_button_new_with_label(nullptr, static_cast<const gchar *>(label));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    g_object_set_data(G_OBJECT(notebook), "group_master", button);
  } else {
    gpointer master = g_object_get_data(G_OBJECT(notebook), "group_master");
    button = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(master), static_cast<const gchar *>(label));
  }

  gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE);
  g_object_set_data(G_OBJECT(button), "page", GINT_TO_POINTER(page_num));

  g_signal_connect(button, "clicked", G_CALLBACK(on_notebook_button_clicked), notebook);

  hildon_gtk_widget_set_theme_size(button,
                                   static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH));

  gtk_box_pack_start(GTK_BOX(g_object_get_data(G_OBJECT(notebook), "hbox")), button, TRUE, TRUE, 0);
}

GtkTreeView *osm2go_platform::tree_view_new()
{
  return GTK_TREE_VIEW(hildon_gtk_tree_view_new(HILDON_UI_MODE_EDIT));
}

GtkWidget *osm2go_platform::scrollable_container(GtkWidget *view, bool)
{
  /* put view into a pannable area */
  GtkWidget *container = hildon_pannable_area_new();
  gtk_container_add(GTK_CONTAINER(container), view);
  return container;
}

GtkWidget *osm2go_platform::entry_new(osm2go_platform::EntryFlags flags) {
  GtkWidget *ret = hildon_entry_new(HILDON_SIZE_AUTO);
  if(flags & EntryFlagsNoAutoCap)
    hildon_gtk_entry_set_input_mode(GTK_ENTRY(ret), HILDON_GTK_INPUT_MODE_FULL);
  return ret;
}

bool osm2go_platform::isEntryWidget(GtkWidget *widget)
{
  return HILDON_IS_ENTRY(widget) == TRUE;
}

GtkWidget *osm2go_platform::button_new_with_label(trstring::arg_type label)
{
  GtkWidget *button = gtk_button_new_with_label(static_cast<const gchar *>(static_cast<trstring::native_type>(label)));
  hildon_gtk_widget_set_theme_size(button,
           static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT | HILDON_SIZE_AUTO_WIDTH));
  return button;
}

GtkWidget *osm2go_platform::check_button_new_with_label(const char *label) {
  GtkWidget *cbut =
    hildon_check_button_new(static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT |
                                                        HILDON_SIZE_AUTO_WIDTH));
  gtk_button_set_label(GTK_BUTTON(cbut), label);
  return cbut;
}

bool osm2go_platform::isCheckButtonWidget(GtkWidget *widget)
{
  return HILDON_IS_CHECK_BUTTON(widget) == TRUE;
}

void osm2go_platform::check_button_set_active(GtkWidget *button, bool active) {
  gboolean state = active ? TRUE : FALSE;
  hildon_check_button_set_active(HILDON_CHECK_BUTTON(button), state);
}

bool osm2go_platform::check_button_get_active(GtkWidget *button) {
  return hildon_check_button_get_active(HILDON_CHECK_BUTTON(button)) == TRUE;
}

static void on_value_changed(HildonPickerButton *widget) {
  g_signal_emit_by_name(widget, "changed");
}

static GtkWidget *picker_button(const gchar *title, GtkWidget *selector)
{
  GtkWidget *button =
    hildon_picker_button_new(static_cast<HildonSizeType>(HILDON_SIZE_FINGER_HEIGHT |
                                                         HILDON_SIZE_AUTO_WIDTH),
			     HILDON_BUTTON_ARRANGEMENT_VERTICAL);

  hildon_button_set_title_alignment(HILDON_BUTTON(button), 0.5, 0.5);
  hildon_button_set_value_alignment(HILDON_BUTTON(button), 0.5, 0.5);

  /* allow button to emit "changed" signal */
  g_signal_connect(button, "value-changed", G_CALLBACK(on_value_changed), nullptr);

  hildon_button_set_title(HILDON_BUTTON (button), title);

  hildon_picker_button_set_selector(HILDON_PICKER_BUTTON(button),
				    HILDON_TOUCH_SELECTOR(selector));

  return button;
}

struct combo_add_string {
  HildonTouchSelector * const selector;
  explicit combo_add_string(HildonTouchSelector *sel) : selector(sel) {}
  inline void operator()(trstring::native_type_arg entry) {
    hildon_touch_selector_append_text(selector, static_cast<const gchar *>(entry));
  }
};

GtkWidget *osm2go_platform::combo_box_new(trstring::native_type_arg title, const std::vector<trstring::native_type> &items, int active)
{
  GtkWidget *selector = hildon_touch_selector_new_text();
  GtkWidget *cbox = picker_button(static_cast<const gchar *>(title), selector);

  /* fill combo box with entries */
  std::for_each(items.begin(), items.end(), combo_add_string(HILDON_TOUCH_SELECTOR(selector)));

  if(active >= 0)
    osm2go_platform::combo_box_set_active(cbox, active);

  return cbox;
}

/**
 * @brief extract current value of hildon_touch_selector_entry
 *
 * In contrast to the default hildon_touch_selector_entry_print_func() it will
 * just return whatever is in the edit field, so that one can clear that field
 * resulting in no value being set.
 */
static gchar *
touch_selector_entry_print_func(HildonTouchSelector *selector, gpointer)
{
  HildonEntry *entry = hildon_touch_selector_entry_get_entry(HILDON_TOUCH_SELECTOR_ENTRY(selector));

  return g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
}

GtkWidget *osm2go_platform::combo_box_entry_new(const char *title) {
  GtkWidget *selector = hildon_touch_selector_entry_new_text();
  hildon_touch_selector_set_print_func(HILDON_TOUCH_SELECTOR(selector), touch_selector_entry_print_func);
  return picker_button(title, selector);
}

void osm2go_platform::combo_box_append_text(GtkWidget *cbox, const char *text) {
  HildonTouchSelector *selector = hildon_picker_button_get_selector(HILDON_PICKER_BUTTON(cbox));

  hildon_touch_selector_append_text(selector, text);
}

void osm2go_platform::combo_box_set_active(GtkWidget *cbox, int index) {
  hildon_picker_button_set_active(HILDON_PICKER_BUTTON(cbox), index);
}

int osm2go_platform::combo_box_get_active(GtkWidget *cbox) {
  return hildon_picker_button_get_active(HILDON_PICKER_BUTTON(cbox));
}

std::string osm2go_platform::combo_box_get_active_text(GtkWidget *cbox) {
  return hildon_button_get_value(HILDON_BUTTON(cbox));
}

void osm2go_platform::combo_box_set_active_text(GtkWidget *cbox, const char *text)
{
  hildon_button_set_value(HILDON_BUTTON(cbox), text);
  // explicitely set the text in the edit, which will not happen when setting the button
  // text to something not in the model
  HildonTouchSelector *selector = hildon_picker_button_get_selector(HILDON_PICKER_BUTTON(cbox));
  HildonEntry *entry = hildon_touch_selector_entry_get_entry(HILDON_TOUCH_SELECTOR_ENTRY(selector));
  gtk_entry_set_text(GTK_ENTRY(entry), text);
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

bool osm2go_platform::isComboBoxWidget(GtkWidget *widget)
{
  return HILDON_IS_PICKER_BUTTON(widget) == TRUE;
}

bool osm2go_platform::isComboBoxEntryWidget(GtkWidget *widget)
{
  return HILDON_IS_PICKER_BUTTON(widget) == TRUE;
}

namespace {

struct g_list_deleter {
  void operator()(GList *list) {
    g_list_foreach(list, reinterpret_cast<GFunc>(gtk_tree_path_free), nullptr);
    g_list_free(list);
  }
};

typedef std::unique_ptr<GList, g_list_deleter> GListGuard;

}

static std::string
select_print_func_str(HildonTouchSelector *selector, gpointer data)
{
  GListGuard selected_rows(hildon_touch_selector_get_selected_rows(selector, 0));
  const char delimiter = reinterpret_cast<intptr_t>(data);

  std::string result;
  if(!selected_rows)
    return result;

  GtkTreeModel *model = hildon_touch_selector_get_model(selector, 0);

  g_string guard;

  for (GList *item = selected_rows.get(); item != nullptr; item = g_list_next(item)) {
    GtkTreeIter iter;
    gtk_tree_model_get_iter(model, &iter, static_cast<GtkTreePath *>(item->data));

    gchar *current_string = nullptr;
    gtk_tree_model_get(model, &iter, 1, &current_string, -1);
    guard.reset(current_string);

    result += current_string;
    result += delimiter;
  }

  result.resize(result.size() - 1);

  return result;
}

/**
 * @brief custom print function for multiselects
 * @param selector the selector to monitor
 * @param data ASCII value of the delimiter casted to a pointer
 */
static gchar *
select_print_func(HildonTouchSelector *selector, gpointer data)
{
  return g_strdup(select_print_func_str(selector, data).c_str());
}

GtkWidget *osm2go_platform::select_widget(GtkTreeModel *model, unsigned int flags, char delimiter)
{
  HildonTouchSelector *selector;

  switch (flags) {
  case NoSelectionFlags:
    selector = HILDON_TOUCH_SELECTOR(hildon_touch_selector_new_text());
    break;
  case AllowEditing:
    selector = HILDON_TOUCH_SELECTOR(hildon_touch_selector_entry_new_text());
    hildon_touch_selector_set_print_func(selector, touch_selector_entry_print_func);
    hildon_touch_selector_entry_set_text_column(HILDON_TOUCH_SELECTOR_ENTRY(selector), 1);
    break;
  case AllowMultiSelection: {
    selector = HILDON_TOUCH_SELECTOR(hildon_touch_selector_new_text());
    intptr_t ch = delimiter;
    hildon_touch_selector_set_print_func_full(selector, select_print_func, reinterpret_cast<gpointer>(ch), nullptr);
    hildon_touch_selector_set_column_selection_mode(selector, HILDON_TOUCH_SELECTOR_SELECTION_MODE_MULTIPLE);
    g_object_set_data(G_OBJECT(selector), "user delimiter", reinterpret_cast<gpointer>(ch));
    break;
  }
  default:
    assert_unreachable();
  }

  hildon_touch_selector_set_model(selector, 0, model);

  return GTK_WIDGET(selector);
}

/**
 * @brief save the list of the initially selected indexes to the selector
 *
 * This list is used when the dialog is cancelled and reopened to show the initial
 * selection again. One could think that one could dive into HildonPickerDialogPrivate
 * and use the current_selection member for that, but this always seems to hold the
 * current selection, i.e. the one that was just cancelled.
 */
static void
set_index_list(HildonTouchSelector *selector, const std::vector<unsigned int> &indexes)
{
  if(indexes.empty()) {
    g_object_set_data(G_OBJECT(selector), "selected indexes", nullptr);
  } else {
    // save the index list for reuse when the dialog is cancelled
    unsigned int *idxlist = static_cast<unsigned int *>(calloc(indexes.size() + 1, sizeof(*idxlist)));
    if (G_UNLIKELY(idxlist == nullptr))
      return;
    memcpy(idxlist, indexes.data(), sizeof(*idxlist) * indexes.size());
    // terminate list
    idxlist[indexes.size()] = static_cast<unsigned int>(-1);
    g_object_set_data_full(G_OBJECT(selector), "selected indexes", idxlist, free);
  }
}

/**
 * @brief handle "done" button in multiselect dialogs
 *
 * This updates the saved index list to the new accepted state.
 */
static void
multiselect_response(GtkDialog *dialog, gint response_id)
{
  if(response_id != GTK_RESPONSE_OK)
    return;

  HildonTouchSelector *selector = hildon_picker_dialog_get_selector(HILDON_PICKER_DIALOG(dialog));
  g_assert(selector != nullptr);
  GListGuard selected_rows(hildon_touch_selector_get_selected_rows(selector, 0));

  GtkTreeModel *model = hildon_touch_selector_get_model(selector, 0);

  std::vector<unsigned int> indexes(g_list_length(selected_rows.get()), -1);
  indexes.clear();

  for (GList *item = selected_rows.get(); item != nullptr; item = g_list_next(item)) {
    GtkTreeIter iter;
    GtkTreePath *path = static_cast<GtkTreePath *>(item->data);
    gtk_tree_model_get_iter(model, &iter, path);
    g_assert_cmpint(gtk_tree_path_get_depth(path), ==, 1);
    const gint *idx = gtk_tree_path_get_indices(path);
    g_assert(idx != nullptr);

    indexes.push_back(*idx);
  }

  set_index_list(selector, indexes);
}

// taken from libhildon, but using the correct type for selector to avoid needless casts
typedef struct _HildonPickerButtonPrivate {
  HildonTouchSelector *selector;
  GtkWidget *dialog;
  gchar *done_button_text;
  guint disable_value_changed : 1;
} HildonPickerButtonPrivate;

/**
 * @brief change the signal handlers for the "Done" button of the Hildon picker button
 */
static void
multiselect_button_callback(GtkWidget *widget)
{
  // get the internal dialog created in the default "clicked" handler of the picker button
  _HildonPickerButtonPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE(widget, HILDON_TYPE_PICKER_BUTTON, HildonPickerButtonPrivate);
  // disconnect all signals with nullptr as data
  // These are:
  // -the "delete-event" callback restored later
  // -one unknown connection (TODO)
  // -the default "response" handler that prohibits empty selections in the multiselect
  //  This is the actual target of this operation.
  g_signal_handlers_disconnect_matched(priv->dialog, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, nullptr);
  // connections must use nullptr as argument again so they will not sum up if the button is clicked multiple times
  g_signal_connect(priv->dialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
  g_signal_connect(priv->dialog, "response", G_CALLBACK(multiselect_response), nullptr);

  // now restore the selection to what it initially was
  // otherwise one may open the dialog, change the selection, cancel,
  // open it again and still find the selection from last time
  const unsigned int *idxlist = static_cast<unsigned int *>(g_object_get_data(G_OBJECT(priv->selector), "selected indexes"));
  hildon_touch_selector_unselect_all(priv->selector, 0);
  if(idxlist != nullptr) {
    GtkTreeModel *model = hildon_touch_selector_get_model(priv->selector, 0);
    for(size_t i = 0; idxlist[i] != static_cast<unsigned int>(-1); i++) {
      GtkTreeIter iter;
      gboolean b = gtk_tree_model_iter_nth_child(model, &iter, nullptr, idxlist[i]);
      g_assert(b == TRUE);
      hildon_touch_selector_select_iter(priv->selector, 0, &iter, FALSE);
    }
  }
}

GtkWidget *osm2go_platform::select_widget_wrapped(const char *title, GtkTreeModel *model, unsigned int flags, char delimiter)
{
  GtkWidget *ret = picker_button(title, select_widget(model, flags, delimiter));
  if(flags & AllowMultiSelection)
    g_signal_connect_after(ret, "clicked", G_CALLBACK(multiselect_button_callback), nullptr);
  return ret;
}

std::string osm2go_platform::select_widget_value(GtkWidget *widget)
{
  // this may be called both for a wrapped and bare widget
  HildonTouchSelector *selector;
  if(HILDON_IS_TOUCH_SELECTOR(widget))
    selector = HILDON_TOUCH_SELECTOR(widget);
  else
    selector = hildon_picker_button_get_selector(HILDON_PICKER_BUTTON(widget));
  g_assert(selector != nullptr);

  if(HILDON_IS_TOUCH_SELECTOR_ENTRY(selector)) {
    HildonEntry *entry = hildon_touch_selector_entry_get_entry(HILDON_TOUCH_SELECTOR_ENTRY(selector));

    return gtk_entry_get_text(GTK_ENTRY(entry));
  } else {
    GtkTreeModel *model = hildon_touch_selector_get_model(selector, 0);
    std::string ret;

    if(hildon_touch_selector_get_column_selection_mode(selector) ==
       HILDON_TOUCH_SELECTOR_SELECTION_MODE_MULTIPLE) {
      // the fastest way to repeat the check if this is a wrapped widget or not
      if(reinterpret_cast<GtkWidget *>(selector) == widget) {
        gpointer p = g_object_get_data(G_OBJECT(selector), "user delimiter");
        ret = select_print_func_str(selector, p);
      } else
      // the button has already the properly formatted result
        ret = hildon_button_get_value(HILDON_BUTTON(widget));
    } else {
      int row = hildon_touch_selector_get_active(selector, 0);
      GtkTreeIter iter;
      gboolean b = gtk_tree_model_iter_nth_child(model, &iter, nullptr, row);
      g_assert(b == TRUE);
      gchar *s;
      gtk_tree_model_get(model, &iter, 1, &s, -1);
      g_string guard(s);
      ret = s;
    }

    return ret;
  }
}

bool osm2go_platform::select_widget_has_selection(GtkWidget *widget)
{
  g_assert(HILDON_IS_TOUCH_SELECTOR(widget));
  GListGuard sel(hildon_touch_selector_get_selected_rows(HILDON_TOUCH_SELECTOR(widget), 0));
  return static_cast<bool>(sel);
}

void osm2go_platform::select_widget_select(GtkWidget *widget, const std::vector<unsigned int> &indexes)
{
  HildonTouchSelector *selector = hildon_picker_button_get_selector(HILDON_PICKER_BUTTON(widget));

  if(HILDON_IS_TOUCH_SELECTOR_ENTRY(selector) ||
     hildon_touch_selector_get_column_selection_mode(selector) !=
         HILDON_TOUCH_SELECTOR_SELECTION_MODE_MULTIPLE) {
    assert_cmpnum(indexes.size(), 1);
    hildon_picker_button_set_active(HILDON_PICKER_BUTTON(widget), indexes.front());
  } else {
    GtkTreeModel *model = hildon_touch_selector_get_model(selector, 0);

    for(size_t i = 0; i < indexes.size(); i++) {
      GtkTreeIter iter;
      gboolean b = gtk_tree_model_iter_nth_child(model, &iter, nullptr, indexes[i]);
      g_assert(b == TRUE);
      hildon_touch_selector_select_iter(selector, 0, &iter, FALSE);
    }

    set_index_list(selector, indexes);
  }
}

void osm2go_platform::setEntryText(GtkEntry *entry, const char *text, trstring::native_type_arg placeholder)
{
  hildon_gtk_entry_set_placeholder_text(entry, static_cast<const gchar *>(placeholder));
  gtk_entry_set_text(entry, text);
}
