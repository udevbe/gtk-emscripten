/* GDK - The GIMP Drawing Kit
 * Copyright (C) 2002,2005 Hans Breuer
 * Copyright (C) 2003 Tor Lillqvist
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#define VK_USE_PLATFORM_WIN32_KHR

#include "gdk.h"
#include "gdkprivate-win32.h"
#include "gdkcairocontext-win32.h"
#include "gdkclipboardprivate.h"
#include "gdkclipboard-win32.h"
#include "gdkdisplay-win32.h"
#include "gdkdevicemanager-win32.h"
#include "gdkglcontext-win32.h"
#include "gdksurface-win32.h"
#include "gdkwin32display.h"
#include "gdkwin32screen.h"
#include "gdkwin32surface.h"
#include "gdkmonitor-win32.h"
#include "gdkwin32.h"
#include "gdkvulkancontext-win32.h"

#include <dwmapi.h>
#include <pango/pangowin32.h>

#ifdef HAVE_EGL
# include <epoxy/egl.h>
#endif

#ifndef IMAGE_FILE_MACHINE_ARM64
# define IMAGE_FILE_MACHINE_ARM64 0xAA64
#endif

static int debug_indent = 0;

/**
 * gdk_win32_display_add_filter:
 * @display: a `GdkWin32Display`
 * @function: (scope notified): filter callback
 * @data: (closure): data to pass to filter callback
 *
 * Adds an event filter to @window, allowing you to intercept messages
 * before they reach GDK. This is a low-level operation and makes it
 * easy to break GDK and/or GTK, so you have to know what you're
 * doing.
 **/
void
gdk_win32_display_add_filter (GdkWin32Display           *display,
                              GdkWin32MessageFilterFunc  function,
                              gpointer                   data)
{
  GList *tmp_list;
  GdkWin32MessageFilter *filter;

  g_return_if_fail (GDK_IS_WIN32_DISPLAY (display));

  tmp_list = display->filters;

  for (tmp_list = display->filters; tmp_list; tmp_list = tmp_list->next)
    {
      filter = (GdkWin32MessageFilter *) tmp_list->data;

      if ((filter->function == function) && (filter->data == data))
        {
          filter->ref_count++;
          return;
        }
    }

  filter = g_new (GdkWin32MessageFilter, 1);
  filter->function = function;
  filter->data = data;
  filter->ref_count = 1;
  filter->removed = FALSE;

  display->filters = g_list_append (display->filters, filter);
}

/**
 * _gdk_win32_message_filter_unref:
 * @display: A `GdkWin32Display`
 * @filter: A message filter
 *
 * Release a reference to @filter.  Note this function may
 * mutate the list storage, so you need to handle this
 * if iterating over a list of filters.
 */
void
_gdk_win32_message_filter_unref (GdkWin32Display       *display,
			         GdkWin32MessageFilter *filter)
{
  GList **filters;
  GList *tmp_list;

  filters = &display->filters;

  tmp_list = *filters;
  while (tmp_list)
    {
      GdkWin32MessageFilter *iter_filter = tmp_list->data;
      GList *node;

      node = tmp_list;
      tmp_list = tmp_list->next;

      if (iter_filter != filter)
	continue;

      g_assert (iter_filter->ref_count > 0);

      filter->ref_count--;
      if (filter->ref_count != 0)
	continue;

      *filters = g_list_remove_link (*filters, node);
      g_free (filter);
      g_list_free_1 (node);
    }
}

/**
 * gdk_win32_display_remove_filter:
 * @display: A `GdkWin32Display`
 * @function: (scope notified): previously-added filter function
 * @data: (closure): user data for previously-added filter function
 *
 * Remove a filter previously added with gdk_win32_display_add_filter().
 */
void
gdk_win32_display_remove_filter (GdkWin32Display           *display,
                                 GdkWin32MessageFilterFunc  function,
                                 gpointer                   data)
{
  GList *tmp_list;
  GdkWin32MessageFilter *filter;

  g_return_if_fail (GDK_IS_WIN32_DISPLAY (display));

  tmp_list = display->filters;

  while (tmp_list)
    {
      filter = (GdkWin32MessageFilter *) tmp_list->data;
      tmp_list = tmp_list->next;

      if ((filter->function == function) && (filter->data == data))
        {
          filter->removed = TRUE;

          _gdk_win32_message_filter_unref (display, filter);

          return;
        }
    }
}

static GdkMonitor *
_gdk_win32_display_find_matching_monitor (GdkWin32Display *win32_display,
                                          GdkMonitor      *needle)
{
  int i;

  for (i = 0; i < g_list_model_get_n_items (win32_display->monitors); i++)
    {
      GdkWin32Monitor *m;

      m = g_list_model_get_item (win32_display->monitors, i);
      g_object_unref (m);

      if (_gdk_win32_monitor_compare (m, GDK_WIN32_MONITOR (needle)) == 0)
        return GDK_MONITOR (m);
    }

  return NULL;
}

void
_gdk_win32_display_init_monitors (GdkWin32Display *win32_display)
{
  GPtrArray *new_monitors;
  int i;
  GdkWin32Monitor *primary_to_move = NULL;

  for (i = 0; i < g_list_model_get_n_items (win32_display->monitors); i++)
    {
      GdkWin32Monitor *m;

      m = g_list_model_get_item (win32_display->monitors, i);
      m->remove = TRUE;
      g_object_unref (m);
    }

  new_monitors = _gdk_win32_display_get_monitor_list (win32_display);

  for (i = 0; i < new_monitors->len; i++)
    {
      GdkWin32Monitor *w32_m;
      GdkMonitor *m;
      GdkWin32Monitor *w32_ex_monitor;
      GdkMonitor *ex_monitor;
      GdkRectangle geometry;
      GdkRectangle workarea, ex_workarea;

      w32_m = GDK_WIN32_MONITOR (g_ptr_array_index (new_monitors, i));
      m = GDK_MONITOR (w32_m);
      ex_monitor = _gdk_win32_display_find_matching_monitor (win32_display, m);
      w32_ex_monitor = GDK_WIN32_MONITOR (ex_monitor);

      if (ex_monitor == NULL)
        {
          w32_m->add = TRUE;
          continue;
        }

      w32_ex_monitor->remove = FALSE;

      if (i == 0)
        primary_to_move = w32_ex_monitor;

      gdk_monitor_get_geometry (m, &geometry);
      gdk_win32_monitor_get_workarea (m, &workarea);
      gdk_win32_monitor_get_workarea (ex_monitor, &ex_workarea);

      if (memcmp (&workarea, &ex_workarea, sizeof (GdkRectangle)) != 0)
        {
          w32_ex_monitor->work_rect = workarea;
        }

      gdk_monitor_set_geometry (ex_monitor, &geometry);

      if (gdk_monitor_get_width_mm (m) != gdk_monitor_get_width_mm (ex_monitor) ||
          gdk_monitor_get_height_mm (m) != gdk_monitor_get_height_mm (ex_monitor))
        {
          gdk_monitor_set_physical_size (ex_monitor,
                                         gdk_monitor_get_width_mm (m),
                                         gdk_monitor_get_height_mm (m));
        }

      if (g_strcmp0 (gdk_monitor_get_model (m), gdk_monitor_get_model (ex_monitor)) != 0)
        {
          gdk_monitor_set_model (ex_monitor,
                                 gdk_monitor_get_model (m));
        }

      if (g_strcmp0 (gdk_monitor_get_manufacturer (m), gdk_monitor_get_manufacturer (ex_monitor)) != 0)
        {
          gdk_monitor_set_manufacturer (ex_monitor,
                                        gdk_monitor_get_manufacturer (m));
        }

      if (gdk_monitor_get_refresh_rate (m) != gdk_monitor_get_refresh_rate (ex_monitor))
        {
          gdk_monitor_set_refresh_rate (ex_monitor, gdk_monitor_get_refresh_rate (m));
        }

      if (gdk_monitor_get_scale_factor (m) != gdk_monitor_get_scale_factor (ex_monitor))
        {
          gdk_monitor_set_scale_factor (ex_monitor, gdk_monitor_get_scale_factor (m));
        }

      if (gdk_monitor_get_subpixel_layout (m) != gdk_monitor_get_subpixel_layout (ex_monitor))
        {
          gdk_monitor_set_subpixel_layout (ex_monitor, gdk_monitor_get_subpixel_layout (m));
        }
    }

  for (i = g_list_model_get_n_items (win32_display->monitors) - 1; i >= 0; i--)
    {
      GdkWin32Monitor *w32_ex_monitor;
      GdkMonitor *ex_monitor;

      w32_ex_monitor = GDK_WIN32_MONITOR (g_list_model_get_item (win32_display->monitors, i));
      ex_monitor = GDK_MONITOR (w32_ex_monitor);

      if (w32_ex_monitor->remove)
        {
          w32_ex_monitor->hmonitor = NULL;
          g_list_store_remove (G_LIST_STORE (win32_display->monitors), i);
          gdk_monitor_invalidate (ex_monitor);
        }

      g_object_unref (w32_ex_monitor);
    }

  for (i = 0; i < new_monitors->len; i++)
    {
      GdkWin32Monitor *w32_m;

      w32_m = GDK_WIN32_MONITOR (g_ptr_array_index (new_monitors, i));

      if (!w32_m->add)
        continue;

      if (i == 0)
        g_list_store_insert (G_LIST_STORE (win32_display->monitors), 0, w32_m);
      else
        g_list_store_append (G_LIST_STORE (win32_display->monitors), w32_m);
    }

  g_ptr_array_free (new_monitors, TRUE);

  if (primary_to_move)
    {
      guint pos;
      g_object_ref (primary_to_move);
      if (g_list_store_find (G_LIST_STORE (win32_display->monitors), primary_to_move, &pos))
        g_list_store_remove (G_LIST_STORE (win32_display->monitors), pos);
      g_list_store_insert (G_LIST_STORE (win32_display->monitors), 0, primary_to_move);
      g_object_unref (primary_to_move);
    }
}


/**
 * gdk_win32_display_set_cursor_theme:
 * @display: (type GdkWin32Display): a `GdkDisplay`
 * @name: (nullable): the name of the cursor theme to use, or %NULL
 *   to unset a previously set value
 * @size: the cursor size to use, or 0 to keep the previous size
 *
 * Sets the cursor theme from which the images for cursor
 * should be taken.
 *
 * If the windowing system supports it, existing cursors created
 * with [ctor@Gdk.Cursor.new_from_name] are updated to reflect the theme
 * change. Custom cursors constructed with [ctor@Gdk.Cursor.new_from_texture]
 * will have to be handled by the application (GTK applications can
 * learn about cursor theme changes by listening for change notification
 * for the corresponding `GtkSetting`).
 */
void
gdk_win32_display_set_cursor_theme (GdkDisplay  *display,
                                    const char *name,
                                    int          size)
{
  int cursor_size;
  int w, h;
  Win32CursorTheme *theme;
  GdkWin32Display *win32_display = GDK_WIN32_DISPLAY (display);

  g_assert (win32_display);

  if (name == NULL)
    name = "system";

  w = GetSystemMetrics (SM_CXCURSOR);
  h = GetSystemMetrics (SM_CYCURSOR);

  /* We can load cursors of any size, but SetCursor() will scale them back
   * to this value. It's possible to break that restrictions with SetSystemCursor(),
   * but that will override cursors for the whole desktop session.
   */
  cursor_size = (w == h) ? w : size;

  if (win32_display->cursor_theme_name != NULL &&
      g_strcmp0 (name, win32_display->cursor_theme_name) == 0 &&
      win32_display->cursor_theme_size == cursor_size)
    return;

  theme = win32_cursor_theme_load (name, cursor_size);
  if (theme == NULL)
    {
      g_warning ("Failed to load cursor theme %s", name);
      return;
    }

  if (win32_display->cursor_theme)
    {
      win32_cursor_theme_destroy (win32_display->cursor_theme);
      win32_display->cursor_theme = NULL;
    }

  win32_display->cursor_theme = theme;
  g_free (win32_display->cursor_theme_name);
  win32_display->cursor_theme_name = g_strdup (name);
  win32_display->cursor_theme_size = cursor_size;

  _gdk_win32_display_update_cursors (win32_display);
}

Win32CursorTheme *
_gdk_win32_display_get_cursor_theme (GdkWin32Display *win32_display)
{
  Win32CursorTheme *theme;

  g_assert (win32_display->cursor_theme_name);

  theme = win32_display->cursor_theme;
  if (!theme)
    {
      theme = win32_cursor_theme_load (win32_display->cursor_theme_name,
                                       win32_display->cursor_theme_size);
      if (theme == NULL)
        {
          g_warning ("Failed to load cursor theme %s",
                     win32_display->cursor_theme_name);
          return NULL;
        }
      win32_display->cursor_theme = theme;
    }

  return theme;
}

static gulong
gdk_win32_display_get_next_serial (GdkDisplay *display)
{
	return 0;
}

static LRESULT CALLBACK
inner_display_change_hwnd_procedure (HWND   hwnd,
                                       UINT   message,
                                       WPARAM wparam,
                                       LPARAM lparam)
{
  switch (message)
    {
    case WM_DESTROY:
      {
        PostQuitMessage (0);
        return 0;
      }
    case WM_DISPLAYCHANGE:
      {
        GdkWin32Display *win32_display = GDK_WIN32_DISPLAY (_gdk_display);

        _gdk_win32_screen_on_displaychange_event (GDK_WIN32_SCREEN (win32_display->screen));
        return 0;
      }
    default:
      /* Otherwise call DefWindowProcW(). */
      GDK_NOTE (EVENTS, g_print (" DefWindowProcW"));
      return DefWindowProc (hwnd, message, wparam, lparam);
    }
}

static LRESULT CALLBACK
display_change_hwnd_procedure (HWND   hwnd,
                                 UINT   message,
                                 WPARAM wparam,
                                 LPARAM lparam)
{
  LRESULT retval;

  GDK_NOTE (EVENTS, g_print ("%s%*s%s %p",
			     (debug_indent > 0 ? "\n" : ""),
			     debug_indent, "",
			     _gdk_win32_message_to_string (message), hwnd));
  debug_indent += 2;
  retval = inner_display_change_hwnd_procedure (hwnd, message, wparam, lparam);
  debug_indent -= 2;

  GDK_NOTE (EVENTS, g_print (" => %" G_GINT64_FORMAT "%s", (gint64) retval, (debug_indent == 0 ? "\n" : "")));

  return retval;
}

/* Use a hidden HWND to be notified about display changes */
static void
register_display_change_notification (GdkDisplay *display)
{
  GdkWin32Display *display_win32 = GDK_WIN32_DISPLAY (display);
  WNDCLASS wclass = { 0, };
  ATOM klass;

  wclass.lpszClassName = L"GdkDisplayChange";
  wclass.lpfnWndProc = display_change_hwnd_procedure;
  wclass.hInstance = this_module ();
  wclass.style = CS_OWNDC;

  klass = RegisterClass (&wclass);
  if (klass)
    {
      display_win32->hwnd = CreateWindow (MAKEINTRESOURCE (klass),
                                          NULL, WS_POPUP,
                                          0, 0, 0, 0, NULL, NULL,
                                          this_module (), NULL);
      if (!display_win32->hwnd)
        {
          UnregisterClass (MAKEINTRESOURCE (klass), this_module ());
        }
    }
}

/* set up input language or text service change notification for our GdkDisplay */

/* COM implementation for receiving notifications when input language or text service has changed in GDK */
static ULONG STDMETHODCALLTYPE
alpn_sink_addref (ITfActiveLanguageProfileNotifySink *This)
{
  GdkWin32ALPNSink *alpn_sink = (GdkWin32ALPNSink *) This;
  int ref_count = ++alpn_sink->ref_count;

  return ref_count;
}

static HRESULT STDMETHODCALLTYPE
alpn_sink_queryinterface (ITfActiveLanguageProfileNotifySink *This,
                          REFIID                              riid,
                          LPVOID                             *ppvObject)
{
  *ppvObject = NULL;

  if (IsEqualGUID (riid, &IID_IUnknown))
    {
      ITfActiveLanguageProfileNotifySink_AddRef (This);
      *ppvObject = This;
      return S_OK;
    }

  if (IsEqualGUID (riid, &IID_ITfActiveLanguageProfileNotifySink))
    {
      ITfActiveLanguageProfileNotifySink_AddRef (This);
      *ppvObject = This;
      return S_OK;
    }

  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
alpn_sink_release (ITfActiveLanguageProfileNotifySink *This)
{
  GdkWin32ALPNSink *alpn_sink = (GdkWin32ALPNSink *) This;
  int ref_count = --alpn_sink->ref_count;

  if (ref_count == 0)
    {
      g_free (This);
    }

  return ref_count;
}

static HRESULT STDMETHODCALLTYPE
alpn_sink_on_activated (ITfActiveLanguageProfileNotifySink *This,
                        REFCLSID                            clsid,
                        REFGUID                             guidProfile,
                        BOOL                                fActivated)
{
  GdkWin32ALPNSink *alpn_sink = (GdkWin32ALPNSink *) This;

  alpn_sink->input_locale_is_ime = fActivated;
  return S_OK;
}

static const ITfActiveLanguageProfileNotifySinkVtbl alpn_sink_vtbl = {
  alpn_sink_queryinterface,
  alpn_sink_addref,
  alpn_sink_release,
  alpn_sink_on_activated,
};

static void
gdk_win32_display_lang_notification_init (GdkWin32Display *display)
{
  HRESULT hr;
  ITfThreadMgr *itf_threadmgr;

  CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);

  if (display->input_locale_items->notifcation_sink != NULL)
    return;

  hr = CoCreateInstance (&CLSID_TF_ThreadMgr,
                         NULL,
                         CLSCTX_INPROC_SERVER,
                         &IID_ITfThreadMgr,
                         (LPVOID *) &itf_threadmgr);

  if (!SUCCEEDED (hr))
    return;

  hr = ITfThreadMgr_QueryInterface (itf_threadmgr, &IID_ITfSource, (VOID **) &display->input_locale_items->itf_source);
  ITfThreadMgr_Release (itf_threadmgr);

  if (!SUCCEEDED (hr))
    return;

  display->input_locale_items->notifcation_sink = g_new0 (GdkWin32ALPNSink, 1);
  display->input_locale_items->notifcation_sink->itf_alpn_sink.lpVtbl = &alpn_sink_vtbl;
  display->input_locale_items->notifcation_sink->ref_count = 0;
  ITfActiveLanguageProfileNotifySink_AddRef (&display->input_locale_items->notifcation_sink->itf_alpn_sink);

  hr = ITfSource_AdviseSink (display->input_locale_items->itf_source,
                             &IID_ITfActiveLanguageProfileNotifySink,
                             (IUnknown *) display->input_locale_items->notifcation_sink,
                             &display->input_locale_items->actlangchangenotify_id);

  if (!SUCCEEDED (hr))
    {
      ITfActiveLanguageProfileNotifySink_Release (&display->input_locale_items->notifcation_sink->itf_alpn_sink);
      display->input_locale_items->notifcation_sink = NULL;
      ITfSource_Release (display->input_locale_items->itf_source);
      display->input_locale_items->itf_source = NULL;
    }
}

static void
gdk_win32_display_lang_notification_exit (GdkWin32Display *display)
{
  if (display->input_locale_items->notifcation_sink != NULL && display->input_locale_items->itf_source != NULL)
    {
      ITfSource_UnadviseSink (display->input_locale_items->itf_source, display->input_locale_items->actlangchangenotify_id);
      ITfSource_Release (display->input_locale_items->itf_source);
      ITfActiveLanguageProfileNotifySink_Release (&display->input_locale_items->notifcation_sink->itf_alpn_sink);
    }

  CoUninitialize ();
}

GdkDisplay *
_gdk_win32_display_open (const char *display_name)
{
  GdkWin32Display *win32_display;

  GDK_NOTE (MISC, g_print ("gdk_display_open: %s\n", (display_name ? display_name : "NULL")));

  if (display_name == NULL ||
      g_ascii_strcasecmp (display_name,
			  gdk_display_get_name (_gdk_display)) == 0)
    {
      if (_gdk_display != NULL)
	{
	  GDK_NOTE (MISC, g_print ("... return _gdk_display\n"));
	  return _gdk_display;
	}
    }
  else
    {
      GDK_NOTE (MISC, g_print ("... return NULL\n"));
      return NULL;
    }

  _gdk_display = g_object_new (GDK_TYPE_WIN32_DISPLAY, NULL);
  win32_display = GDK_WIN32_DISPLAY (_gdk_display);

  win32_display->screen = g_object_new (GDK_TYPE_WIN32_SCREEN, NULL);

  _gdk_events_init (_gdk_display);

  _gdk_device_manager = g_object_new (GDK_TYPE_DEVICE_MANAGER_WIN32,
                                      NULL);
  _gdk_device_manager->display = _gdk_display;

  gdk_win32_display_lang_notification_init (win32_display);
  _gdk_drag_init ();

  _gdk_display->clipboard = gdk_win32_clipboard_new (_gdk_display);
  _gdk_display->primary_clipboard = gdk_clipboard_new (_gdk_display);

  /* Precalculate display name */
  (void) gdk_display_get_name (_gdk_display);

  register_display_change_notification (_gdk_display);

  g_signal_emit_by_name (_gdk_display, "opened");

  /* Precalculate keymap, see #6203 */
  (void) _gdk_win32_display_get_keymap (_gdk_display);

  GDK_NOTE (MISC, g_print ("... _gdk_display now set up\n"));

  return _gdk_display;
}

G_DEFINE_TYPE (GdkWin32Display, gdk_win32_display, GDK_TYPE_DISPLAY)

static const char *
gdk_win32_display_get_name (GdkDisplay *display)
{
  static const char *display_name_cache = NULL;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  if (g_once_init_enter (&display_name_cache))
    {
      HDESK hdesk = GetThreadDesktop (GetCurrentThreadId ());
      char dummy;
      char dummy_dword[11];
      wchar_t *desktop_name;
      HWINSTA hwinsta = GetProcessWindowStation ();
      wchar_t *window_station_name;
      DWORD n;
      DWORD session_id;
      wchar_t *display_name_w;
      char *display_name;
      size_t wchar_size;
      size_t display_name_len = 0;

      n = 0;
      GetUserObjectInformation (hdesk, UOI_NAME, &dummy, 0, &n);
      wchar_size = sizeof (wchar_t);
      if (n == 0)
        desktop_name = L"Default";
      else
        {
          n++;
          desktop_name = g_alloca ((n + 1) * wchar_size);
          memset (desktop_name, 0, (n + 1) * wchar_size);

          if (!GetUserObjectInformation (hdesk, UOI_NAME, desktop_name, n, &n))
          desktop_name = L"Default";
        }

      n = 0;
      GetUserObjectInformation (hwinsta, UOI_NAME, &dummy, 0, &n);
      if (n == 0)
        window_station_name = L"WinSta0";
      else
        {
          n++;
          window_station_name = g_alloca ((n + 1) * wchar_size);
          memset (window_station_name, 0, (n + 1) * wchar_size);

          if (!GetUserObjectInformation (hwinsta, UOI_NAME, window_station_name, n, &n))
            window_station_name = L"WinSta0";
        }

      if (!ProcessIdToSessionId (GetCurrentProcessId (), &session_id))
        session_id = 0;

      /* display_name is in the form of "%ld\\%s\\%s" */
      display_name_len = strlen (itoa (session_id, dummy_dword, 10)) + 1 + wcslen (window_station_name) + 1 + wcslen (desktop_name);
      display_name_w = g_alloca ((display_name_len + 1) * wchar_size);
      memset (display_name_w, 0, (display_name_len + 1) * wchar_size);
      swprintf_s (display_name_w, display_name_len + 1, L"%ld\\%s\\%s", session_id, window_station_name, desktop_name);

      display_name = g_utf16_to_utf8 (display_name_w, -1, NULL, NULL, NULL);

      GDK_NOTE (MISC, g_print ("gdk_win32_display_get_name: %s\n", display_name));

      g_once_init_leave (&display_name_cache, display_name);
    }

  return display_name_cache;
}

static void
gdk_win32_display_beep (GdkDisplay *display)
{
  g_return_if_fail (display == gdk_display_get_default());
  if (!MessageBeep (-1))
    Beep(1000, 50);
}

static void
gdk_win32_display_flush (GdkDisplay * display)
{
  g_return_if_fail (display == _gdk_display);

  GdiFlush ();
}

static void
gdk_win32_display_sync (GdkDisplay * display)
{
  g_return_if_fail (display == _gdk_display);

  GdiFlush ();
}

static void
gdk_win32_display_dispose (GObject *object)
{
  GdkWin32Display *display_win32 = GDK_WIN32_DISPLAY (object);

  if (display_win32->dummy_context_wgl.hglrc != NULL)
    {
      wglMakeCurrent (NULL, NULL);
      wglDeleteContext (display_win32->dummy_context_wgl.hglrc);
      display_win32->dummy_context_wgl.hglrc = NULL;
    }

  if (display_win32->hwnd != NULL)
    {
      DestroyWindow (display_win32->hwnd);
      display_win32->hwnd = NULL;
    }

  if (display_win32->have_at_least_win81)
    {
      if (display_win32->shcore_funcs.hshcore != NULL)
        {
          FreeLibrary (display_win32->shcore_funcs.hshcore);
          display_win32->shcore_funcs.hshcore = NULL;
        }
    }

  G_OBJECT_CLASS (gdk_win32_display_parent_class)->dispose (object);
}

static void
gdk_win32_display_finalize (GObject *object)
{
  GdkWin32Display *display_win32 = GDK_WIN32_DISPLAY (object);

  _gdk_win32_display_finalize_cursors (display_win32);
  _gdk_win32_dnd_exit ();
  gdk_win32_display_lang_notification_exit (display_win32);
  g_free (display_win32->input_locale_items->notifcation_sink);
  g_free (display_win32->input_locale_items);
  g_free (display_win32->cb_dnd_items);

  g_list_store_remove_all (G_LIST_STORE (display_win32->monitors));
  g_object_unref (display_win32->monitors);

  while (display_win32->filters)
    _gdk_win32_message_filter_unref (display_win32, display_win32->filters->data);

  G_OBJECT_CLASS (gdk_win32_display_parent_class)->finalize (object);
}

static void
_gdk_win32_enable_hidpi (GdkWin32Display *display)
{
  gboolean check_for_dpi_awareness = FALSE;
  gboolean have_hpi_disable_envvar = FALSE;

  HMODULE user32;
  user32 = GetModuleHandleW (L"user32.dll");

  enum dpi_aware_status {
    DPI_STATUS_PENDING,
    DPI_STATUS_SUCCESS,
    DPI_STATUS_DISABLED,
    DPI_STATUS_FAILED
  } status = DPI_STATUS_PENDING;

  if (g_win32_check_windows_version (6, 3, 0, G_WIN32_OS_ANY))
    {
      /* If we are on Windows 8.1 or later, cache up functions from shcore.dll, by all means */
      display->have_at_least_win81 = TRUE;

      if (user32 != NULL)
        {
          display->user32_dpi_funcs.setPDAC =
            (funcSPDAC) GetProcAddress (user32, "SetProcessDpiAwarenessContext");
          display->user32_dpi_funcs.getTDAC =
            (funcGTDAC) GetProcAddress (user32, "GetThreadDpiAwarenessContext");
          display->user32_dpi_funcs.areDACEqual =
            (funcADACE) GetProcAddress (user32, "AreDpiAwarenessContextsEqual");
        }

      display->shcore_funcs.hshcore = LoadLibraryW (L"shcore.dll");

      if (display->shcore_funcs.hshcore != NULL)
        {
          display->shcore_funcs.setDpiAwareFunc =
            (funcSetProcessDpiAwareness) GetProcAddress (display->shcore_funcs.hshcore,
                                                         "SetProcessDpiAwareness");
          display->shcore_funcs.getDpiAwareFunc =
            (funcGetProcessDpiAwareness) GetProcAddress (display->shcore_funcs.hshcore,
                                                         "GetProcessDpiAwareness");

          display->shcore_funcs.getDpiForMonitorFunc =
            (funcGetDpiForMonitor) GetProcAddress (display->shcore_funcs.hshcore,
                                                   "GetDpiForMonitor");
        }
    }
  else
    {
      /* Windows Vista through 8: use functions from user32.dll directly */

      display->have_at_least_win81 = FALSE;

      if (user32 != NULL)
        {
          display->user32_dpi_funcs.setDpiAwareFunc =
            (funcSetProcessDPIAware) GetProcAddress (user32, "SetProcessDPIAware");
          display->user32_dpi_funcs.isDpiAwareFunc =
            (funcIsProcessDPIAware) GetProcAddress (user32, "IsProcessDPIAware");
        }
    }

  if (g_getenv ("GDK_WIN32_DISABLE_HIDPI") == NULL)
    {
      /* For Windows 8.1 and later, use SetProcessDPIAwareness() */
      if (display->have_at_least_win81)
        {
          /* then make the GDK-using app DPI-aware */
          if (display->user32_dpi_funcs.setPDAC != NULL)
            {
              HANDLE hidpi_mode_ctx;
              GdkWin32ProcessDpiAwareness hidpi_mode;

              /* TODO: See how per-monitor DPI awareness is done by the Wayland backend */
              if (g_getenv ("GDK_WIN32_PER_MONITOR_HIDPI") != NULL)
                {
                  hidpi_mode_ctx = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
                  hidpi_mode = PROCESS_PER_MONITOR_DPI_AWARE;
                }
              else
                {
                  hidpi_mode_ctx = DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
                  hidpi_mode = PROCESS_SYSTEM_DPI_AWARE;
                }

              if (display->user32_dpi_funcs.setPDAC (hidpi_mode_ctx))
                {
                  display->dpi_aware_type = hidpi_mode;
                  status = DPI_STATUS_SUCCESS;
                }
              else
                {
                  DWORD err = GetLastError ();

                  if (err == ERROR_ACCESS_DENIED)
                    check_for_dpi_awareness = TRUE;
                  else
                    {
                      display->dpi_aware_type = PROCESS_DPI_UNAWARE;
                      status = DPI_STATUS_FAILED;
                    }
                }
            }
          else if (display->shcore_funcs.setDpiAwareFunc != NULL)
            {
              GdkWin32ProcessDpiAwareness hidpi_mode;

              /* TODO: See how per-monitor DPI awareness is done by the Wayland backend */
              if (g_getenv ("GDK_WIN32_PER_MONITOR_HIDPI") != NULL)
                hidpi_mode = PROCESS_PER_MONITOR_DPI_AWARE;
              else
                hidpi_mode = PROCESS_SYSTEM_DPI_AWARE;

              switch (display->shcore_funcs.setDpiAwareFunc (hidpi_mode))
                {
                  case S_OK:
                    display->dpi_aware_type = hidpi_mode;
                    status = DPI_STATUS_SUCCESS;
                    break;
                  case E_ACCESSDENIED:
                    /* This means the app used a manifest to set DPI awareness, or a
                       DPI compatibility setting is used.
                       The manifest is the trump card in this game of bridge here.  The
                       same applies if one uses the control panel or program properties to
                       force system DPI awareness */
                    check_for_dpi_awareness = TRUE;
                    break;
                  default:
                    display->dpi_aware_type = PROCESS_DPI_UNAWARE;
                    status = DPI_STATUS_FAILED;
                    break;
                }
            }
          else
            {
              check_for_dpi_awareness = TRUE;
            }
        }
      else
        {
          /* For Windows Vista through 8, use SetProcessDPIAware() */
          display->have_at_least_win81 = FALSE;
          if (display->user32_dpi_funcs.setDpiAwareFunc != NULL)
            {
              if (display->user32_dpi_funcs.setDpiAwareFunc () != 0)
                {
                  display->dpi_aware_type = PROCESS_SYSTEM_DPI_AWARE;
                  status = DPI_STATUS_SUCCESS;
                }
              else
                {
                  check_for_dpi_awareness = TRUE;
                }
            }
          else
            {
              display->dpi_aware_type = PROCESS_DPI_UNAWARE;
              status = DPI_STATUS_FAILED;
            }
        }
    }
  else
    {
      /* if GDK_WIN32_DISABLE_HIDPI is set, check for any DPI
       * awareness settings done via manifests or user settings
       */
      check_for_dpi_awareness = TRUE;
      have_hpi_disable_envvar = TRUE;
    }

  if (check_for_dpi_awareness)
    {
      if (display->have_at_least_win81)
        {
          if (display->user32_dpi_funcs.getTDAC != NULL &&
              display->user32_dpi_funcs.areDACEqual != NULL)
            {
              HANDLE dpi_aware_ctx = display->user32_dpi_funcs.getTDAC ();
              if (display->user32_dpi_funcs.areDACEqual (dpi_aware_ctx,
                                                         DPI_AWARENESS_CONTEXT_UNAWARE))
                /* This means the DPI awareness setting was forcefully disabled */
                status = DPI_STATUS_DISABLED;
              else
                {
                  status = DPI_STATUS_SUCCESS;
                  if (display->user32_dpi_funcs.areDACEqual (dpi_aware_ctx,
                                                             DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
                    display->dpi_aware_type = PROCESS_SYSTEM_DPI_AWARE;
                  else
                    display->dpi_aware_type = PROCESS_PER_MONITOR_DPI_AWARE_V2;
                }
            }
          else if (display->shcore_funcs.getDpiAwareFunc != NULL)
            {
              display->shcore_funcs.getDpiAwareFunc (NULL, &display->dpi_aware_type);

              if (display->dpi_aware_type != PROCESS_DPI_UNAWARE)
                status = DPI_STATUS_SUCCESS;
              else
                /* This means the DPI awareness setting was forcefully disabled */
                status = DPI_STATUS_DISABLED;
            }
          else
            {
              display->dpi_aware_type = PROCESS_DPI_UNAWARE;
              status = DPI_STATUS_FAILED;
            }
        }
      else
        {
          if (display->user32_dpi_funcs.isDpiAwareFunc != NULL)
            {
              /* This most probably means DPI awareness is set through
                 the manifest, or a DPI compatibility setting is used. */
              display->dpi_aware_type = display->user32_dpi_funcs.isDpiAwareFunc () ?
                                        PROCESS_SYSTEM_DPI_AWARE :
                                        PROCESS_DPI_UNAWARE;

              if (display->dpi_aware_type == PROCESS_SYSTEM_DPI_AWARE)
                status = DPI_STATUS_SUCCESS;
              else
                status = DPI_STATUS_DISABLED;
            }
          else
            {
              display->dpi_aware_type = PROCESS_DPI_UNAWARE;
              status = DPI_STATUS_FAILED;
            }
        }
      if (have_hpi_disable_envvar &&
          status == DPI_STATUS_SUCCESS)
        {
          /* The user setting or application manifest trumps over GDK_WIN32_DISABLE_HIDPI */
          g_print ("Note: GDK_WIN32_DISABLE_HIDPI is ignored due to preset\n"
                   "      DPI awareness settings in user settings or application\n"
                   "      manifest, DPI awareness is still enabled.\n");
        }
    }

  switch (status)
    {
    case DPI_STATUS_SUCCESS:
      GDK_NOTE (MISC, g_message ("HiDPI support enabled, type: %s",
                                 display->dpi_aware_type == PROCESS_PER_MONITOR_DPI_AWARE ? "per-monitor" : "system"));
      break;
    case DPI_STATUS_DISABLED:
      GDK_NOTE (MISC, g_message ("HiDPI support disabled via manifest"));
      break;
    case DPI_STATUS_FAILED:
      g_warning ("Failed to enable HiDPI support.");
      break;
    case DPI_STATUS_PENDING:
      g_assert_not_reached ();
      break;
    }
}

gboolean
_gdk_win32_check_processor (GdkWin32ProcessorCheckType check_type)
{
  static gsize checked = 0;
  static gboolean is_arm64 = FALSE;
  static gboolean is_wow64 = FALSE;

  if (g_once_init_enter (&checked))
    {
      gboolean fallback_wow64_check = FALSE;
      HMODULE kernel32 = LoadLibraryW (L"kernel32.dll");

      if (kernel32 != NULL)
        {
          typedef BOOL (WINAPI *funcIsWow64Process2) (HANDLE, USHORT *, USHORT *);

          funcIsWow64Process2 isWow64Process2 =
            (funcIsWow64Process2) GetProcAddress (kernel32, "IsWow64Process2");

          if (isWow64Process2 != NULL)
            {
              USHORT proc_cpu = 0;
              USHORT native_cpu = 0;

              isWow64Process2 (GetCurrentProcess (), &proc_cpu, &native_cpu);

              if (native_cpu == IMAGE_FILE_MACHINE_ARM64)
                is_arm64 = TRUE;

              if (proc_cpu != IMAGE_FILE_MACHINE_UNKNOWN)
                is_wow64 = TRUE;
            }
          else
            {
              fallback_wow64_check = TRUE;
            }

          FreeLibrary (kernel32);
        }
      else
        {
          fallback_wow64_check = TRUE;
        }

      if (fallback_wow64_check)
        IsWow64Process (GetCurrentProcess (), &is_wow64);

      g_once_init_leave (&checked, 1);
    }

  switch (check_type)
    {
      case GDK_WIN32_ARM64:
        return is_arm64;
        break;

      case GDK_WIN32_WOW64:
        return is_wow64;
        break;

      default:
        g_warning ("unknown CPU check type");
        return FALSE;
        break;
    }
}

static void
gdk_win32_display_init (GdkWin32Display *display_win32)
{
  const char *scale_str = g_getenv ("GDK_SCALE");

  display_win32->monitors = G_LIST_MODEL (g_list_store_new (GDK_TYPE_MONITOR));
  display_win32->input_locale_items = g_new0 (GdkWin32InputLocaleItems, 1);
  display_win32->cb_dnd_items = g_new0 (GdkWin32CbDnDItems, 1);
  display_win32->cb_dnd_items->display_main_thread = g_thread_self ();

  _gdk_win32_enable_hidpi (display_win32);
  display_win32->running_on_arm64 = _gdk_win32_check_processor (GDK_WIN32_ARM64);

  /* if we have DPI awareness, set up fixed scale if set */
  if (display_win32->dpi_aware_type != PROCESS_DPI_UNAWARE &&
      scale_str != NULL)
    {
      display_win32->surface_scale = atol (scale_str);

      if (display_win32->surface_scale <= 0)
        display_win32->surface_scale = 1;

      display_win32->has_fixed_scale = TRUE;
    }
  else
    display_win32->surface_scale =
      gdk_win32_display_get_monitor_scale_factor (display_win32, NULL, NULL);

  _gdk_win32_display_init_cursors (display_win32);
  gdk_win32_display_check_composited (display_win32);
}

void
gdk_win32_display_check_composited (GdkWin32Display *display)
{
  gboolean composited;

  /* On Windows 8 and later, DWM (composition) is always enabled */
  if (g_win32_check_windows_version (6, 2, 0, G_WIN32_OS_ANY))
    {
      composited = TRUE;
    }
  else
    {
      if (DwmIsCompositionEnabled (&composited) != S_OK)
        composited = FALSE;
    }

  gdk_display_set_composited (GDK_DISPLAY (display), composited);
  gdk_display_set_shadow_width (GDK_DISPLAY (display), composited);
}

static void
gdk_win32_display_notify_startup_complete (GdkDisplay  *display,
                                           const char *startup_id)
{
  /* nothing */
}

GdkMonitor *
gdk_win32_display_get_primary_monitor (GdkDisplay *display)
{
  GdkWin32Display *self = GDK_WIN32_DISPLAY (display);
  GdkMonitor *result;

  /* We arrange for the first monitor in the array to also be the primary monitor */
  result = g_list_model_get_item (self->monitors, 0);
  g_object_unref (result);

  return result;
}

static GListModel *
gdk_win32_display_get_monitors (GdkDisplay *display)
{
  GdkWin32Display *self = GDK_WIN32_DISPLAY (display);

  return self->monitors;
}

guint
gdk_win32_display_get_monitor_scale_factor (GdkWin32Display *display_win32,
                                            GdkSurface      *surface,
                                            HMONITOR         hmonitor)
{
  gboolean is_scale_acquired = FALSE;
  gboolean use_dpi_for_monitor = FALSE;
  guint dpix, dpiy;

  if (display_win32->have_at_least_win81)
    {
      if (surface != NULL && hmonitor == NULL)
        hmonitor = MonitorFromWindow (GDK_SURFACE_HWND (surface),
                                      MONITOR_DEFAULTTONEAREST);
      if (hmonitor != NULL &&
          display_win32->shcore_funcs.hshcore != NULL &&
          display_win32->shcore_funcs.getDpiForMonitorFunc != NULL)
        use_dpi_for_monitor = TRUE;
    }

  if (use_dpi_for_monitor)
    {
      /* Use GetDpiForMonitor() for Windows 8.1+, when we have a HMONITOR */
      if (display_win32->shcore_funcs.getDpiForMonitorFunc (hmonitor,
                                                            MDT_EFFECTIVE_DPI,
                                                           &dpix,
                                                           &dpiy) == S_OK)
        is_scale_acquired = TRUE;
    }
  else
    {
      /* Go back to GetDeviceCaps() for Windows 8 and earlier, or when we don't
       * have a HMONITOR nor a HWND
       */
      HDC hdc;

      if (surface != NULL)
        {
          if (GDK_WIN32_SURFACE (surface)->hdc == NULL)
            GDK_WIN32_SURFACE (surface)->hdc = GetDC (GDK_SURFACE_HWND (surface));
          hdc = GDK_WIN32_SURFACE (surface)->hdc;
        }
      else
        hdc = GetDC (NULL);

      /* in case we can't get the DC for the HWND, return 1 for the scale */
      if (hdc == NULL)
        return 1;

      dpix = GetDeviceCaps (hdc, LOGPIXELSX);
      dpiy = GetDeviceCaps (hdc, LOGPIXELSY);

      /*
       * If surface is not NULL, the HDC should not be released, since surfaces have
       * Win32 HWNDs created with CS_OWNDC
       */
      if (surface == NULL)
        ReleaseDC (NULL, hdc);

      is_scale_acquired = TRUE;
    }

  if (is_scale_acquired)
    /* USER_DEFAULT_SCREEN_DPI = 96, in winuser.h */
    {
      if (display_win32->has_fixed_scale)
        return display_win32->surface_scale;
      else
        return dpix / USER_DEFAULT_SCREEN_DPI > 1 ? dpix / USER_DEFAULT_SCREEN_DPI : 1;
    }
  else
    return 1;
}

static char *
_get_system_font_name (HDC hdc)
{
  NONCLIENTMETRICSW ncm;
  PangoFontDescription *font_desc;
  char *result, *font_desc_string;
  int logpixelsy;
  int font_size;

  ncm.cbSize = sizeof(NONCLIENTMETRICSW);
  if (!SystemParametersInfoW (SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
    return NULL;

  logpixelsy = GetDeviceCaps (hdc, LOGPIXELSY);
  font_desc = pango_win32_font_description_from_logfontw (&ncm.lfMessageFont);
  font_desc_string = pango_font_description_to_string (font_desc);
  pango_font_description_free (font_desc);

  /* https://docs.microsoft.com/en-us/windows/desktop/api/wingdi/ns-wingdi-taglogfonta */
  font_size = -MulDiv (ncm.lfMessageFont.lfHeight, 72, logpixelsy);
  result = g_strdup_printf ("%s %d", font_desc_string, font_size);
  g_free (font_desc_string);

  return result;
}

static gboolean
gdk_win32_display_get_setting (GdkDisplay  *display,
                               const char *name,
                               GValue      *value)
{
  if (gdk_display_get_debug_flags (display) & GDK_DEBUG_DEFAULT_SETTINGS)
    return FALSE;

  if (strcmp ("gtk-alternative-button-order", name) == 0)
    {
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : TRUE\n", name));
      g_value_set_boolean (value, TRUE);
      return TRUE;
    }
  else if (strcmp ("gtk-alternative-sort-arrows", name) == 0)
    {
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : TRUE\n", name));
      g_value_set_boolean (value, TRUE);
      return TRUE;
    }
  else if (strcmp ("gtk-cursor-blink", name) == 0)
    {
      gboolean blinks = (GetCaretBlinkTime () != INFINITE);
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %s\n", name, blinks ? "TRUE" : "FALSE"));
      g_value_set_boolean (value, blinks);
      return TRUE;
    }
  else if (strcmp ("gtk-cursor-theme-size", name) == 0)
    {
      int cursor_size = GetSystemMetrics (SM_CXCURSOR);
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %d\n", name, cursor_size));
      g_value_set_int (value, cursor_size);
      return TRUE;
    }
  else if (strcmp ("gtk-dnd-drag-threshold", name) == 0)
    {
      int i = MAX(GetSystemMetrics (SM_CXDRAG), GetSystemMetrics (SM_CYDRAG));
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %d\n", name, i));
      g_value_set_int (value, i);
      return TRUE;
    }
  else if (strcmp ("gtk-double-click-distance", name) == 0)
    {
      int i = MAX(GetSystemMetrics (SM_CXDOUBLECLK), GetSystemMetrics (SM_CYDOUBLECLK));
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %d\n", name, i));
      g_value_set_int (value, i);
      return TRUE;
    }
  else if (strcmp ("gtk-double-click-time", name) == 0)
    {
      int i = GetDoubleClickTime ();
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %d\n", name, i));
      g_value_set_int (value, i);
      return TRUE;
    }
  else if (strcmp ("gtk-font-name", name) == 0)
    {
      char *font_name = NULL;
      HDC hdc = NULL;

      if ((hdc = GetDC (HWND_DESKTOP)) != NULL)
        {
          font_name = _get_system_font_name (hdc);
          ReleaseDC (HWND_DESKTOP, hdc);
          hdc = NULL;
        }

      if (font_name)
        {
          GDK_NOTE(MISC, g_print("gdk_screen_get_setting(\"%s\") : %s\n", name, font_name));
          g_value_take_string (value, font_name);
          return TRUE;
        }
      else
        {
          g_warning ("gdk_win32_get_setting: Detecting the system font failed");
          return FALSE;
        }
    }
  else if (strcmp ("gtk-hint-font-metrics", name) == 0)
    {
      gboolean hint_font_metrics = TRUE;
      GDK_NOTE(MISC, g_print("gdk_screen_get_setting(\"%s\") : %s\n", name,
                             hint_font_metrics ? "TRUE" : "FALSE"));
      g_value_set_boolean (value, hint_font_metrics);
      return TRUE;
    }
  else if (strcmp ("gtk-im-module", name) == 0)
    {
      const char *im_module = GDK_WIN32_DISPLAY (display)->input_locale_items->notifcation_sink->input_locale_is_ime ? "ime" : "";
      GDK_NOTE(MISC, g_print("gdk_screen_get_setting(\"%s\") : %s\n", name, im_module));
      g_value_set_static_string (value, im_module);
      return TRUE;
    }
  else if (strcmp ("gtk-overlay-scrolling", name) == 0)
    {
      DWORD val = 0;
      DWORD sz = sizeof (val);
      LSTATUS ret = 0;

      ret = RegGetValueW (HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", RRF_RT_DWORD, NULL, &val, &sz);
      if (ret == ERROR_SUCCESS)
        {
          gboolean overlay_scrolling = val != 0;
          GDK_NOTE(MISC, g_print("gdk_screen_get_setting(\"%s\") : %s\n", name,
                                 overlay_scrolling ? "TRUE" : "FALSE"));
          g_value_set_boolean (value, overlay_scrolling);
          return TRUE;
        }
    }
  else if (strcmp ("gtk-shell-shows-desktop", name) == 0)
    {
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : TRUE\n", name));
      g_value_set_boolean (value, TRUE);
      return TRUE;
    }
  else if (strcmp ("gtk-split-cursor", name) == 0)
    {
      GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : FALSE\n", name));
      g_value_set_boolean (value, FALSE);
      return TRUE;
    }
  else if (strcmp ("gtk-theme-name", name) == 0)
    {
      HIGHCONTRASTW hc;
      memset (&hc, 0, sizeof (hc));
      hc.cbSize = sizeof (hc);
      if (API_CALL (SystemParametersInfoW, (SPI_GETHIGHCONTRAST, sizeof (hc), &hc, 0)))
        {
          if (hc.dwFlags & HCF_HIGHCONTRASTON)
            {
              const char *theme_name = "Default-hc";

              GDK_NOTE(MISC, g_print("gdk_display_get_setting(\"%s\") : %s\n", name, theme_name));
              g_value_set_string (value, theme_name);
              return TRUE;
            }
        }
    }
  else if (strcmp ("gtk-xft-antialias", name) == 0)
    {
      GDK_NOTE(MISC, g_print ("gdk_screen_get_setting(\"%s\") : 1\n", name));
      g_value_set_int (value, 1);
      return TRUE;
    }
  else if (strcmp ("gtk-xft-dpi", name) == 0)
    {
      GdkWin32Display *display = GDK_WIN32_DISPLAY (_gdk_display);

      if (display->dpi_aware_type == PROCESS_SYSTEM_DPI_AWARE &&
          !display->has_fixed_scale)
        {
          HDC hdc = GetDC (NULL);

          if (hdc != NULL)
            {
              int dpi = GetDeviceCaps (GetDC (NULL), LOGPIXELSX);
              ReleaseDC (NULL, hdc);

              if (dpi >= 96)
                {
                  int xft_dpi = 1024 * dpi / display->surface_scale;
                  GDK_NOTE(MISC, g_print ("gdk_screen_get_setting(\"%s\") : %d\n", name, xft_dpi));
                  g_value_set_int (value, xft_dpi);
                  return TRUE;
                }
            }
        }
    }
  else if (strcmp ("gtk-xft-hinting", name) == 0)
    {
      GDK_NOTE(MISC, g_print ("gdk_screen_get_setting(\"%s\") : 1\n", name));
      g_value_set_int (value, 1);
      return TRUE;
    }
  else if (strcmp ("gtk-xft-hintstyle", name) == 0)
    {
      g_value_set_static_string (value, "hintfull");
      GDK_NOTE(MISC, g_print ("gdk_screen_get_setting(\"%s\") : %s\n", name, g_value_get_string (value)));
      return TRUE;
    }
  else if (strcmp ("gtk-xft-rgba", name) == 0)
    {
      unsigned int orientation = 0;
      if (SystemParametersInfoW (SPI_GETFONTSMOOTHINGORIENTATION, 0, &orientation, 0))
        {
          if (orientation == FE_FONTSMOOTHINGORIENTATIONRGB)
            g_value_set_static_string (value, "rgb");
          else if (orientation == FE_FONTSMOOTHINGORIENTATIONBGR)
            g_value_set_static_string (value, "bgr");
          else
            g_value_set_static_string (value, "none");
        }
      else
        g_value_set_static_string (value, "none");

      GDK_NOTE(MISC, g_print ("gdk_screen_get_setting(\"%s\") : %s\n", name, g_value_get_string (value)));
      return TRUE;
    }

  return FALSE;
}

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE          0x3202
#endif

static GdkGLContext *
gdk_win32_display_init_gl (GdkDisplay  *display,
                           GError     **error)
{
  GdkWin32Display *display_win32 = GDK_WIN32_DISPLAY (display);
  HDC init_gl_hdc = NULL;
  GdkGLContext *context;

  /*
   * No env vars set, do the regular GL initialization, first WGL and then EGL,
   * as WGL is the more tried-and-tested configuration.
   */

#ifdef HAVE_EGL
  /*
   * Disable defaulting to EGL as EGL is used more as a compatibility layer
   * on Windows rather than being a native citizen on Windows
   */
  if (!gdk_has_feature (GDK_FEATURE_WGL) || !gdk_has_feature (GDK_FEATURE_GL_API))
    {
      init_gl_hdc = GetDC (display_win32->hwnd);

      if (gdk_display_init_egl (display,
                                EGL_PLATFORM_ANGLE_ANGLE,
                                init_gl_hdc,
                                FALSE,
                                error))
        {
          return g_object_new (GDK_TYPE_WIN32_GL_CONTEXT_EGL,
                               "display", display,
                               "allowed-apis", GDK_GL_API_GLES,
                               NULL);
        }
      else
        g_clear_error (error);
    }
#endif

  context = gdk_win32_display_init_wgl (display, error);

  if (context)
    return context;

#ifdef HAVE_EGL
  g_clear_error (error);

  init_gl_hdc = GetDC (display_win32->hwnd);
  if (gdk_display_init_egl (display,
                            EGL_PLATFORM_ANGLE_ANGLE,
                            init_gl_hdc,
                            TRUE,
                            error))
    {
      return g_object_new (GDK_TYPE_WIN32_GL_CONTEXT_EGL,
                           "display", display,
                           NULL);

    }
#endif

  return NULL;
}

/**
 * gdk_win32_display_get_egl_display:
 * @display: (type GdkWin32Display): a Win32 display
 *
 * Retrieves the EGL display connection object for the given GDK display.
 *
 * Returns: (nullable): the EGL display
 *
 * Since: 4.4
 */
gpointer
gdk_win32_display_get_egl_display (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_WIN32_DISPLAY (display), NULL);

  return gdk_display_get_egl_display (display);
}

static void
gdk_win32_display_class_init (GdkWin32DisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkDisplayClass *display_class = GDK_DISPLAY_CLASS (klass);

  object_class->dispose = gdk_win32_display_dispose;
  object_class->finalize = gdk_win32_display_finalize;

  display_class->toplevel_type = GDK_TYPE_WIN32_TOPLEVEL;
  display_class->popup_type = GDK_TYPE_WIN32_POPUP;
  display_class->cairo_context_type = GDK_TYPE_WIN32_CAIRO_CONTEXT;

  display_class->get_name = gdk_win32_display_get_name;
  display_class->beep = gdk_win32_display_beep;
  display_class->sync = gdk_win32_display_sync;
  display_class->flush = gdk_win32_display_flush;
  display_class->has_pending = _gdk_win32_display_has_pending;
  display_class->queue_events = _gdk_win32_display_queue_events;

  //? display_class->get_app_launch_context = _gdk_win32_display_get_app_launch_context;

  display_class->get_next_serial = gdk_win32_display_get_next_serial;
  display_class->notify_startup_complete = gdk_win32_display_notify_startup_complete;

  display_class->get_keymap = _gdk_win32_display_get_keymap;

  display_class->get_monitors = gdk_win32_display_get_monitors;

#ifdef GDK_RENDERING_VULKAN
  display_class->vk_context_type = GDK_TYPE_WIN32_VULKAN_CONTEXT;
  display_class->vk_extension_name = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif

  display_class->get_setting = gdk_win32_display_get_setting;
  display_class->set_cursor_theme = gdk_win32_display_set_cursor_theme;
  display_class->init_gl = gdk_win32_display_init_gl;

  _gdk_win32_surfaceing_init ();
}
