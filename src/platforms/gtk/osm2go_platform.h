/*
 * Copyright (C) 2017 Rolf Eike Beer <eike@sf-mail.de>.
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

#ifndef OSM2GO_PLATFORM_H
#define OSM2GO_PLATFORM_H

#include <glib.h>
#include <memory>

#include <osm2go_cpp.h>
#include <osm2go_stl.h>

class color_t;
typedef struct _GtkWidget GtkWidget;

namespace osm2go_platform {
  typedef GtkWidget Widget;

  struct gtk_widget_deleter {
    void operator()(GtkWidget *mem);
  };
  typedef std::unique_ptr<GtkWidget, gtk_widget_deleter> WidgetGuard;

  /**
   * @brief process all pending GUI events
   */
  void process_events();

  /**
   * @brief simple interface to the systems web browser
   */
  void open_url(const char *url);

  class Timer {
    guint id;
  public:
    explicit inline Timer()
      : id(0) {}
    inline ~Timer()
    { stop(); }

    void restart(unsigned int seconds, GSourceFunc callback, void *data);
    void stop();

    inline bool isActive() const
    { return id != 0; }
  };

  class MappedFile {
    GMappedFile *map;
  public:
    explicit inline MappedFile(const char *fname)
      : map(g_mapped_file_new(fname, FALSE, nullptr)) {}
    inline ~MappedFile()
    { reset(); }

    inline operator bool() const
    { return map != nullptr; }

    inline const char *data()
    { return g_mapped_file_get_contents(map); }

    inline size_t length()
    { return g_mapped_file_get_length(map); }

    void reset();
  };

  /**
   * @brief parses a string representation of a color value using
   * @param str the string to parse
   * @param color the color object to update
   * @returns if the given string is a valid color
   *
   * The string is expected to begin with a '#'.
   */
  bool parse_color_string(const char *str, color_t &color) __attribute__((nonnull(1)));

  /**
   * @brief converts a character string to a double in local-unaware fashion
   * @param str the string to parse
   * @returns the parsed value or NAN if str == nullptr
   */
  double string_to_double(const char *str);
};

#endif // OSM2GO_PLATFORM_H
