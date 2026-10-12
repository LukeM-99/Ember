// coverart.c: album covers from the internet, for Discord, which can only show a picture by URL.
// MusicBrainz finds the album, the Cover Art Archive has its front cover. Off unless turned on
// in settings; then each album is looked up once (one at a time, as MusicBrainz asks) and the
// answer kept in ~/.cache/ember/coverart. Requests go through curl.
#include "ember.h"
#include <string.h>

#define UA "Ember/0.1 ( https://github.com/LukeM-99/Kindling )"  // MusicBrainz wants a contact

typedef struct { char *key, *artist, *album; gboolean stripped; } Job;

static GKeyFile *cache;     // [covers] md5-of-key=url, or md5= for "no cover"
static char *cache_path;
static GQueue jobs = G_QUEUE_INIT;
static gboolean busy;
static void (*ready)(void);

static char *id(const char *key) { return g_compute_checksum_for_string(G_CHECKSUM_MD5, key, -1); }

static void job_free(Job *j) { g_free(j->key), g_free(j->artist), g_free(j->album), g_free(j); }

static void save(void) {
  char *d = g_path_get_dirname(cache_path);
  g_mkdir_with_parents(d, 0755);
  g_key_file_save_to_file(cache, cache_path, NULL);
  g_free(d);
}

static char *lucene(const char *s) {  // a quoted search term, with Lucene's special characters escaped
  GString *o = g_string_new("\"");
  for (; *s; s++) {
    if (strchr("+-&|!(){}[]^\"~*?:\\/", *s)) g_string_append_c(o, '\\');
    g_string_append_c(o, *s);
  }
  g_string_append_c(o, '"');
  return g_string_free(o, FALSE);
}

// "XSCAPE (Deluxe Version)" -> "XSCAPE", "Sempiternal [Explicit]" -> "Sempiternal": shops add these
// to album names, MusicBrainz doesn't. Only trailing groups go, and never the whole name.
static char *strip_edition(const char *album) {
  char *s = g_strstrip(g_strdup(album));
  for (size_t n; (n = strlen(s)) && (s[n - 1] == ')' || s[n - 1] == ']'); ) {
    char *open = strrchr(s, s[n - 1] == ')' ? '(' : '[');
    if (!open || open == s) break;
    *open = 0;
    g_strchomp(s);
  }
  return s;
}

static void next(void);

static gboolean next_later(gpointer unused) { (void)unused; busy = FALSE; next(); return G_SOURCE_REMOVE; }

static void finish(Job *j, const char *url) {  // url NULL: a failure worth retrying another time
  if (url) {
    char *k = id(j->key);
    g_key_file_set_string(cache, "covers", k, url);
    g_free(k);
    save();
    if (*url && ready) ready();
  }
  job_free(j);
  g_timeout_add(1100, next_later, NULL);  // MusicBrainz allows one request a second
}

static void on_check(GObject *p, GAsyncResult *res, gpointer data) {  // does the archive have a front cover?
  Job *j = data;
  char *out = NULL;
  g_subprocess_communicate_utf8_finish(G_SUBPROCESS(p), res, &out, NULL, NULL);
  char *url = g_object_get_data(p, "url");
  if (!out || !g_subprocess_get_successful(G_SUBPROCESS(p))) finish(j, NULL);
  else finish(j, out[0] == '2' || out[0] == '3' ? url : "");  // 200 or a redirect to the image
  g_free(out);
  g_object_unref(p);
}

static void on_search(GObject *p, GAsyncResult *res, gpointer data) {
  Job *j = data;
  char *out = NULL;
  g_subprocess_communicate_utf8_finish(G_SUBPROCESS(p), res, &out, NULL, NULL);
  gboolean worked = out && g_subprocess_get_successful(G_SUBPROCESS(p));
  g_object_unref(p);
  if (!worked) { g_free(out); finish(j, NULL); return; }

  // the first release group's id and score; no JSON library for two fields
  char *list = strstr(out, "\"release-groups\":["), *id = list ? strstr(list, "\"id\":\"") : NULL;
  char *score = list ? strstr(list, "\"score\":") : NULL;
  if (!id || !score || atoi(score + 8) < 90 || strlen(id + 6) < 36) {
    g_free(out);
    char *plain = strip_edition(j->album);
    if (!j->stripped && *plain && strcmp(plain, j->album)) {  // try again without "(Deluxe Version)" and co.
      g_free(j->album), j->album = plain, j->stripped = TRUE;
      g_queue_push_head(&jobs, j);
      g_timeout_add(1100, next_later, NULL);
      return;
    }
    g_free(plain);
    finish(j, "");
    return;
  }
  char *url = g_strdup_printf("https://coverartarchive.org/release-group/%.36s/front-250", id + 6);
  g_free(out);
  GSubprocess *c = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
    "curl", "-s", "-o", "/dev/null", "-I", "-w", "%{http_code}", "--max-time", "10", "-A", UA, url, NULL);
  if (!c) { g_free(url); finish(j, NULL); return; }
  g_object_set_data_full(G_OBJECT(c), "url", url, g_free);
  g_subprocess_communicate_utf8_async(c, NULL, NULL, on_check, j);
}

static void next(void) {
  Job *j;
  if (busy || !(j = g_queue_pop_head(&jobs))) return;
  busy = TRUE;
  char *a = lucene(j->artist), *b = lucene(j->album), *q = g_strdup_printf("releasegroup:%s AND artist:%s", b, a);
  char *qe = g_uri_escape_string(q, NULL, FALSE);
  char *url = g_strconcat("https://musicbrainz.org/ws/2/release-group/?fmt=json&limit=1&query=", qe, NULL);
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
    "curl", "-sf", "--max-time", "10", "-A", UA, url, NULL);
  g_free(a), g_free(b), g_free(q), g_free(qe), g_free(url);
  if (p) g_subprocess_communicate_utf8_async(p, NULL, NULL, on_search, j);
  else finish(j, NULL);  // no curl
}

// The cover's URL if it's known; otherwise NULL, and once it's found, ready() runs.
const char *coverart_url(const char *key, const char *artist, const char *album) {
  static char *url;
  if (!settings_cover_lookup() || !key || !strcmp(album, "Unknown Album")) return NULL;
  g_free(url);
  char *k = id(key);
  url = g_key_file_get_string(cache, "covers", k, NULL);
  g_free(k);
  if (url) return *url ? url : NULL;
  for (GList *l = jobs.head; l; l = l->next) if (!strcmp(((Job *)l->data)->key, key)) return NULL;
  Job *j = g_new0(Job, 1);
  j->key = g_strdup(key), j->artist = g_strdup(artist), j->album = g_strdup(album);
  g_queue_push_tail(&jobs, j);
  next();
  return NULL;
}

void coverart_init(void (*cb)(void)) {
  ready = cb;
  cache_path = g_build_filename(g_get_user_cache_dir(), "ember", "coverart", NULL);
  cache = g_key_file_new();
  g_key_file_load_from_file(cache, cache_path, G_KEY_FILE_NONE, NULL);
}
