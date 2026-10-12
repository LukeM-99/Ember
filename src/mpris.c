// mpris.c: MPRIS on the session bus (org.mpris.MediaPlayer2.ember), so playerctl, and with it
// Kindling's media keys, controls MPD through Ember. It also hands out the title and cover.
#include "ember.h"
#include <stdlib.h>
#include <string.h>

static const char *xml =
  "<node>"
  " <interface name='org.mpris.MediaPlayer2'>"
  "  <method name='Raise'/><method name='Quit'/>"
  "  <property name='CanQuit' type='b' access='read'/>"
  "  <property name='CanRaise' type='b' access='read'/>"
  "  <property name='HasTrackList' type='b' access='read'/>"
  "  <property name='Identity' type='s' access='read'/>"
  "  <property name='DesktopEntry' type='s' access='read'/>"
  "  <property name='SupportedUriSchemes' type='as' access='read'/>"
  "  <property name='SupportedMimeTypes' type='as' access='read'/>"
  " </interface>"
  " <interface name='org.mpris.MediaPlayer2.Player'>"
  "  <method name='Next'/><method name='Previous'/><method name='Pause'/><method name='PlayPause'/>"
  "  <method name='Stop'/><method name='Play'/>"
  "  <method name='Seek'><arg direction='in' name='Offset' type='x'/></method>"
  "  <method name='SetPosition'><arg direction='in' name='TrackId' type='o'/><arg direction='in' name='Position' type='x'/></method>"
  "  <method name='OpenUri'><arg direction='in' name='Uri' type='s'/></method>"
  "  <signal name='Seeked'><arg name='Position' type='x'/></signal>"
  "  <property name='PlaybackStatus' type='s' access='read'/>"
  "  <property name='Rate' type='d' access='readwrite'/>"
  "  <property name='Metadata' type='a{sv}' access='read'/>"
  "  <property name='Volume' type='d' access='readwrite'/>"
  "  <property name='Position' type='x' access='read'/>"
  "  <property name='MinimumRate' type='d' access='read'/>"
  "  <property name='MaximumRate' type='d' access='read'/>"
  "  <property name='CanGoNext' type='b' access='read'/>"
  "  <property name='CanGoPrevious' type='b' access='read'/>"
  "  <property name='CanPlay' type='b' access='read'/>"
  "  <property name='CanPause' type='b' access='read'/>"
  "  <property name='CanSeek' type='b' access='read'/>"
  "  <property name='CanControl' type='b' access='read'/>"
  " </interface>"
  "</node>";

#define PATH "/org/mpris/MediaPlayer2"
#define PLAYER "org.mpris.MediaPlayer2.Player"

static GDBusConnection *bus;
static const char *status = "Stopped";
static GVariant *metadata;           // floating-free: always a held reference
static double volume = -1;           // 0..1; -1: MPD has no mixer
static gint64 elapsed_us, elapsed_at, length_us;
static int song_id = -1;
static char *track, *key, *uri;      // current track's object path and cover

static gint64 position(void) {
  gint64 p = elapsed_us + (!strcmp(status, "Playing") ? g_get_monotonic_time() - elapsed_at : 0);
  return length_us ? MIN(p, length_us) : p;
}

static void changed(const char *name, GVariant *value) {
  if (!bus) { g_variant_unref(g_variant_ref_sink(value)); return; }
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&b, "{sv}", name, value);
  g_dbus_connection_emit_signal(bus, NULL, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
    g_variant_new("(sa{sv}as)", PLAYER, &b, NULL), NULL);
}

static GVariant *build_metadata(const struct mpd_song *song) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  if (song) {
    const char *v;
    g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(track));
    g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(length_us));
    char *base = g_path_get_basename(mpd_song_get_uri(song));
    v = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
    g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(v ? v : base));
    g_free(base);
    if ((v = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0))) {
      const char *a[] = { v, NULL };
      g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(a, -1));
    }
    if ((v = mpd_song_get_tag(song, MPD_TAG_ALBUM_ARTIST, 0))) {
      const char *a[] = { v, NULL };
      g_variant_builder_add(&b, "{sv}", "xesam:albumArtist", g_variant_new_strv(a, -1));
    }
    if ((v = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0))) g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(v));
    if ((v = mpd_song_get_tag(song, MPD_TAG_TRACK, 0))) g_variant_builder_add(&b, "{sv}", "xesam:trackNumber", g_variant_new_int32(atoi(v)));
    char *art = key ? art_file(key, art_size * 3 / 2) : NULL;  // the micro player's size
    if (!art && key) art = art_file(key, art_size);
    if (art) {
      char *u = g_filename_to_uri(art, NULL, NULL);
      if (u) g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(u));
      g_free(u), g_free(art);
    }
  }
  return g_variant_ref_sink(g_variant_builder_end(&b));
}

void mpris_update(const struct mpd_status *st, const struct mpd_song *song) {
  enum mpd_state s = mpd_status_get_state(st);
  const char *now = s == MPD_STATE_PLAY ? "Playing" : s == MPD_STATE_PAUSE ? "Paused" : "Stopped";
  gint64 predicted = position();
  elapsed_us = (gint64)mpd_status_get_elapsed_ms(st) * 1000;
  elapsed_at = g_get_monotonic_time();
  length_us = (gint64)mpd_status_get_total_time(st) * 1000000;
  if (strcmp(now, status)) status = now, changed("PlaybackStatus", g_variant_new_string(status));

  int vol = mpd_status_get_volume(st);
  double v = vol < 0 ? -1 : vol / 100.0;
  if (v != volume) volume = v, changed("Volume", g_variant_new_double(MAX(volume, 0)));

  int id = song ? (int)mpd_song_get_id(song) : -1;
  if (id != song_id) {
    song_id = id;
    g_free(track), g_free(key), g_free(uri);
    track = id >= 0 ? g_strdup_printf("/org/mpris/MediaPlayer2/Track/%d", id) : g_strdup("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    key = song ? album_key(song) : NULL;
    uri = song ? g_strdup(mpd_song_get_uri(song)) : NULL;
    if (key) art_want_size(key, uri, art_size * 3 / 2);  // so artUrl has a file to point at
    if (metadata) g_variant_unref(metadata);
    metadata = build_metadata(song);
    changed("Metadata", metadata);
  } else if (bus && ABS(elapsed_us - predicted) > 1500000)  // someone seeked
    g_dbus_connection_emit_signal(bus, NULL, PATH, PLAYER, "Seeked", g_variant_new("(x)", elapsed_us), NULL);
}

void mpris_art_changed(void) {  // the cover arrived after the song started: point artUrl at it
  if (!key || !metadata) return;
  char *art = art_file(key, art_size * 3 / 2);
  gboolean had = g_variant_lookup(metadata, "mpris:artUrl", "&s", NULL);
  if (art && !had && begin()) {
    struct mpd_song *song = mpd_run_current_song(conn);
    if (song && (int)mpd_song_get_id(song) == song_id) {
      g_variant_unref(metadata);
      metadata = build_metadata(song);
      changed("Metadata", metadata);
    }
    if (song) mpd_song_free(song);
    end();
  }
  g_free(art);
}

// ---- calls from the bus

static void play_state(int what) {  // 1 pause, 2 stop
  if (!begin()) return;
  if (what == 1) mpd_run_pause(conn, true);
  else mpd_run_stop(conn);
  end();
}

static void on_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                    const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer unused) {
  (void)c, (void)sender, (void)path, (void)iface, (void)unused;
  if (!strcmp(method, "Raise")) g_application_activate(g_application_get_default());
  else if (!strcmp(method, "Quit")) g_application_quit(g_application_get_default());
  else if (!strcmp(method, "Next")) player_next();
  else if (!strcmp(method, "Previous")) player_prev();
  else if (!strcmp(method, "PlayPause")) player_toggle();
  else if (!strcmp(method, "Play")) { if (strcmp(status, "Playing")) player_toggle(); }  // empty queue: everything
  else if (!strcmp(method, "Pause")) play_state(1);
  else if (!strcmp(method, "Stop")) play_state(2);
  else if (!strcmp(method, "Seek")) {
    gint64 off;
    g_variant_get(args, "(x)", &off);
    if (begin()) mpd_run_seek_current(conn, off / 1e6, true), end();
  } else if (!strcmp(method, "SetPosition")) {
    const char *t;
    gint64 at;
    g_variant_get(args, "(&ox)", &t, &at);
    if (!g_strcmp0(t, track) && at >= 0 && at <= length_us && begin()) mpd_run_seek_current(conn, at / 1e6, false), end();
  }  // OpenUri: not supported; MPD plays its own library
  g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *on_get(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *prop, GError **err, gpointer unused) {
  (void)c, (void)sender, (void)path, (void)err, (void)unused;
  if (!strcmp(iface, "org.mpris.MediaPlayer2")) {
    if (!strcmp(prop, "CanQuit") || !strcmp(prop, "CanRaise")) return g_variant_new_boolean(TRUE);
    if (!strcmp(prop, "HasTrackList")) return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "Identity")) return g_variant_new_string("Ember");
    if (!strcmp(prop, "DesktopEntry")) return g_variant_new_string("ember");
    return g_variant_new_strv(NULL, 0);  // SupportedUriSchemes, SupportedMimeTypes
  }
  if (!strcmp(prop, "PlaybackStatus")) return g_variant_new_string(status);
  if (!strcmp(prop, "Metadata")) return metadata ? g_variant_ref(metadata) : g_variant_new("a{sv}", NULL);
  if (!strcmp(prop, "Volume")) return g_variant_new_double(MAX(volume, 0));
  if (!strcmp(prop, "Position")) return g_variant_new_int64(position());
  if (!strcmp(prop, "Rate") || !strcmp(prop, "MinimumRate") || !strcmp(prop, "MaximumRate")) return g_variant_new_double(1);
  if (!strcmp(prop, "CanSeek")) return g_variant_new_boolean(length_us > 0);
  return g_variant_new_boolean(TRUE);  // CanGoNext, CanGoPrevious, CanPlay, CanPause, CanControl
}

static gboolean on_set(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                       const char *prop, GVariant *value, GError **err, gpointer unused) {
  (void)c, (void)sender, (void)path, (void)iface, (void)err, (void)unused;
  if (!strcmp(prop, "Volume") && volume >= 0 && begin())
    mpd_run_set_volume(conn, CLAMP(g_variant_get_double(value), 0, 1) * 100 + .5), end();
  return TRUE;  // Rate: only 1 is supported, so any write is a no-op
}

void mpris_init(GApplication *app) {
  bus = g_application_get_dbus_connection(app);
  if (!bus) return;
  static const GDBusInterfaceVTable vt = { on_call, on_get, on_set, { 0 } };
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
  for (int i = 0; node->interfaces[i]; i++)
    g_dbus_connection_register_object(bus, PATH, node->interfaces[i], &vt, NULL, NULL, NULL);
  g_dbus_node_info_unref(node);
  g_bus_own_name_on_connection(bus, "org.mpris.MediaPlayer2.ember", G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
}
