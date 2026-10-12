// art.c: album covers. A worker thread with its own MPD connection fetches each cover once
// (the folder's cover file, else art embedded in the song), shrinks it and caches it in
// ~/.cache/ember/covers/<size>/. The UI asks only for the albums on screen. The grid uses
// art_size; the micro player asks for a bigger one. In memory a cover is "<key>\x1e<size>".
#include "ember.h"
#include <string.h>

#define CHUNK (256 * 1024)  // bytes per round trip; MPD's default is 8 KB

typedef struct { char *key, *uri, *path; int size; GdkPixbuf *pix; } Job;

int art_size = 128;
static GHashTable *mem;      // key -> GdkPixbuf; a NULL value means the album has no cover
static GHashTable *pending;  // keys queued or being fetched
static GAsyncQueue *jobs;
static void (*changed)(void);
static char *dir;  // ~/.cache/ember/covers

static void unref0(gpointer p) { if (p) g_object_unref(p); }

static char *cache_path(const char *key, int size) {
  char *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, key, -1), *n = g_strdup_printf("%d", size);
  char *d = g_build_filename(dir, n, NULL), *p = g_build_filename(d, sum, NULL);
  g_mkdir_with_parents(d, 0755);
  g_free(sum), g_free(n), g_free(d);
  return p;
}

static char *mem_key(const char *key, int size) { return g_strdup_printf("%s\x1e%d", key, size); }

// Reads a whole picture in chunks; NULL if there is none.
static GBytes *fetch(struct mpd_connection *c, gboolean embedded, const char *uri, guchar *chunk) {
  GByteArray *buf = g_byte_array_new();
  for (;;) {
    int n = embedded ? mpd_run_readpicture(c, uri, buf->len, chunk, CHUNK)
                     : mpd_run_albumart(c, uri, buf->len, chunk, CHUNK);
    if (n <= 0) break;
    g_byte_array_append(buf, chunk, n);
  }
  if (mpd_connection_get_error(c) != MPD_ERROR_SUCCESS) {
    mpd_connection_clear_error(c);  // "no such file" is a server error; the worker checks the rest
    g_byte_array_set_size(buf, 0);
  }
  if (buf->len) return g_byte_array_free_to_bytes(buf);
  g_byte_array_unref(buf);
  return NULL;
}

static gboolean done(gpointer p) {  // back on the main thread
  Job *j = p;
  g_hash_table_remove(pending, j->key);
  g_hash_table_insert(mem, j->key, j->pix);
  g_free(j->uri), g_free(j->path), g_free(j);
  changed();
  return G_SOURCE_REMOVE;
}

static gpointer worker(gpointer unused) {
  (void)unused;
  struct mpd_connection *c = NULL;
  guchar *chunk = g_malloc(CHUNK);
  for (;;) {
    Job *j = g_async_queue_pop(jobs);
    if (!c && (c = mpd_open()) && !mpd_run_binarylimit(c, CHUNK)) mpd_connection_clear_error(c);
    GBytes *data = c ? fetch(c, FALSE, j->uri, chunk) : NULL;
    if (c && !data) data = fetch(c, TRUE, j->uri, chunk);
    if (c && mpd_connection_get_error(c) != MPD_ERROR_SUCCESS) mpd_connection_free(c), c = NULL;
    if (data) {
      GInputStream *in = g_memory_input_stream_new_from_bytes(data);
      j->pix = gdk_pixbuf_new_from_stream_at_scale(in, j->size, j->size, TRUE, NULL, NULL);
      if (j->pix)  // JPEG has no alpha; the few covers with transparency stay PNG
        gdk_pixbuf_save(j->pix, j->path, gdk_pixbuf_get_has_alpha(j->pix) ? "png" : "jpeg", NULL,
                        gdk_pixbuf_get_has_alpha(j->pix) ? NULL : "quality", "90", NULL);
      g_object_unref(in);
      g_bytes_unref(data);
    }
    g_idle_add(done, j);
  }
  return NULL;
}

void art_init(void (*cb)(void)) {
  changed = cb;
  int dpi = -1;  // Kindling's scale arrives as Xft.dpi; covers follow it like the text does
  g_object_get(gtk_settings_get_default(), "gtk-xft-dpi", &dpi, NULL);
  if (dpi > 0) art_size = CLAMP((int)(128 * dpi / 1024.0 / 96.0 + .5), 128, 384);
  dir = g_build_filename(g_get_user_cache_dir(), "ember", "covers", NULL);
  mem = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unref0);
  pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  jobs = g_async_queue_new();
  g_thread_unref(g_thread_new("art", worker, NULL));
}

// Loads a cover from the disk cache now, or queues it for the worker. Newest requests go
// first, so whatever is on screen right now beats what scrolled past.
void art_want_size(const char *key, const char *uri, int size) {
  char *mk = mem_key(key, size);
  if (g_hash_table_contains(mem, mk) || g_hash_table_contains(pending, mk)) { g_free(mk); return; }
  char *path = cache_path(key, size);
  GdkPixbuf *p = gdk_pixbuf_new_from_file(path, NULL);
  if (p) {
    g_hash_table_insert(mem, mk, p);
    g_free(path);
    return;
  }
  Job *j = g_new0(Job, 1);
  j->key = mk, j->uri = g_strdup(uri), j->path = path, j->size = size;
  g_hash_table_add(pending, g_strdup(mk));
  g_async_queue_push_front(jobs, j);
}

GdkPixbuf *art_peek_size(const char *key, int size) {
  char *mk = mem_key(key, size);
  GdkPixbuf *p = g_hash_table_lookup(mem, mk);
  g_free(mk);
  return p;
}

char *art_file(const char *key, int size) {
  char *p = cache_path(key, size);
  if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;
  g_free(p);
  return NULL;
}

GdkPixbuf *art_fit(GdkPixbuf *p, int n) {  // keeps the shape of covers that aren't square
  int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
  double k = (double)n / MAX(w, h);
  return gdk_pixbuf_scale_simple(p, MAX(1, w * k + .5), MAX(1, h * k + .5), GDK_INTERP_BILINEAR);
}

void art_want(const char *key, const char *uri) { art_want_size(key, uri, art_size); }
GdkPixbuf *art_peek(const char *key) { return art_peek_size(key, art_size); }

static gboolean not_kept(gpointer key, gpointer value, gpointer keep) {
  (void)value;
  char *k = g_strndup(key, strcspn(key, "\x1e"));
  gboolean drop = !g_hash_table_contains(keep, k);
  g_free(k);
  return drop;
}

void art_trim(GHashTable *keep) {  // 400 covers at 128 px is about 20 MB
  if (g_hash_table_size(mem) > 400) g_hash_table_foreach_remove(mem, not_kept, keep);
}
