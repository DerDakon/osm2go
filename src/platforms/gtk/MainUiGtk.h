/*
 * Copyright (C) 2017,2018 Rolf Eike Beer <eike@sf-mail.de>.
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

#ifndef MAINUIGTK_H
#define MAINUIGTK_H

#include <uicontrol.h>

#include <array>

typedef struct _GtkWidget GtkWidget;

class MainUiGtk : public MainUi {
  std::array<GtkWidget *, MainUi::MENU_ITEMS_COUNT> menuitems;

public:
  MainUiGtk(appdata_t &a);

  /*
   * @brief init the widgets
   */
  void initItem(menu_items item, GtkWidget *widget);

  inline GtkWidget *menu_item(menu_items item)
  { return menuitems[item]; }
};

#endif /* MAINUIGTK_H */
