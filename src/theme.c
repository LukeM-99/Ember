// theme.c: Kindling's wallpaper palette (~/.cache/kindling/colors, key=#rrggbb lines) as GTK
// colours, reloaded whenever kd-theme rewrites the file. Without Kindling, the GTK theme stays.
// The style, flat (iOS 7-8, the default) or classic (iOS 4-6), comes from settings.c.
#include "ember.h"
#include <string.h>

// Shared by both styles: the content areas.
static const char *shared =
  "window, dialog, messagedialog, .background { background-color: @bg; color: @fg; }\n"
  ".dim { color: @fg2; }\n"
  "button { background: none; border: none; box-shadow: none; color: @fg; border-radius: 6px;"
  "  -gtk-icon-shadow: none; outline-color: alpha(@accent, .6); }\n"
  "button:hover { background-color: alpha(@fg, .08); }\n"
  "button:active { background-color: alpha(@accent, .25); }\n"
  "button:disabled { color: @fg2; }\n"
  "scale { outline-color: alpha(@accent, .6); }\n"
  "scale highlight { background: @accent; border: none; }\n"
  "scale slider { border: none; box-shadow: none; border-radius: 50%; }\n"
  "scale:disabled highlight, scale:disabled slider { background: @bg3; }\n"
  "entry { color: @fg; caret-color: @accent; box-shadow: none; }\n"
  "entry image { color: @fg2; }\n"
  "treeview.view, iconview { background-color: @bg; color: @fg; }\n"
  "treeview.view:selected, iconview:selected { background-color: alpha(@accent, .3); color: @fg; }\n"
  "treeview.view header button { background: @bg; color: @fg2; border: none; box-shadow: none;"
  "  border-bottom: 1px solid alpha(@fg, .08); padding: 4px 6px; border-radius: 0; }\n"
  "popover { background: @bg2; border: 1px solid alpha(@fg, .12); box-shadow: 0 4px 16px alpha(black, .4); }\n"
  "popover treeview.view { background-color: @bg2; }\n"
  "scrollbar { background: none; border: none; }\n"
  "scrollbar slider { background: alpha(@fg, .25); border: none; border-radius: 4px; min-width: 6px; min-height: 6px; }\n"
  "paned > separator { background: alpha(@fg, .08); min-width: 1px; }\n"
  "menu, .menu, .context-menu { background: @bg2; color: @fg; border: 1px solid alpha(@fg, .12); padding: 4px 0; }\n"
  "menuitem { padding: 5px 14px; }\n"
  "menuitem:hover { background: alpha(@accent, .3); color: @fg; }\n"
  "menuitem arrow { color: @fg2; }\n"
  "menu separator { background: alpha(@fg, .1); margin: 4px 0; min-height: 1px; }\n"
  "dialog .dialog-action-area button, messagedialog .dialog-action-box button { background: alpha(@fg, .08); padding: 5px 16px; }\n"
  "messagedialog .dialog-action-box { margin-top: 8px; }\n"
  "dialog, messagedialog { background-color: @bg2; }\n"
  "button.destructive-action { color: @urgent; }\n"
  "dialog entry { background: @bg; border: 1px solid alpha(@fg, .15); border-radius: 4px; padding: 4px 8px; }\n"
  "button.newlist, button.textbutton { color: @accent; }\n"
  "button.newlist { margin: 0 8px; }\n"
  ".nowbar button:checked { background: alpha(@accent, .2); }\n"
  ".nowbar button.mode { color: @fg2; }\n"  /* shuffle, repeat off: dim, like the iPod's */
  ".nowbar button.mode:checked { color: @accent; }\n"
  ".mini, .micro { border: 1px solid alpha(black, .5); }\n"
  ".mini .title { font-weight: bold; }\n"
  ".shade { background: alpha(black, .55); }\n"
  ".shade button { color: white; -gtk-icon-shadow: 0 1px 2px alpha(black, .6); }\n"
  ".shade button:hover { background: alpha(white, .15); }\n"
  ".mini scale slider, .micro scale slider { min-width: 8px; min-height: 8px; margin: -3px; }\n"
  ".micro scale trough { background: alpha(white, .25); }\n"
  ".micro scale slider { background: white; }\n"
  ".nowbar label link { color: @fg2; }\n"
  ".nowbar label link:hover { color: @accent; }\n"
  "button.link, button.albumlink, button.back { padding: 0; min-height: 0; }\n"
  "button.link { color: @fg2; font-size: larger; }\n"
  "button.albumlink { font-weight: bold; font-size: larger; }\n"
  "button.back { color: @accent; margin-bottom: 6px; }\n"
  "button.link:hover, button.albumlink:hover, button.back:hover { background: none; color: @accent; }\n"
  "button.pill { color: @accent; background: alpha(@accent, .15); border-radius: 14px; padding: 4px 18px; }\n"
  "button.pill:hover { background: alpha(@accent, .28); }\n"
  "selection, *:selected { background-color: @accent; color: @bg; }\n";

// iOS 7-8 (2013-15): flat bars, hairlines, controls in the accent colour, text tabs.
static const char *flat =
  ".mini { background: @bg2; }\n"
  ".nowbar { background: @bg2; border-bottom: 1px solid alpha(@fg, .08); }\n"
  ".tabbar { background: @bg2; border-top: 1px solid alpha(@fg, .08); }\n"
  ".nowbar button { color: @accent; }\n"
  ".tabbar button { color: @fg2; padding: 4px 18px; }\n"
  ".tabbar button:checked { color: @accent; background: none; }\n"
  "scale trough { background: alpha(@fg, .15); border: none; border-radius: 1px; min-height: 2px; }\n"
  "scale slider { background: @fg; min-width: 12px; min-height: 12px; margin: -6px; }\n"
  "entry { background: alpha(@fg, .07); border: none; border-radius: 6px; }\n";

// iOS 4-6 (2010-12): gradient bars, embossed icons, a segmented tab control, a pill search field.
static const char *classic =
  ".mini { background-image: linear-gradient(to bottom, @bg3, @bg2); }\n"
  ".nowbar { background-image: linear-gradient(to bottom, @bg3, @bg2); border-bottom: 1px solid @bg; }\n"
  ".tabbar { background-image: linear-gradient(to bottom, @bg3, @bg2); border-top: 1px solid @bg; }\n"
  ".nowbar button { -gtk-icon-shadow: 0 1px alpha(black, .6); }\n"
  ".tabbar button { padding: 3px 16px; border-radius: 0; border: 1px solid @bg;"
  "  background-image: linear-gradient(to bottom, alpha(@fg, .12), alpha(@fg, .02)); }\n"
  ".tabbar button:first-child { border-radius: 5px 0 0 5px; }\n"
  ".tabbar button:last-child { border-radius: 0 5px 5px 0; }\n"
  ".tabbar button:checked { background-image: linear-gradient(to bottom, alpha(@accent, .55), alpha(@accent, .3)); }\n"
  "scale trough { background: @bg; border: none; border-radius: 3px; min-height: 4px;"
  "  box-shadow: inset 0 1px 2px alpha(black, .5); }\n"
  "scale highlight { border-radius: 3px; }\n"
  "scale slider { min-width: 14px; min-height: 14px; margin: -5px;"
  "  background-image: linear-gradient(to bottom, @fg, @fg2); box-shadow: 0 1px 2px alpha(black, .5); }\n"
  "entry { background: @bg; border: 1px solid alpha(black, .4); border-radius: 14px;"
  "  box-shadow: inset 0 1px 2px alpha(black, .4); }\n";

static const char *needed[] = { "bg", "bg2", "bg3", "fg", "fg2", "accent", "urgent" };
static GtkCssProvider *current;
static char *palette;  // ~/.cache/kindling/colors
static GFileMonitor *monitor;

static gboolean is_colour(const char *v) {
  if (strlen(v) != 7 || v[0] != '#') return FALSE;
  for (int i = 1; i < 7; i++) if (!g_ascii_isxdigit(v[i])) return FALSE;
  return TRUE;
}

static void load(const char *path) {
  char *text;
  if (!g_file_get_contents(path, &text, NULL, NULL)) return;
  GString *css = g_string_new(NULL);
  unsigned found = 0;
  char **lines = g_strsplit(text, "\n", -1);
  for (char **l = lines; *l; l++) {
    char **kv = g_strsplit(*l, "=", 2);
    if (kv[0] && kv[1] && *kv[0] && is_colour(kv[1]) && strspn(kv[0], "abcdefghijklmnopqrstuvwxyz0123456789") == strlen(kv[0])) {
      g_string_append_printf(css, "@define-color %s %s;\n", kv[0], kv[1]);
      for (unsigned i = 0; i < G_N_ELEMENTS(needed); i++) if (!strcmp(kv[0], needed[i])) found |= 1u << i;
    }
    g_strfreev(kv);
  }
  g_strfreev(lines);
  g_free(text);

  // a half-written or foreign file keeps the current look
  if (found == (1u << G_N_ELEMENTS(needed)) - 1) {
    g_string_append(css, shared);
    g_string_append(css, strcmp(settings_style(), "classic") ? flat : classic);
    GtkCssProvider *p = gtk_css_provider_new();
    GError *err = NULL;
    gtk_css_provider_load_from_data(p, css->str, -1, &err);
    if (err) {
      g_message("theme: %s", err->message);
      g_error_free(err);
      g_object_unref(p);
    } else {
      GdkScreen *screen = gdk_screen_get_default();
      if (current) gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(current)), g_object_unref(current);
      gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      current = p;
    }
  }
  g_string_free(css, TRUE);
}

static void on_change(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer unused) {
  (void)m, (void)other, (void)unused;
  if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_CREATED) {
    char *path = g_file_get_path(f);
    load(path);
    g_free(path);
  }
}

void theme_init(void) {
  // layout, not colour: applies with or without Kindling
  GtkCssProvider *base = gtk_css_provider_new();
  gtk_css_provider_load_from_data(base, ".nowbar { padding: 8px; } .tabbar { padding: 6px; }", -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(base), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(base);

  char *path = palette = g_build_filename(g_get_user_cache_dir(), "kindling", "colors", NULL);
  load(path);
  GFile *f = g_file_new_for_path(path);
  monitor = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
  if (monitor) g_signal_connect(monitor, "changed", G_CALLBACK(on_change), NULL);
  g_object_unref(f);
}

void theme_reload(void) { if (palette) load(palette); }  // after the style changes
