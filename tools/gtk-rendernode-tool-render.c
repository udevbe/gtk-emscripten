/*  Copyright 2023 Red Hat, Inc.
 *
 * GTK is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GTK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GTK; see the file COPYING.  If not,
 * see <http://www.gnu.org/licenses/>.
 *
 * Author: Matthias Clasen
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib/gi18n-lib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include "gtk-rendernode-tool.h"

static char *
get_save_filename (const char *filename)
{
  int length = strlen (filename);
  const char *extension = ".png";
  char *result;

  if (strcmp (filename + (length - strlen (".node")), ".node") == 0)
    {
      char *basename = g_strndup (filename, length - strlen (".node"));
      result = g_strconcat (basename, extension, NULL);
      g_free (basename);
    }
  else
    result = g_strconcat (filename, extension, NULL);

  return result;
}

static void
render_file (const char *filename,
             const char *renderer_name,
             const char *save_file,
             gboolean    force)
{
  GskRenderNode *node;
  GBytes *bytes;
  GdkTexture *texture;
  char *save_to;
  GskRenderer *renderer;
  GdkSurface *window;
  GError *error = NULL;

  node = load_node_file (filename);

  if (renderer_name)
    g_object_set_data_full (G_OBJECT (gdk_display_get_default ()), "gsk-renderer",
                            g_strdup (renderer_name), g_free);

  window = gdk_surface_new_toplevel (gdk_display_get_default ());
  renderer = gsk_renderer_new_for_surface (window);

  texture = gsk_renderer_render_texture (renderer, node, NULL);

  save_to = (char *)save_file;

  if (save_to == NULL)
    save_to = get_save_filename (filename);

  if (g_file_test (save_to, G_FILE_TEST_EXISTS) && !force)
    {
      g_printerr (_("File %s exists.\n"
                    "Use --force to overwrite.\n"), save_to);
      exit (1);
    }

  bytes = gdk_texture_save_to_png_bytes (texture);

  if (g_file_set_contents (save_to,
                           g_bytes_get_data (bytes, NULL),
                           g_bytes_get_size (bytes),
                           &error))
    {
      if (save_file == NULL)
        g_print (_("Output written to %s.\n"), save_to);
    }
  else
    {
      g_printerr (_("Failed to save %s: %s\n"), save_to, error->message);
      exit (1);
    }

  g_bytes_unref (bytes);

  if (save_to != save_file)
    g_free (save_to);

  g_object_unref (texture);
  gsk_render_node_unref (node);
}

void
do_render (int          *argc,
           const char ***argv)
{
  GOptionContext *context;
  char **filenames = NULL;
  gboolean force = FALSE;
  char *renderer = NULL;
  const GOptionEntry entries[] = {
    { "renderer", 0, 0, G_OPTION_ARG_STRING, &renderer, N_("Renderer to use"), N_("RENDERER") },
    { "force", 0, 0, G_OPTION_ARG_NONE, &force, N_("Overwrite existing file"), NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames, NULL, N_("FILE…") },
    { NULL, }
  };
  GError *error = NULL;

  if (gdk_display_get_default () == NULL)
    {
      g_printerr (_("Could not initialize windowing system\n"));
      exit (1);
    }

  g_set_prgname ("gtk4-rendernode-tool render");
  context = g_option_context_new (NULL);
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_summary (context, _("Render a .node file to an image."));

  if (!g_option_context_parse (context, argc, (char ***)argv, &error))
    {
      g_printerr ("%s\n", error->message);
      g_error_free (error);
      exit (1);
    }

  g_option_context_free (context);

  if (filenames == NULL)
    {
      g_printerr (_("No .node file specified\n"));
      exit (1);
    }

  if (g_strv_length (filenames) > 2)
    {
      g_printerr (_("Can only render a single .node file to a single output file\n"));
      exit (1);
    }

  render_file (filenames[0], renderer, filenames[1], force);

  g_strfreev (filenames);
}