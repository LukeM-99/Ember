// settings.c: ~/.config/ember/settings (a key file) and the top bar's ⋯ menu that changes it.
#include "ember.h"
#include <string.h>

static GKeyFile *kf;
static char *path;

static void save(void) {
  char *d = g_path_get_dirname(path);
  g_mkdir_with_parents(d, 0755);
  g_key_file_save_to_file(kf, path, NULL);
  g_free(d);
}

const char *settings_style(void) {  // EMBER_STYLE wins, for trying a style once
  static char *s;
  const char *env = g_getenv("EMBER_STYLE");
  if (env) return env;
  g_free(s);
  s = g_key_file_get_string(kf, "ember", "style", NULL);
  return s && !g_strcmp0(s, "classic") ? "classic" : "flat";
}

gboolean settings_discord(void) { return g_key_file_get_boolean(kf, "ember", "discord", NULL); }
gboolean settings_cover_lookup(void) { return g_key_file_get_boolean(kf, "ember", "cover-lookup", NULL); }

gboolean settings_keep_playing(void) {
  return g_key_file_get_boolean(kf, "ember", "keep-playing", NULL);  // missing: FALSE
}

static void on_style(GtkCheckMenuItem *item, const char *style) {
  if (!gtk_check_menu_item_get_active(item)) return;  // the radio being switched off
  g_key_file_set_string(kf, "ember", "style", style);
  save();
  theme_reload();
}

static void on_toggle(GtkCheckMenuItem *item, const char *key) {
  gboolean on = gtk_check_menu_item_get_active(item);
  g_key_file_set_boolean(kf, "ember", key, on);
  save();
  if (!strcmp(key, "discord")) {
    discord_enable(on);
    if (on && begin()) end();  // end() refreshes, which hands Discord the playing song
  } else discord_art_found();
}

static GtkWidget *check(GtkWidget *menu, const char *label, const char *key) {
  GtkWidget *c = gtk_check_menu_item_new_with_label(label);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(c), g_key_file_get_boolean(kf, "ember", key, NULL));
  g_signal_connect(c, "toggled", G_CALLBACK(on_toggle), (gpointer)key);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), c);
  return c;
}

static void on_keep(GtkCheckMenuItem *item) {
  g_key_file_set_boolean(kf, "ember", "keep-playing", gtk_check_menu_item_get_active(item));
  save();
}

void settings_init(void) {
  path = g_build_filename(g_get_user_config_dir(), "ember", "settings", NULL);
  kf = g_key_file_new();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
}

GtkWidget *settings_button(void) {
  GtkWidget *menu = gtk_menu_new();
  GSList *group = NULL;
  const char *now = settings_style();
  static const char *styles[][2] = { { "flat", "Flat Style (iOS 7)" }, { "classic", "Classic Style (iOS 6)" } };
  for (int i = 0; i < 2; i++) {
    GtkWidget *r = gtk_radio_menu_item_new_with_label(group, styles[i][1]);
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(r));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(r), !strcmp(now, styles[i][0]));
    gtk_widget_set_sensitive(r, !g_getenv("EMBER_STYLE"));
    g_signal_connect(r, "toggled", G_CALLBACK(on_style), (gpointer)styles[i][0]);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), r);
  }
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  GtkWidget *keep = gtk_check_menu_item_new_with_label("Keep Playing After Ember Closes");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(keep), settings_keep_playing());
  g_signal_connect(keep, "toggled", G_CALLBACK(on_keep), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), keep);
  // modules that talk to other services: off until turned on
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  check(menu, "Show What's Playing on Discord", "discord");
  check(menu, "Look Up Album Covers Online (for Discord)", "cover-lookup");
  gtk_widget_show_all(menu);

  GtkWidget *b = gtk_menu_button_new();
  gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR));
  gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(b, "Settings");
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(b), menu);
  return b;
}
