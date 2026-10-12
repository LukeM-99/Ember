// focus.c: the album and artist pages, opened from the names in the top bar or a right-click.
//   album:  a large cover on the left with the details and Play/Shuffle; the songs on the right.
//   artist: each album down the page, its cover on the left and its songs beside it.
#include "ember.h"
#include <string.h>

typedef struct { GtkWidget *img; char *key; int size; } Cover;

static GtkStack *stack;
static GtkWidget *album_page, *artist_page;
static char *shown_album, *shown_artist;  // what they show, to rebuild after a library reload
static char *back_to;                     // the tab to go back to
static GPtrArray *covers;                 // on the pages now, so arriving art can fill them in

static void cover_free(gpointer p) { Cover *c = p; g_free(c->key), g_free(c); }

static void set_cover(Cover *c) {
  GdkPixbuf *p = art_peek_size(c->key, c->size);
  if (!p) p = art_peek(c->key);  // the grid's smaller one, until the big one arrives
  if (!p) p = cover_placeholder();
  GdkPixbuf *f = art_fit(p, c->size);
  gtk_image_set_from_pixbuf(GTK_IMAGE(c->img), f);
  g_object_unref(f);
}

static GtkWidget *cover(Group *g, int size) {
  Cover *c = g_new0(Cover, 1);
  c->img = gtk_image_new();
  c->key = g_strdup(g->art), c->size = size;
  gtk_widget_set_size_request(c->img, size, size);
  gtk_widget_set_valign(c->img, GTK_ALIGN_START);
  art_want_size(g->art, ((Song *)g->songs->pdata[0])->uri, size);
  set_cover(c);
  g_ptr_array_add(covers, c);
  return c->img;
}

void focus_art_changed(void) {
  for (guint i = 0; covers && i < covers->len; i++) set_cover(covers->pdata[i]);
}

static void clear(GtkWidget *page) {  // covers first: they point at the images being destroyed
  g_ptr_array_set_size(covers, 0);
  GList *kids = gtk_container_get_children(GTK_CONTAINER(page));
  for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
  g_list_free(kids);
}

// ---- pieces

static GtkWidget *label(const char *markup, const char *cls) {
  GtkWidget *l = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(l), markup);
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(l), 30);
  if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
  return l;
}

static void on_back(void) { gtk_stack_set_visible_child_name(stack, back_to ? back_to : "albums"); }

static void play(Group *g) { play_list(g->songs, 0, FALSE); }
static void shuffle(Group *g) { play_list(g->songs, 0, TRUE); }

GtkWidget *play_buttons(Group *g) {
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(row), text_button("Play", "pill", G_CALLBACK(play), g), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), text_button("Shuffle", "pill", G_CALLBACK(shuffle), g), FALSE, FALSE, 0);
  return row;
}

static GtkWidget *tracks(GPtrArray *list, guint from, guint to) {
  GtkListStore *store = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  for (guint i = from; i < to; i++) gtk_list_store_insert_with_values(store, NULL, -1, COL_SONG, list->pdata[i], -1);
  static const int cols[] = { NUM, NAME, TIME };
  GtkWidget *v = song_list(GTK_TREE_MODEL(store), cols, 3, FALSE);
  g_object_unref(store);
  return v;
}

static char *details(Group *g) {  // "2014 · 12 songs, 49 min"
  char *yr = g->year ? g_strdup_printf("%u · ", g->year) : g_strdup("");
  char *d = g_strdup_printf("%s%u song%s, %u min", yr, g->songs->len, g->songs->len == 1 ? "" : "s", (g->secs + 30) / 60);
  g_free(yr);
  return d;
}

// ---- pages

static void build_album(Group *g) {
  clear(album_page);
  int big = art_size * 5 / 2;  // 320 px at 96 dpi
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(left, big, -1);
  gtk_box_pack_start(GTK_BOX(left), text_button("‹ Back", "back", G_CALLBACK(on_back), NULL), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left), cover(g, big), FALSE, FALSE, 6);
  char *t = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", g->name);
  gtk_box_pack_start(GTK_BOX(left), label(t, NULL), FALSE, FALSE, 0);
  GtkWidget *artist = text_button(g->artist, "link", G_CALLBACK(focus_artist), g->artist);
  gtk_box_pack_start(GTK_BOX(left), artist, FALSE, FALSE, 0);
  char *d = details(g), *de = g_markup_escape_text(d, -1);
  gtk_box_pack_start(GTK_BOX(left), label(de, "dim"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left), play_buttons(g), FALSE, FALSE, 10);
  g_free(t), g_free(d), g_free(de);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
  g_object_set(row, "margin", 24, NULL);
  gtk_box_pack_start(GTK_BOX(row), left, FALSE, FALSE, 0);
  GtkWidget *list = tracks(g->songs, 0, g->songs->len);
  gtk_widget_set_valign(list, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), list, TRUE, TRUE, 0);
  // the page scrolls as a whole, so the big cover never forces the window taller than the screen
  gtk_box_pack_start(GTK_BOX(album_page), scrolled(row), TRUE, TRUE, 0);
  gtk_widget_show_all(album_page);
}

static void build_artist(Group *a) {
  clear(artist_page);
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  g_object_set(head, "margin", 16, "margin-start", 24, "margin-end", 24, NULL);
  GtkWidget *back = text_button("‹ Back", "back", G_CALLBACK(on_back), NULL);
  gtk_widget_set_valign(back, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(head), back, FALSE, FALSE, 0);
  GtkWidget *names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  char *t = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", a->name);
  char *c = g_strdup_printf("%u album%s, %u song%s", a->albums, a->albums == 1 ? "" : "s", a->songs->len, a->songs->len == 1 ? "" : "s");
  gtk_box_pack_start(GTK_BOX(names), label(t, NULL), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(names), label(c, "dim"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(head), names, TRUE, TRUE, 0);
  GtkWidget *buttons = play_buttons(a);
  gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
  gtk_box_pack_end(GTK_BOX(head), buttons, FALSE, FALSE, 0);
  g_free(t), g_free(c);

  int mid = art_size * 3 / 2;  // 192 px, the micro player's size, so it's cached once
  GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 32);
  g_object_set(list, "margin", 24, "margin-top", 8, NULL);
  for (guint i = 0, j; i < a->songs->len; i = j) {  // a->songs is in album order: one run per album
    Group *g = ((Song *)a->songs->pdata[i])->album_g;
    for (j = i; j < a->songs->len && ((Song *)a->songs->pdata[j])->album_g == g; j++) ;
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *title = text_button(g->name, "albumlink", G_CALLBACK(focus_album), g->art);
    char *d = details(g);
    gtk_box_pack_start(GTK_BOX(info), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), label(d, "dim"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info), tracks(a->songs, i, j), FALSE, FALSE, 6);
    g_free(d);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_box_pack_start(GTK_BOX(row), cover(g, mid), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), info, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(list), row, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(artist_page), head, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(artist_page), scrolled(list), TRUE, TRUE, 0);
  gtk_widget_show_all(artist_page);
}

static void go(const char *page) {
  const char *now = gtk_stack_get_visible_child_name(stack);
  if (now && strcmp(now, "album") && strcmp(now, "artist")) g_free(back_to), back_to = g_strdup(now);
  gtk_stack_set_visible_child_name(stack, page);
}

void focus_album(const char *key) {
  Group *g = library_album(key);
  if (!g) return;
  char *k = g_strdup(key);  // key may be shown_album itself
  g_free(shown_album), shown_album = k;
  build_album(g);
  go("album");
}

void focus_artist(const char *name) {
  Group *a = library_artist(name);
  if (!a) return;
  char *n = g_strdup(name);
  g_free(shown_artist), shown_artist = n;
  build_artist(a);
  go("artist");
}

void focus_detach(void) {
  if (!stack) return;
  clear(album_page);
  clear(artist_page);
}

void focus_attach(void) {  // rebuild what was showing; if it's gone from the library, go back
  const char *now = stack ? gtk_stack_get_visible_child_name(stack) : NULL;
  Group *g;
  if (!g_strcmp0(now, "album")) { if ((g = library_album(shown_album))) build_album(g); else on_back(); }
  if (!g_strcmp0(now, "artist")) { if ((g = library_artist(shown_artist))) build_artist(g); else on_back(); }
}

void focus_pages(GtkStack *s) {
  stack = s;
  covers = g_ptr_array_new_with_free_func(cover_free);
  album_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  artist_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_stack_add_named(stack, album_page, "album");
  gtk_stack_add_named(stack, artist_page, "artist");
}
