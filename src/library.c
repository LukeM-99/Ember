// library.c: the Songs, Artists, Albums and Genres views, built from one pass over MPD's
// database and grouped here. Rows hold pointers into that data; a reload clears the views first.
#include "ember.h"
#include <stdlib.h>
#include <string.h>


static GPtrArray *songs, *artists, *albums, *genres;
static GHashTable *album_index, *artist_index;  // cover key -> album, name -> artist
static char *query;  // casefolded; NULL shows everything

static GtkListStore *song_store, *album_store;
static GtkWidget *song_view, *album_view, *card, *card_cover;
static Group *card_album;

typedef struct {     // the Artists and Genres pages: names on the left, their songs on the right
  GPtrArray **groups;
  GtkListStore *names;
  GtkTreeView *list;
  GtkTreeStore *detail;
} Browse;
static Browse by_artist = { .groups = &artists }, by_genre = { .groups = &genres };

static char *tag(const struct mpd_song *m, enum mpd_tag_type t, const char *unknown) {
  const char *v = mpd_song_get_tag(m, t, 0);
  return g_strdup(v && *v ? v : unknown);
}

// Case- and accent-folded, compared bytewise: spaces count ("A Flower" before "Above"),
// digits come before letters, like the iPod. Locale collation would ignore the spaces.
char *sort_key(const char *s) {
  char *f = g_utf8_casefold(s, -1), *k = g_utf8_normalize(f, -1, G_NORMALIZE_ALL);
  g_free(f);
  return k;
}

Song *song_new(const struct mpd_song *m) {
  Song *s = g_new0(Song, 1);
  s->uri = g_strdup(mpd_song_get_uri(m));
  s->title = tag(m, MPD_TAG_TITLE, NULL);
  if (!s->title) s->title = g_path_get_basename(s->uri);
  s->artist = tag(m, MPD_TAG_ARTIST, "Unknown Artist");
  s->albumartist = tag(m, MPD_TAG_ALBUM_ARTIST, s->artist);
  s->album = tag(m, MPD_TAG_ALBUM, "Unknown Album");
  s->genre = tag(m, MPD_TAG_GENRE, "Unknown Genre");
  const char *v;
  if ((v = mpd_song_get_tag(m, MPD_TAG_TRACK, 0))) s->track = atoi(v);  // "3" or "3/12"
  if ((v = mpd_song_get_tag(m, MPD_TAG_DISC, 0))) s->disc = atoi(v);
  if ((v = mpd_song_get_tag(m, MPD_TAG_DATE, 0))) s->year = atoi(v);    // "2003" or "2003-05-01"
  s->secs = mpd_song_get_duration(m);
  char *k = g_strjoin("\n", s->title, s->artist, s->album, NULL);
  s->key = g_utf8_casefold(k, -1);
  s->sort = sort_key(s->title);
  g_free(k);
  return s;
}

char *album_key(const struct mpd_song *m) {  // as group() builds it for albums
  char *artist = tag(m, MPD_TAG_ARTIST, "Unknown Artist"), *aa = tag(m, MPD_TAG_ALBUM_ARTIST, artist);
  char *album = tag(m, MPD_TAG_ALBUM, "Unknown Album"), *k = g_strconcat(aa, "\x1f", album, NULL);
  g_free(artist), g_free(aa), g_free(album);
  return k;
}

void song_free(gpointer p) {
  Song *s = p;
  g_free(s->uri), g_free(s->title), g_free(s->artist), g_free(s->album);
  g_free(s->albumartist), g_free(s->genre), g_free(s->key), g_free(s->sort);
  g_free(s);
}

static void group_free(gpointer p) {
  Group *g = p;
  g_free(g->name), g_free(g->key), g_free(g->sort), g_free(g->artist), g_free(g->art);
  g_ptr_array_unref(g->songs);
  g_free(g);
}

static int by_title(gconstpointer a, gconstpointer b) {
  return strcmp((*(Song **)a)->sort, (*(Song **)b)->sort);
}

static int by_name(gconstpointer a, gconstpointer b) {
  return strcmp((*(Group **)a)->sort, (*(Group **)b)->sort);
}

static int album_order(gconstpointer a, gconstpointer b) {  // year, album, disc, track
  const Song *x = *(Song **)a, *y = *(Song **)b;
  if (x->album_g != y->album_g) {
    if (x->album_g->year != y->album_g->year) return x->album_g->year < y->album_g->year ? -1 : 1;
    return strcmp(x->album_g->sort, y->album_g->sort);
  }
  if (x->disc != y->disc) return x->disc < y->disc ? -1 : 1;
  if (x->track != y->track) return x->track < y->track ? -1 : 1;
  return strcmp(x->sort, y->sort);
}

// Splits the songs into groups by name (albums: by album artist and album too), sorted by name.
static GPtrArray *group(size_t name_at, size_t artist_at) {
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *out = g_ptr_array_new_with_free_func(group_free);
  for (guint i = 0; i < songs->len; i++) {
    Song *s = songs->pdata[i];
    const char *name = *(char **)((char *)s + name_at);
    const char *artist = artist_at ? *(char **)((char *)s + artist_at) : "";
    char *id = g_strconcat(artist, "\x1f", name, NULL);
    Group *g = g_hash_table_lookup(seen, id);
    if (!g) {
      g = g_new0(Group, 1);
      g->name = g_strdup(name);
      g->key = g_utf8_casefold(name, -1);
      g->sort = sort_key(name);
      g->artist = artist_at ? g_strdup(artist) : NULL;
      g->art = artist_at ? g_strdup(id) : NULL;
      g->songs = g_ptr_array_new();
      g->year = s->year;
      g_hash_table_insert(seen, id, g);
      g_ptr_array_add(out, g);
    } else g_free(id);
    g_ptr_array_add(g->songs, s);
    g->secs += s->secs;
    if (artist_at) s->album_g = g;
  }
  g_hash_table_destroy(seen);
  g_ptr_array_sort(out, by_name);
  return out;
}

static void count_albums(GPtrArray *groups) {
  for (guint i = 0; i < groups->len; i++) {
    Group *g = groups->pdata[i];
    g_ptr_array_sort(g->songs, album_order);
    g->albums = 0;
    for (guint j = 0; j < g->songs->len; j++)
      if (!j || ((Song *)g->songs->pdata[j])->album_g != ((Song *)g->songs->pdata[j - 1])->album_g) g->albums++;
  }
}

static gboolean song_matches(Song *s) { return !query || strstr(s->key, query); }

static gboolean group_matches(Group *g) {
  if (!query || strstr(g->key, query)) return TRUE;
  for (guint i = 0; i < g->songs->len; i++) if (song_matches(g->songs->pdata[i])) return TRUE;
  return FALSE;
}

static char *fmt_secs(unsigned s) {
  return s >= 3600 ? g_strdup_printf("%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60) : g_strdup_printf("%u:%02u", s / 60, s % 60);
}

// ---- playing: like tapping a song on the iPod, the list it's in becomes the queue

static gboolean collect(GtkTreeModel *m, GtkTreePath *p, GtkTreeIter *it, gpointer list) {
  (void)p;
  Song *s;
  gtk_tree_model_get(m, it, COL_SONG, &s, -1);
  if (s) g_ptr_array_add(list, s);
  return FALSE;
}

void play_list(GPtrArray *list, guint at) {
  if (begin()) {
    mpd_run_clear(conn);
    for (guint i = 0; i < list->len && ok(); i += 500) {  // MPD caps a command list at 2 MB
      mpd_command_list_begin(conn, false);
      for (guint j = i; j < list->len && j < i + 500; j++) mpd_send_add(conn, ((Song *)list->pdata[j])->uri);
      mpd_command_list_end(conn);
      mpd_response_finish(conn);
    }
    if (ok()) mpd_run_play_pos(conn, at);
    end();
  }
}

static void play_from(GtkTreeModel *m, Song *target) {
  GPtrArray *list = g_ptr_array_new();
  gtk_tree_model_foreach(m, collect, list);
  guint at = 0;
  g_ptr_array_find(list, target, &at);
  play_list(list, at);
  g_ptr_array_unref(list);
}

Group *library_album(const char *key) { return album_index && key ? g_hash_table_lookup(album_index, key) : NULL; }
Group *library_artist(const char *name) { return artist_index && name ? g_hash_table_lookup(artist_index, name) : NULL; }

static void on_song(GtkTreeView *v, GtkTreePath *path, GtkTreeViewColumn *c, gpointer unused) {
  (void)c, (void)unused;
  GtkTreeModel *m = gtk_tree_view_get_model(v);
  GtkTreeIter it;
  Song *s;
  void (*act)(GtkTreeView *, GtkTreePath *) = g_object_get_data(G_OBJECT(v), "on-activate");
  if (act) { act(v, path); return; }
  gtk_tree_model_get_iter(m, &it, path);
  gtk_tree_model_get(m, &it, COL_SONG, &s, -1);
  if (s) play_from(m, s);
}

// ---- song lists (Songs page, artist and genre detail, album card)

static void song_cell(GtkTreeViewColumn *c, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer what) {
  (void)c;
  Song *s;
  Group *g;
  gtk_tree_model_get(m, it, COL_SONG, &s, COL_GROUP, &g, -1);
  char *text = NULL;
  switch (GPOINTER_TO_INT(what)) {
  case NUM: text = s && !g_strcmp0(s->uri, now_playing()) ? g_strdup("▶")  // as the iPod marked it
                 : s && s->track ? g_strdup_printf("%u", s->track) : NULL; break;
  case NAME: text = s ? g_strdup(s->title) : g->year ? g_strdup_printf("%s  (%u)", g->name, g->year) : g_strdup(g->name); break;
  case TIME: text = fmt_secs(s ? s->secs : g->secs); break;
  case ARTIST: text = g_strdup(s ? s->artist : ""); break;
  case ALBUM: text = g_strdup(s ? s->album : ""); break;
  }
  g_object_set(r, "text", text ? text : "", "weight", s ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD, NULL);
  g_free(text);
}

GtkWidget *scrolled(GtkWidget *child) {
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), child);
  return sw;
}

// ---- right-click: a menu for the selected songs (an album heading stands for its songs)

typedef struct { GPtrArray *list; GtkTreeSelection *sel; } Picked;

static void pick_row(GtkTreeModel *m, GtkTreePath *p, GtkTreeIter *it, gpointer data) {
  (void)p;
  Picked *k = data;
  Song *s;
  GtkTreeIter child;
  gtk_tree_model_get(m, it, COL_SONG, &s, -1);
  if (s) g_ptr_array_add(k->list, s);
  else for (gboolean more = gtk_tree_model_iter_children(m, &child, it); more; more = gtk_tree_model_iter_next(m, &child))
    if (!gtk_tree_selection_iter_is_selected(k->sel, &child)) {  // a selected song adds itself
      gtk_tree_model_get(m, &child, COL_SONG, &s, -1);
      g_ptr_array_add(k->list, s);
    }
}

static gboolean on_list_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
  (void)unused;
  GtkTreeView *v = GTK_TREE_VIEW(w);
  GtkTreePath *path;
  if (!gdk_event_triggers_context_menu((GdkEvent *)ev) ||
      !gtk_tree_view_get_path_at_pos(v, ev->x, ev->y, &path, NULL, NULL, NULL)) return GDK_EVENT_PROPAGATE;
  GtkTreeSelection *sel = gtk_tree_view_get_selection(v);
  if (!gtk_tree_selection_path_is_selected(sel, path)) {
    gtk_tree_selection_unselect_all(sel);
    gtk_tree_selection_select_path(sel, path);
  }
  gtk_tree_path_free(path);
  Picked k = { g_ptr_array_new(), sel };
  gtk_tree_selection_selected_foreach(sel, pick_row, &k);
  song_menu(k.list, (GdkEvent *)ev, w);
  g_ptr_array_unref(k.list);
  return GDK_EVENT_STOP;
}

GtkWidget *song_list(GtkTreeModel *m, const int *cols, int n, gboolean headers) {
  static const char *titles[] = { "", "Name", "Time", "Artist", "Album" };
  GtkWidget *v = gtk_tree_view_new_with_model(m);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(v), headers);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(v), FALSE);  // the search field does that
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(v), TRUE);  // keeps 50k-song lists quick
  for (int i = 0; i < n; i++) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 8, "ypad", 3, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[cols[i]], r, NULL);
    gtk_tree_view_column_set_cell_data_func(c, r, song_cell, GINT_TO_POINTER(cols[i]), NULL);
    gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
    if (cols[i] == NUM || cols[i] == TIME) {
      g_object_set(r, "xalign", 1.0, NULL);
      gtk_tree_view_column_set_alignment(c, 1.0);
      gtk_tree_view_column_set_fixed_width(c, cols[i] == NUM ? 44 : 72);
    } else {
      gtk_tree_view_column_set_expand(c, TRUE);
      gtk_tree_view_column_set_fixed_width(c, 120);
    }
    gtk_tree_view_append_column(GTK_TREE_VIEW(v), c);
  }
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(v)), GTK_SELECTION_MULTIPLE);
  g_signal_connect(v, "row-activated", G_CALLBACK(on_song), NULL);
  g_signal_connect(v, "button-press-event", G_CALLBACK(on_list_press), NULL);
  return v;
}

// ---- Artists and Genres: pick a name, see its albums and songs

static void show_detail(GtkTreeSelection *sel, Browse *b) {
  GtkTreeModel *m;
  GtkTreeIter it;
  gtk_tree_store_clear(b->detail);
  if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
  Group *g;
  gtk_tree_model_get(m, &it, COL_GROUP, &g, -1);
  GtkTreeIter head;
  for (guint i = 0; i < g->songs->len; i++) {
    Song *s = g->songs->pdata[i];
    if (!i || s->album_g != ((Song *)g->songs->pdata[i - 1])->album_g)
      gtk_tree_store_insert_with_values(b->detail, &head, NULL, -1, COL_SONG, NULL, COL_GROUP, s->album_g, -1);
    gtk_tree_store_insert_with_values(b->detail, NULL, &head, -1, COL_SONG, s, COL_GROUP, s->album_g, -1);
  }
  GtkTreeView *dv = g_object_get_data(G_OBJECT(b->list), "detail");
  gtk_tree_view_expand_all(dv);
  if (gtk_widget_get_realized(GTK_WIDGET(dv))) gtk_tree_view_scroll_to_point(dv, 0, 0);
}

static void name_cell(GtkTreeViewColumn *c, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer unused) {
  (void)c, (void)unused;
  Group *g;
  gtk_tree_model_get(m, it, COL_GROUP, &g, -1);
  char *mk = g_markup_printf_escaped("%s\n<span size='small' alpha='60%%'>%u album%s, %u song%s</span>",
    g->name, g->albums, g->albums == 1 ? "" : "s", g->songs->len, g->songs->len == 1 ? "" : "s");
  g_object_set(r, "markup", mk, NULL);
  g_free(mk);
}

static gboolean on_name_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
  (void)unused;
  GtkTreeView *v = GTK_TREE_VIEW(w);
  GtkTreePath *path;
  GtkTreeIter it;
  Group *g;
  if (!gdk_event_triggers_context_menu((GdkEvent *)ev) ||
      !gtk_tree_view_get_path_at_pos(v, ev->x, ev->y, &path, NULL, NULL, NULL)) return GDK_EVENT_PROPAGATE;
  gtk_tree_selection_select_path(gtk_tree_view_get_selection(v), path);
  gtk_tree_model_get_iter(gtk_tree_view_get_model(v), &it, path);
  gtk_tree_model_get(gtk_tree_view_get_model(v), &it, COL_GROUP, &g, -1);
  gtk_tree_path_free(path);
  song_menu(g->songs, (GdkEvent *)ev, w);
  return GDK_EVENT_STOP;
}

static GtkWidget *browse_page(Browse *b) {
  b->names = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  GtkWidget *list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(b->names));
  b->list = GTK_TREE_VIEW(list);
  gtk_tree_view_set_headers_visible(b->list, FALSE);
  gtk_tree_view_set_enable_search(b->list, FALSE);
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 8, "ypad", 4, NULL);
  GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes("", r, NULL);
  gtk_tree_view_column_set_cell_data_func(c, r, name_cell, NULL, NULL);
  gtk_tree_view_append_column(b->list, c);
  g_signal_connect(list, "button-press-event", G_CALLBACK(on_name_press), NULL);

  b->detail = gtk_tree_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  static const int cols[] = { NUM, NAME, TIME };
  GtkWidget *dv = song_list(GTK_TREE_MODEL(b->detail), cols, 3, FALSE);
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(dv), FALSE);  // headings are taller
  gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(dv), FALSE);
  gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(dv), 0);
  g_object_set_data(G_OBJECT(list), "detail", dv);
  g_signal_connect(gtk_tree_view_get_selection(b->list), "changed", G_CALLBACK(show_detail), b);

  GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *left = scrolled(list);
  gtk_widget_set_size_request(left, 240, -1);
  gtk_paned_pack1(GTK_PANED(pane), left, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(pane), scrolled(dv), TRUE, FALSE);
  return pane;
}

// ---- Albums: a grid of covers; clicking one opens its songs on a card over the grid

GdkPixbuf *cover_placeholder(void) {  // for albums without a cover; grey reads on dark and light
  static GdkPixbuf *p;
  if (p) return p;
  int n = art_size;
  cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, n, n);
  cairo_t *cr = cairo_create(cs);
  cairo_set_source_rgba(cr, .5, .5, .5, .18);
  cairo_paint(cr);
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, "Sans");
  pango_font_description_set_absolute_size(fd, n * .45 * PANGO_SCALE);
  pango_layout_set_font_description(l, fd);
  pango_layout_set_text(l, "♪", -1);
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  cairo_move_to(cr, (n - w) / 2.0, (n - h) / 2.0);
  cairo_set_source_rgba(cr, .5, .5, .5, .45);
  pango_cairo_show_layout(cr, l);
  pango_font_description_free(fd);
  g_object_unref(l);
  cairo_destroy(cr);
  p = gdk_pixbuf_get_from_surface(cs, 0, 0, n, n);
  cairo_surface_destroy(cs);
  return p;
}

static GdkPixbuf *cover(Group *g) {
  GdkPixbuf *p = art_peek(g->art);
  return p ? p : cover_placeholder();
}

static void cover_cell(GtkCellLayout *l, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer unused) {
  (void)l, (void)unused;
  Group *g;
  gtk_tree_model_get(m, it, COL_GROUP, &g, -1);
  g_object_set(r, "pixbuf", cover(g), NULL);
}

// Asks for the covers on screen (and the next rows), once scrolling or resizing settles.
static guint art_idle;

static gboolean want_visible(gpointer unused) {
  (void)unused;
  art_idle = 0;
  GtkTreePath *a, *b;
  if (!gtk_icon_view_get_visible_range(GTK_ICON_VIEW(album_view), &a, &b)) return G_SOURCE_REMOVE;
  GtkTreeModel *m = GTK_TREE_MODEL(album_store);
  int from = gtk_tree_path_get_indices(a)[0], to = gtk_tree_path_get_indices(b)[0] + 16;
  gtk_tree_path_free(a), gtk_tree_path_free(b);
  GHashTable *keep = g_hash_table_new(g_str_hash, g_str_equal);
  GtkTreeIter it;
  for (int i = to; i >= from; i--)  // bottom first: the worker takes the newest request first
    if (gtk_tree_model_iter_nth_child(m, &it, NULL, i)) {
      Group *g;
      gtk_tree_model_get(m, &it, COL_GROUP, &g, -1);
      art_want(g->art, ((Song *)g->songs->pdata[0])->uri);
      g_hash_table_add(keep, g->art);
    }
  if (card_album) g_hash_table_add(keep, card_album->art);
  if (mini_album()) g_hash_table_add(keep, (char *)mini_album());
  art_trim(keep);
  g_hash_table_destroy(keep);
  gtk_widget_queue_draw(album_view);
  return G_SOURCE_REMOVE;
}

static void want_art(void) {
  if (!art_idle) art_idle = g_timeout_add(60, want_visible, NULL);
}

static void art_arrived(void) {
  gtk_widget_queue_draw(album_view);
  mini_art_changed();
  mpris_art_changed();
  focus_art_changed();
  if (card_album) gtk_image_set_from_pixbuf(GTK_IMAGE(card_cover), cover(card_album));
}

static gboolean on_album_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
  (void)unused;
  GtkIconView *v = GTK_ICON_VIEW(w);
  GtkTreePath *path;
  GtkTreeIter it;
  Group *g;
  if (!gdk_event_triggers_context_menu((GdkEvent *)ev) || !(path = gtk_icon_view_get_path_at_pos(v, ev->x, ev->y)))
    return GDK_EVENT_PROPAGATE;
  gtk_icon_view_unselect_all(v);
  gtk_icon_view_select_path(v, path);
  gtk_tree_model_get_iter(GTK_TREE_MODEL(album_store), &it, path);
  gtk_tree_model_get(GTK_TREE_MODEL(album_store), &it, COL_GROUP, &g, -1);
  gtk_tree_path_free(path);
  song_menu(g->songs, (GdkEvent *)ev, w);
  return GDK_EVENT_STOP;
}

static void album_cell(GtkCellLayout *l, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer artist) {
  (void)l;
  Group *g;
  gtk_tree_model_get(m, it, COL_GROUP, &g, -1);
  g_object_set(r, "text", artist ? g->artist : g->name, NULL);
}

static void on_album(GtkIconView *v, GtkTreePath *path, gpointer unused) {
  (void)unused;
  GtkTreeIter it;
  Group *g;
  gtk_tree_model_get_iter(GTK_TREE_MODEL(album_store), &it, path);
  gtk_tree_model_get(GTK_TREE_MODEL(album_store), &it, COL_GROUP, &g, -1);

  GtkListStore *tracks = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  for (guint i = 0; i < g->songs->len; i++)
    gtk_list_store_insert_with_values(tracks, NULL, -1, COL_SONG, g->songs->pdata[i], COL_GROUP, g, -1);
  static const int cols[] = { NUM, NAME, TIME };
  GtkWidget *list = song_list(GTK_TREE_MODEL(tracks), cols, 3, FALSE);
  g_object_unref(tracks);

  char *yr = g->year ? g_strdup_printf(" · %u", g->year) : g_strdup("");
  char *len = fmt_secs(g->secs);
  char *mk = g_markup_printf_escaped("<big><b>%s</b></big>\n<span alpha='60%%'>%s%s · %u song%s, %s</span>",
    g->name, g->artist, yr, g->songs->len, g->songs->len == 1 ? "" : "s", len);
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), mk);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  g_free(yr), g_free(len), g_free(mk);
  card_cover = gtk_image_new_from_pixbuf(cover(g));
  card_album = g;
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_pack_start(GTK_BOX(head), card_cover, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(head), label, TRUE, TRUE, 0);

  GtkWidget *sw = scrolled(list);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 420);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  g_object_set(box, "margin", 12, NULL);
  gtk_widget_set_size_request(box, 460, -1);
  gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

  if (card) gtk_widget_destroy(card);
  card_album = g;  // the old card's destroy cleared it
  card = gtk_popover_new(GTK_WIDGET(v));
  g_signal_connect(card, "destroy", G_CALLBACK(gtk_widget_destroyed), &card);
  g_signal_connect_swapped(card, "destroy", G_CALLBACK(g_nullify_pointer), &card_album);
  g_signal_connect(card, "closed", G_CALLBACK(gtk_widget_destroy), NULL);
  GdkRectangle rect;
  gtk_icon_view_get_cell_rect(v, path, NULL, &rect);
  gtk_popover_set_pointing_to(GTK_POPOVER(card), &rect);
  gtk_container_add(GTK_CONTAINER(card), box);
  gtk_widget_show_all(box);
  gtk_popover_popup(GTK_POPOVER(card));
  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)));  // focus picks row 1
}

static GtkWidget *albums_page(void) {
  album_store = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  album_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(album_store));
  GtkIconView *v = GTK_ICON_VIEW(album_view);
  gtk_icon_view_set_item_width(v, art_size);
  gtk_icon_view_set_item_padding(v, 4);
  gtk_icon_view_set_column_spacing(v, 20);
  gtk_icon_view_set_row_spacing(v, 16);
  gtk_icon_view_set_margin(v, 20);
  gtk_icon_view_set_activate_on_single_click(v, TRUE);
  GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
  gtk_cell_renderer_set_fixed_size(pix, art_size, art_size);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), pix, FALSE);
  gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v), pix, cover_cell, NULL, NULL);
  // two lines under each cover: album, then artist, each cut short rather than wrapped
  for (int i = 0; i < 2; i++) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xalign", 0.0, "width", art_size,
                 "weight", i ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD, NULL);
    if (i) g_object_set(r, "scale", 0.9, "foreground-rgba", &(GdkRGBA){ .5, .5, .5, 1 }, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(v), r, FALSE);
    gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(v), r, album_cell, GINT_TO_POINTER(i), NULL);
  }
  g_signal_connect(v, "item-activated", G_CALLBACK(on_album), NULL);
  g_signal_connect(v, "button-press-event", G_CALLBACK(on_album_press), NULL);
  g_signal_connect(v, "size-allocate", G_CALLBACK(want_art), NULL);
  GtkWidget *sw = scrolled(album_view);
  g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)), "value-changed", G_CALLBACK(want_art), NULL);
  art_init(art_arrived);
  return sw;
}

// ---- loading and searching

// Refills a view with the items matching the search. The view is detached meanwhile, so it
// redraws once rather than per row. (GtkTreeModelFilter is the usual tool, but GTK3's warns
// and misbehaves when a search removes the selected row.)
static void refill(GtkWidget *view, GtkListStore *store, GPtrArray *items, gboolean groups) {
  gboolean icons = GTK_IS_ICON_VIEW(view);
  if (icons) gtk_icon_view_set_model(GTK_ICON_VIEW(view), NULL);
  else gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(store);
  for (guint i = 0; items && i < items->len; i++) {
    gpointer p = items->pdata[i];
    if (groups ? !group_matches(p) : !song_matches(p)) continue;
    gtk_list_store_insert_with_values(store, NULL, -1, groups ? COL_GROUP : COL_SONG, p, -1);
  }
  if (icons) gtk_icon_view_set_model(GTK_ICON_VIEW(view), GTK_TREE_MODEL(store));
  else gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(store));
}

static void refill_browse(Browse *b) {  // keeps the selected name if it still matches, else the first
  GtkTreeModel *m = GTK_TREE_MODEL(b->names);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(b->list);
  GtkTreeIter it, first;
  Group *was = NULL, *g;
  if (gtk_tree_selection_get_selected(sel, NULL, &it)) gtk_tree_model_get(m, &it, COL_GROUP, &was, -1);
  refill(GTK_WIDGET(b->list), b->names, *b->groups, TRUE);
  if (!gtk_tree_model_get_iter_first(m, &first)) return;
  for (it = first; ; ) {
    gtk_tree_model_get(m, &it, COL_GROUP, &g, -1);
    if (g == was) break;
    if (!gtk_tree_model_iter_next(m, &it)) { it = first; break; }
  }
  gtk_tree_selection_select_iter(sel, &it);
  GtkTreePath *path = gtk_tree_model_get_path(m, &it);
  gtk_tree_view_scroll_to_cell(b->list, path, NULL, FALSE, 0, 0);
  gtk_tree_path_free(path);
}

static void fill(void) {
  refill(song_view, song_store, songs, FALSE);
  refill(album_view, album_store, albums, TRUE);
  refill_browse(&by_artist);
  refill_browse(&by_genre);
}

void library_load(void) {
  if (!begin()) return;
  GPtrArray *all = g_ptr_array_new_with_free_func(song_free);
  for (unsigned start = 0;; start += 1000) {  // 1000 at a time, so MPD's output buffer never fills
    guint before = all->len;
    if (!mpd_search_db_songs(conn, true) || !mpd_search_add_expression(conn, "(base '')") ||
        !mpd_search_add_window(conn, start, start + 1000) || !mpd_search_commit(conn)) break;
    struct mpd_song *m;
    while ((m = mpd_recv_song(conn))) g_ptr_array_add(all, song_new(m)), mpd_song_free(m);
    if (!mpd_response_finish(conn) || all->len - before < 1000) break;
  }
  if (!ok()) { g_ptr_array_unref(all); end(); return; }  // keep showing the old library
  end();

  // views first: their rows point into the old data
  if (card) gtk_widget_destroy(card);
  for (GtkListStore **s = (GtkListStore *[]){ song_store, album_store, by_artist.names, by_genre.names, NULL }; *s; s++)
    gtk_list_store_clear(*s);
  gtk_tree_store_clear(by_artist.detail), gtk_tree_store_clear(by_genre.detail);
  focus_detach();  // its pages point into the old data too
  if (album_index) g_hash_table_destroy(album_index), g_hash_table_destroy(artist_index);
  if (genres) g_ptr_array_unref(genres), g_ptr_array_unref(artists), g_ptr_array_unref(albums), g_ptr_array_unref(songs);

  songs = all;
  g_ptr_array_sort(songs, by_title);
  albums = group(G_STRUCT_OFFSET(Song, album), G_STRUCT_OFFSET(Song, albumartist));
  artists = group(G_STRUCT_OFFSET(Song, albumartist), 0);
  genres = group(G_STRUCT_OFFSET(Song, genre), 0);
  count_albums(albums), count_albums(artists), count_albums(genres);
  album_index = g_hash_table_new(g_str_hash, g_str_equal);
  artist_index = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < albums->len; i++) g_hash_table_insert(album_index, ((Group *)albums->pdata[i])->art, albums->pdata[i]);
  for (guint i = 0; i < artists->len; i++) g_hash_table_insert(artist_index, ((Group *)artists->pdata[i])->name, artists->pdata[i]);
  fill();
  focus_attach();
}

void library_search(const char *q) {
  g_free(query);
  query = q && *q ? g_utf8_casefold(q, -1) : NULL;
  fill();
}

GtkWidget *library_new(void) {
  song_store = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  static const int cols[] = { NAME, TIME, ARTIST, ALBUM };

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_add_titled(GTK_STACK(stack), playlists_page(), "playlists", "Playlists");
  gtk_stack_add_titled(GTK_STACK(stack), scrolled(song_view = song_list(GTK_TREE_MODEL(song_store), cols, 4, TRUE)), "songs", "Songs");
  gtk_stack_add_titled(GTK_STACK(stack), browse_page(&by_artist), "artists", "Artists");
  gtk_stack_add_titled(GTK_STACK(stack), albums_page(), "albums", "Albums");
  gtk_stack_add_titled(GTK_STACK(stack), browse_page(&by_genre), "genres", "Genres");
  focus_pages(GTK_STACK(stack));  // untitled, so they get no tab
  return stack;
}
