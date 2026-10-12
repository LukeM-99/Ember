// discord.c: Discord Rich Presence, "Listening to Ember": the song, the artist, a progress bar
// and the album on hover. It speaks Discord's local IPC socket directly (frames of opcode,
// length and JSON). Off unless turned on in settings; while off, no socket is opened.
#include "ember.h"
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <unistd.h>

#define CLIENT_ID "1559198028286533723"  // the Ember application on discord.com/developers

static GSocketConnection *sock;
static GSource *reader;
static gint64 retry_at;           // after a failed connect, wait before trying again
static guint pending;             // coalesces bursts of changes into one update
static char *sent;                // the last activity sent, so repeats are skipped
static gint64 sent_start;

static struct { gboolean playing; char *title, *artist, *album, *albumartist, *key; gint64 start, end; } now;

static void disconnect(void) {
  if (reader) g_source_destroy(reader), g_source_unref(reader), reader = NULL;
  if (sock) {  // Discord clears the presence when we hang up
    g_io_stream_close(G_IO_STREAM(sock), NULL, NULL);
    g_object_unref(sock), sock = NULL;
  }
  g_clear_pointer(&sent, g_free);
}

static gboolean frame(guint32 op, const char *json) {
  guint32 head[2] = { GUINT32_TO_LE(op), GUINT32_TO_LE((guint32)strlen(json)) };
  GOutputStream *o = g_io_stream_get_output_stream(G_IO_STREAM(sock));
  if (g_output_stream_write_all(o, head, sizeof head, NULL, NULL, NULL) &&
      g_output_stream_write_all(o, json, strlen(json), NULL, NULL, NULL)) return TRUE;
  disconnect();
  return FALSE;
}

static gboolean on_read(GSocket *s, GIOCondition c, gpointer unused) {  // replies are only read and dropped
  (void)unused;
  char buf[4096];
  if (c & (G_IO_HUP | G_IO_ERR) || g_socket_receive(s, buf, sizeof buf, NULL, NULL) <= 0) {
    disconnect();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean connect_discord(void) {
  if (sock) return TRUE;
  if (g_get_monotonic_time() < retry_at) return FALSE;
  retry_at = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;  // Discord may simply not be running
  const char *dirs[] = { g_get_user_runtime_dir(), g_getenv("TMPDIR"), "/tmp" };
  const char *subs[] = { "", "app/com.discordapp.Discord", "snap.discord", ".flatpak/com.discordapp.Discord/xdg-run" };
  GSocketClient *client = g_socket_client_new();
  for (guint d = 0; d < G_N_ELEMENTS(dirs) && !sock; d++)
    for (guint s = 0; dirs[d] && s < G_N_ELEMENTS(subs) && !sock; s++)
      for (int i = 0; i < 10 && !sock; i++) {
        char *name = g_strdup_printf("discord-ipc-%d", i), *path = g_build_filename(dirs[d], subs[s], name, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
          GSocketAddress *a = g_unix_socket_address_new(path);
          sock = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(a), NULL, NULL);
          g_object_unref(a);
        }
        g_free(name), g_free(path);
      }
  g_object_unref(client);
  if (!sock) return FALSE;
  GSocket *s = g_socket_connection_get_socket(sock);
  reader = g_socket_create_source(s, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
  g_source_set_callback(reader, G_SOURCE_FUNC(on_read), NULL, NULL);
  g_source_attach(reader, NULL);
  return frame(0, "{\"v\":1,\"client_id\":\"" CLIENT_ID "\"}");  // the handshake
}

static void json_str(GString *o, const char *s) {  // a JSON string, cut to Discord's 128-byte limit
  char *cut = g_utf8_substring(s, 0, MIN(g_utf8_strlen(s, -1), 100));
  g_string_append_c(o, '"');
  for (const char *c = cut; *c; c++) {
    if (*c == '"' || *c == '\\') g_string_append_printf(o, "\\%c", *c);
    else if ((guchar)*c < 0x20) g_string_append_printf(o, "\\u%04x", *c);
    else g_string_append_c(o, *c);
  }
  if (g_utf8_strlen(cut, -1) < 2) g_string_append(o, "  ");  // Discord wants at least 2 characters
  g_string_append_c(o, '"');
  g_free(cut);
}

static gboolean send_now(gpointer unused) {
  (void)unused;
  pending = 0;
  if (!settings_get("discord") || !connect_discord()) return G_SOURCE_REMOVE;
  GString *a = g_string_new(NULL);
  if (now.playing && now.title) {  // paused or stopped: no presence
    const char *art = coverart_url(now.key, now.albumartist, now.album);
    g_string_append(a, "{\"type\":2,\"details\":");
    json_str(a, now.title);
    g_string_append(a, ",\"state\":");
    json_str(a, now.artist);
    g_string_append(a, ",\"status_display_type\":1");  // "Listening to <artist>", not "to Ember"
    if (now.end > now.start) g_string_append_printf(a, ",\"timestamps\":{\"start\":%" G_GINT64_FORMAT ",\"end\":%" G_GINT64_FORMAT "}", now.start, now.end);
    g_string_append(a, ",\"assets\":{\"large_image\":");
    json_str(a, art ? art : "ember");  // "ember": the logo uploaded to the Discord application
    g_string_append(a, ",\"large_text\":");
    json_str(a, now.album);
    g_string_append(a, "}}");
  } else g_string_append(a, "null");

  // skip repeats, but not a seek (a moved start time)
  if (g_strcmp0(a->str, sent) || ABS(now.start - sent_start) > 2000) {
    char *msg = g_strdup_printf("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":%s},\"nonce\":\"%" G_GINT64_FORMAT "\"}",
                                getpid(), a->str, g_get_real_time());
    if (frame(1, msg)) g_free(sent), sent = g_strdup(a->str), sent_start = now.start;
    g_free(msg);
  }
  g_string_free(a, TRUE);
  return G_SOURCE_REMOVE;
}

static void schedule(void) {  // Discord accepts about 5 updates in 20 s; next/next/next becomes one
  if (pending) g_source_remove(pending);
  pending = g_timeout_add(1000, send_now, NULL);
}

void discord_update(const struct mpd_song *song, gboolean playing, double elapsed, unsigned duration) {
  if (!settings_get("discord")) return;
  g_free(now.title), g_free(now.artist), g_free(now.album), g_free(now.albumartist), g_free(now.key);
  now.title = now.artist = now.album = now.albumartist = now.key = NULL;
  now.playing = playing && song;
  if (song) {
    const char *t = song_tag(song, MPD_TAG_TITLE), *ar = song_tag(song, MPD_TAG_ARTIST);
    const char *al = song_tag(song, MPD_TAG_ALBUM), *aa = song_tag(song, MPD_TAG_ALBUM_ARTIST);
    now.title = t ? g_strdup(t) : g_path_get_basename(mpd_song_get_uri(song));
    now.artist = g_strdup(ar ? ar : "Unknown Artist");
    now.album = g_strdup(al ? al : "Unknown Album");
    now.albumartist = g_strdup(aa ? aa : now.artist);
    now.key = album_key(song);
    now.start = g_get_real_time() / 1000 - (gint64)(elapsed * 1000);
    now.end = duration ? now.start + (gint64)duration * 1000 : 0;
  }
  schedule();
}

void discord_art_found(void) { if (settings_get("discord")) schedule(); }

void discord_enable(gboolean on) {  // from the settings menu
  if (on) { retry_at = 0; schedule(); return; }
  if (pending) g_source_remove(pending), pending = 0;
  disconnect();  // which clears the presence
}
