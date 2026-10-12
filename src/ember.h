// ember.h: what the files share.
#include <gtk/gtk.h>
#include <mpd/client.h>

// ember.c: the MPD connection. Wrap commands in begin()/end() (they nest); NULL while disconnected.
extern struct mpd_connection *conn;
gboolean ok(void);
gboolean begin(void);
void end(void);
struct mpd_connection *mpd_open(void);  // another connection to the same MPD, or NULL

// library.c: the Songs, Artists, Albums and Genres views.
typedef struct Group Group;
struct Group {         // an artist, album or genre
  char *name, *key, *sort;
  char *artist, *art;  // albums only: album artist, cover key
  GPtrArray *songs;    // Song *, in album order
  unsigned secs, albums, year;
};
typedef struct {
  char *uri, *title, *artist, *album, *albumartist, *genre;
  char *key, *sort;  // casefolded title/artist/album for search; sort key of the title
  unsigned track, disc, secs, year;
  Group *album_g;    // NULL outside the library (playlists)
} Song;

enum { COL_SONG, COL_GROUP };             // list rows: a Song, or (album headings) only a Group
enum { NUM, NAME, TIME, ARTIST, ALBUM };  // song_list columns
GtkWidget *library_new(void);             // a GtkStack, one page per view
void library_load(void);                  // re-reads MPD's database
void library_search(const char *q);       // filters every view
Song *song_new(const struct mpd_song *m);
void song_free(gpointer song);
GtkWidget *song_list(GtkTreeModel *m, const int *cols, int n, gboolean headers);
GtkWidget *scrolled(GtkWidget *child);
char *sort_key(const char *s);
char *album_key(const struct mpd_song *m);  // the cover key: album artist and album
const char *song_tag(const struct mpd_song *m, enum mpd_tag_type t);  // NULL if missing or empty
Group *library_album(const char *key);      // NULL if it isn't in the library
Group *library_artist(const char *name);    // by album artist
void play_list(GPtrArray *songs, guint at, gboolean shuffle);  // replaces the queue and plays songs[at]
char *fmt_secs(unsigned s);                  // "3:07", "1:02:09"
GdkPixbuf *cover_placeholder(void);

// A song_list can carry, as object data: "on-activate", a void (*)(GtkTreeView *, GtkTreePath *)
// run on double-click instead of playing the list; "on-delete", run by the Delete key with
// (NULL, NULL), for lists songs can be removed from; "menu-extra", a void (*)(GtkWidget *menu,
// guint n_songs) that adds its own items to the right-click menu.

// playlists.c: the Playlists tab and the right-click menu.
GtkWidget *playlists_page(void);
void playlists_load(void);
void song_menu(GPtrArray *songs, GdkEvent *ev, GtkWidget *from);  // songs: Song *, borrowed
char *ask_name(const char *title, const char *initial, const char *ok_label);  // NULL: cancelled

// queue.c: Up Next.
GtkWidget *queue_button(void);  // the top bar's button; its card holds the list
void queue_load(void);          // refreshes the list, if it is open

// focus.c: the album and artist pages.
void focus_pages(GtkStack *stack);
void focus_album(const char *key);    // shows the album page
void focus_artist(const char *name);
void focus_detach(void);              // before the library reloads
void focus_attach(void);              // after
void focus_art_changed(void);
GtkWidget *play_buttons(Group *g);   // Play and Shuffle

// mini.c: the mini and micro players.
void mini_init(GtkApplication *app, GtkWidget *main_window);
void mini_toggle(void);                                  // full <-> mini
gboolean mini_present(void);                             // raises it, if one is showing
void mini_song(const char *title, const char *sub, const struct mpd_song *song);  // song may be NULL
void mini_progress(gboolean playing, double pos, unsigned duration);
void mini_art_changed(void);
void mini_save(void);                                    // remembers where they were left
const char *mini_album(void);                            // its cover key, or NULL

// ember.c: player actions, for the mini player's buttons.
const char *now_playing(void);  // the playing song's file, or NULL
void player_toggle(void);
void player_prev(void);
void player_next(void);
void player_shuffle(GtkToggleButton *b);
gboolean player_scrub(GtkRange *r, GtkScrollType s, double v, gpointer unused);
gboolean player_key(GdkEventKey *ev, gboolean typing);  // the shared shortcuts; TRUE if handled
GtkWidget *icon_button(const char *icon, const char *tip, GtkIconSize size, GCallback cb);
GtkWidget *text_button(const char *text, const char *cls, GCallback cb, gpointer data);  // cb(data)

// mpris.c: MPRIS on the session bus, for playerctl and media keys.
void mpris_init(GApplication *app);
void mpris_update(const struct mpd_status *st, const struct mpd_song *song);  // st NULL: disconnected
void mpris_art_changed(void);

// art.c: album covers, fetched in the background and cached on disk.
extern int art_size;                      // pixels; 128 at 96 dpi
void art_init(void (*changed)(void));     // changed: runs when a cover arrives
void art_want(const char *key, const char *uri);
GdkPixbuf *art_peek(const char *key);     // NULL until it arrives (or if there is none)
void art_want_size(const char *key, const char *uri, int size);
GdkPixbuf *art_peek_size(const char *key, int size);
char *art_file(const char *key, int size);  // the cached file, if there is one
GdkPixbuf *art_fit(GdkPixbuf *p, int size); // scaled to fit a size x size square (new reference)
void art_trim(GHashTable *keep);          // drops covers not in keep, once there are many

// theme.c
void theme_init(void);
void theme_reload(void);

// settings.c: ~/.config/ember/settings.
void settings_init(void);
char *settings_string(const char *key);  // NULL if unset
void settings_set_string(const char *key, const char *v);
gboolean settings_get(const char *key);  // keep-playing, discord, cover-lookup: off unless set
const char *settings_style(void);     // "flat" or "classic"

// discord.c: Rich Presence. coverart.c: cover URLs from the internet, for it. Both off by default.
void discord_update(const struct mpd_song *song, gboolean playing, double elapsed, unsigned duration);
void discord_enable(gboolean on);
void discord_art_found(void);
void coverart_init(void (*ready)(void));
const char *coverart_url(const char *key, const char *artist, const char *album);  // NULL: not known (yet)
GtkWidget *settings_button(void);     // the top bar's menu
