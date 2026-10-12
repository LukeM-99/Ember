// ember: an iPod-style GTK3 frontend for MPD.
// The connection sits in MPD's `idle` mode on a GLib IO watch; commands leave idle,
// run, refresh the UI and go back. Nothing polls MPD.
#include "ember.h"
#include <string.h>

struct mpd_connection *conn;
static guint watch, ticker, seeker, retry;
static gboolean idling;  // conn is in MPD's idle mode
static int depth;        // begin()s not yet end()ed
static GtkWidget *win, *title, *subtitle, *play_icon, *scrub, *elapsed_l, *remain_l, *volume, *stack, *shuffle, *repeat;
static gboolean repeat_on, single_on;  // MPD's repeat and single modes: Repeat All, Repeat One

static enum mpd_state state;
static unsigned duration, queue_len;  // seconds, songs
static double elapsed;                // seconds, as of elapsed_at (monotonic usec)
static gint64 elapsed_at;
static double seek_to = -1;
static char *now_album, *now_artist;  // the playing song's album (cover key) and album artist, for the links
static char *now_uri;                 // the playing song's file, for the ▶ in song lists

const char *now_playing(void) { return now_uri; }           // pending scrubber position while the user drags

static gboolean reconnect(gpointer);

static void disconnect(const char *why) {  // why may belong to conn, so use it before freeing
  gtk_label_set_markup(GTK_LABEL(title), "<b>Not connected to MPD</b>");
  gtk_label_set_text(GTK_LABEL(subtitle), why);
  gtk_label_set_text(GTK_LABEL(elapsed_l), "");
  gtk_label_set_text(GTK_LABEL(remain_l), "");
  gtk_image_set_from_icon_name(GTK_IMAGE(play_icon), "media-playback-start-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_sensitive(scrub, FALSE);
  gtk_window_set_title(GTK_WINDOW(win), "Ember");
  mini_song("Not connected to MPD", why, NULL);
  mini_progress(FALSE, 0, 0);
  mpris_update(NULL, NULL);
  discord_update(NULL, FALSE, 0, 0);
  if (now_uri) { g_clear_pointer(&now_uri, g_free); gtk_widget_queue_draw(win); }  // no ▶ anywhere
  state = MPD_STATE_UNKNOWN;
  if (watch) g_source_remove(watch);
  if (ticker) g_source_remove(ticker);
  watch = ticker = 0;
  idling = FALSE, depth = 0;
  if (conn) mpd_connection_free(conn);
  conn = NULL;
  if (!retry) retry = g_timeout_add_seconds(2, reconnect, NULL);
}

// TRUE if the last call worked. Server errors (bad seek, empty database) keep the
// connection; anything else drops it and starts reconnecting.
gboolean ok(void) {
  if (!conn || mpd_connection_get_error(conn) == MPD_ERROR_SUCCESS) return conn != NULL;
  const char *msg = mpd_connection_get_error_message(conn);
  g_message("mpd: %s", msg);
  if (!mpd_connection_clear_error(conn)) disconnect(msg);
  return FALSE;
}

// Moves the scrubber and the time labels to the current position; runs every second while playing.
static gboolean tick(gpointer unused) {
  (void)unused;
  double pos = seek_to >= 0 ? seek_to
    : elapsed + (state == MPD_STATE_PLAY ? (g_get_monotonic_time() - elapsed_at) / 1e6 : 0);
  if (duration && pos > duration) pos = duration;
  if (seek_to < 0) gtk_range_set_value(GTK_RANGE(scrub), pos);
  char *e = fmt_secs(pos), *r = fmt_secs(duration > pos ? duration - pos : 0), *rm = g_strconcat("-", r, NULL);
  gtk_label_set_text(GTK_LABEL(elapsed_l), state == MPD_STATE_STOP ? "" : e);
  gtk_label_set_text(GTK_LABEL(remain_l), state == MPD_STATE_STOP || !duration ? "" : rm);
  g_free(e), g_free(r), g_free(rm);
  mini_progress(state == MPD_STATE_PLAY, pos, duration);
  return G_SOURCE_CONTINUE;
}

static void on_repeat(void) {  // off -> all -> one -> off, like the iPod
  if (begin()) mpd_run_repeat(conn, !repeat_on || !single_on), mpd_run_single(conn, repeat_on && !single_on), end();
}

static void refresh(void) {
  struct mpd_status *st = mpd_run_status(conn);
  if (!st) { ok(); return; }
  struct mpd_song *song = mpd_run_current_song(conn);
  if (!ok()) { mpd_status_free(st); return; }
  mpris_update(st, song);
  discord_update(song, mpd_status_get_state(st) == MPD_STATE_PLAY, mpd_status_get_elapsed_ms(st) / 1000.0, mpd_status_get_total_time(st));

  state = mpd_status_get_state(st);
  duration = mpd_status_get_total_time(st);
  queue_len = mpd_status_get_queue_length(st);
  elapsed = mpd_status_get_elapsed_ms(st) / 1000.0;
  elapsed_at = g_get_monotonic_time();
  // follow MPD's random mode, which other clients can change too; blocked so this doesn't toggle it back
  g_signal_handlers_block_by_func(shuffle, player_shuffle, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(shuffle), mpd_status_get_random(st));
  g_signal_handlers_unblock_by_func(shuffle, player_shuffle, NULL);
  repeat_on = mpd_status_get_repeat(st), single_on = mpd_status_get_single(st);
  g_signal_handlers_block_by_func(repeat, on_repeat, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(repeat), repeat_on);
  g_signal_handlers_unblock_by_func(repeat, on_repeat, NULL);
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(repeat))),
    single_on ? "media-playlist-repeat-song-symbolic" : "media-playlist-repeat-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_widget_set_tooltip_text(repeat, !repeat_on ? "Repeat: Off" : single_on ? "Repeat: One" : "Repeat: All");
  int vol = mpd_status_get_volume(st);  // -1: no mixer
  gtk_widget_set_sensitive(volume, vol >= 0);
  if (vol >= 0) gtk_range_set_value(GTK_RANGE(volume), vol);
  mpd_status_free(st);

  gtk_image_set_from_icon_name(GTK_IMAGE(play_icon), state == MPD_STATE_PLAY
    ? "media-playback-pause-symbolic" : "media-playback-start-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_range_set_range(GTK_RANGE(scrub), 0, MAX(duration, 1));
  gtk_widget_set_sensitive(scrub, duration && state != MPD_STATE_STOP);

  if (song) {
    const char *t = song_tag(song, MPD_TAG_TITLE);
    const char *ar = song_tag(song, MPD_TAG_ARTIST);
    const char *al = song_tag(song, MPD_TAG_ALBUM);
    char *base = g_path_get_basename(mpd_song_get_uri(song));
    char *m = g_markup_printf_escaped("<b>%s</b>", t ? t : base);
    char *sub = g_strjoin(ar && al ? " — " : "", ar ? ar : "", al ? al : "", NULL);
    char *wt = g_strconcat(t ? t : base, " — Ember", NULL);
    gtk_label_set_markup(GTK_LABEL(title), m);
    // the artist and album names link to their pages
    char *la = ar ? g_markup_printf_escaped("<a href=\"artist\">%s</a>", ar) : g_strdup("");
    char *lb = al ? g_markup_printf_escaped("<a href=\"album\">%s</a>", al) : g_strdup("");
    char *links = g_strjoin(ar && al ? " — " : "", la, lb, NULL);
    gtk_label_set_markup(GTK_LABEL(subtitle), links);
    g_free(la), g_free(lb), g_free(links);
    const char *aa = song_tag(song, MPD_TAG_ALBUM_ARTIST);
    g_free(now_album), g_free(now_artist);
    now_album = album_key(song);
    now_artist = g_strdup(aa ? aa : ar ? ar : "Unknown Artist");
    if (g_strcmp0(now_uri, mpd_song_get_uri(song))) {
      g_free(now_uri), now_uri = g_strdup(mpd_song_get_uri(song));
      gtk_widget_queue_draw(win);  // move the ▶
    }
    gtk_window_set_title(GTK_WINDOW(win), wt);
    mini_song(t ? t : base, sub, song);
    g_free(base), g_free(m), g_free(sub), g_free(wt);
    mpd_song_free(song);
  } else {
    gtk_label_set_markup(GTK_LABEL(title), "<b>Not playing</b>");
    gtk_label_set_text(GTK_LABEL(subtitle), queue_len ? "" : "Press play to play everything");
    gtk_window_set_title(GTK_WINDOW(win), "Ember");
    mini_song("Not playing", "", NULL);
    if (now_uri) { g_clear_pointer(&now_uri, g_free); gtk_widget_queue_draw(win); }
  }

  tick(NULL);
  if (state == MPD_STATE_PLAY && !ticker) ticker = g_timeout_add_seconds(1, tick, NULL);
  if (state != MPD_STATE_PLAY && ticker) g_source_remove(ticker), ticker = 0;
}

static gboolean on_idle(GIOChannel *ch, GIOCondition c, gpointer unused) {
  (void)ch, (void)c, (void)unused;
  guint w = watch;
  watch = 0;  // so a disconnect in here doesn't remove the source we're running in
  idling = FALSE;
  enum mpd_idle ev = mpd_recv_idle(conn, false);
  if (!ok()) return G_SOURCE_REMOVE;
  depth++;  // as if begin(): the loads below nest inside, and end() re-enters idle
  if (ev & MPD_IDLE_DATABASE) library_load();
  if (conn && ev & MPD_IDLE_STORED_PLAYLIST) playlists_load();
  if (conn && ev & (MPD_IDLE_QUEUE | MPD_IDLE_PLAYER)) queue_load();
  if (conn) end();
  if (!conn) return G_SOURCE_REMOVE;
  watch = w;
  return G_SOURCE_CONTINUE;
}

// Every command is wrapped in begin()/end(): leave idle, run it, refresh, re-enter idle.
// They nest, so code that loads something works both from a click and from an idle event.
gboolean begin(void) {
  if (!conn) return FALSE;
  if (depth++ == 0 && idling) idling = FALSE, mpd_run_noidle(conn);
  if (ok()) return TRUE;
  if (conn) depth--;  // (a disconnect already reset it)
  return FALSE;
}

void end(void) {
  ok();  // a server error from the command is logged and cleared; the connection stays
  if (!conn || --depth > 0) return;
  refresh();
  if (conn && !(idling = mpd_send_idle(conn))) ok();
}

// MPD_HOST wins (libmpdclient reads it when host is NULL); otherwise Ember's per-user socket.
static struct mpd_connection *open_raw(void) {
  char *path = g_build_filename(g_get_home_dir(), ".local/share/mpd/socket", NULL);
  struct mpd_connection *c = mpd_connection_new(g_getenv("MPD_HOST") ? NULL : path, 0, 0);
  g_free(path);
  return c;
}

struct mpd_connection *mpd_open(void) {
  struct mpd_connection *c = open_raw();
  if (mpd_connection_get_error(c) == MPD_ERROR_SUCCESS) return c;
  mpd_connection_free(c);
  return NULL;
}

static gboolean connect_mpd(gboolean spawn) {
  conn = open_raw();
  if (spawn && !g_getenv("MPD_HOST") && mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {  // only Ember's own MPD
    g_spawn_command_line_async("mpd", NULL);
    for (int i = 0; i < 30 && mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS; i++) {
      mpd_connection_free(conn);
      g_usleep(100000);
      conn = open_raw();
    }
  }
  if (mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS) {
    if (spawn) disconnect(mpd_connection_get_error_message(conn));  // also schedules reconnects
    else mpd_connection_free(conn), conn = NULL;
    return FALSE;
  }
  idling = FALSE, depth = 1;  // as if begin(); end() refreshes and enters idle
  library_load();
  if (conn) playlists_load();
  if (conn) end();
  if (!conn) return FALSE;
  GIOChannel *ch = g_io_channel_unix_new(mpd_connection_get_fd(conn));
  watch = g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_idle, NULL);
  g_io_channel_unref(ch);
  return TRUE;
}

static gboolean reconnect(gpointer unused) {
  (void)unused;
  if (!conn && !connect_mpd(FALSE)) return G_SOURCE_CONTINUE;
  retry = 0;
  return G_SOURCE_REMOVE;
}

void player_toggle(void) {
  if (!begin()) return;
  if (state == MPD_STATE_PLAY) mpd_run_pause(conn, true);
  else if (state == MPD_STATE_PAUSE) mpd_run_pause(conn, false);
  else {
    if (!queue_len) mpd_run_add(conn, "");  // empty queue: play the whole library, like the iPod
    if (ok()) mpd_run_play(conn);
  }
  end();
}

void player_prev(void) {  // like the iPod: restart the song unless it has only just begun
  double pos = elapsed + (state == MPD_STATE_PLAY ? (g_get_monotonic_time() - elapsed_at) / 1e6 : 0);
  if (!begin()) return;
  if (pos > 3) mpd_run_seek_current(conn, 0, false);
  else mpd_run_previous(conn);
  end();
}
void player_next(void) { if (begin()) mpd_run_next(conn), end(); }

void player_shuffle(GtkToggleButton *b) {  // MPD's random mode: plays the queue in a random order
  if (begin()) mpd_run_random(conn, gtk_toggle_button_get_active(b)), end();
}

static gboolean on_volume(GtkRange *r, GtkScrollType s, double v, gpointer unused) {
  (void)r, (void)s, (void)unused;
  if (begin()) mpd_run_set_volume(conn, CLAMP(v, 0, 100)), end();
  return FALSE;
}

// Dragging the scrubber only moves the labels; the seek happens once it rests for 250 ms.
static gboolean do_seek(gpointer unused) {
  (void)unused;
  seeker = 0;
  double to = seek_to;
  seek_to = -1;
  if (begin()) mpd_run_seek_current(conn, to, false), end();
  return G_SOURCE_REMOVE;
}

gboolean player_scrub(GtkRange *r, GtkScrollType s, double v, gpointer unused) {
  (void)r, (void)s, (void)unused;
  seek_to = CLAMP(v, 0, duration);
  tick(NULL);
  if (seeker) g_source_remove(seeker);
  seeker = g_timeout_add(250, do_seek, NULL);
  return FALSE;
}

static gboolean on_link(GtkLabel *l, const char *uri) {
  (void)l;
  if (!strcmp(uri, "album")) focus_album(now_album);
  else focus_artist(now_artist);
  return TRUE;  // handled: don't open it as a URL
}

static void on_search(GtkSearchEntry *e) { library_search(gtk_entry_get_text(GTK_ENTRY(e))); }
static void on_stop_search(GtkSearchEntry *e) { gtk_entry_set_text(GTK_ENTRY(e), ""); }

// Shortcuts for every Ember window. typing: a text field has focus, so keys that edit text pass.
gboolean player_key(GdkEventKey *ev, gboolean typing) {
  guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK), k = gdk_keyval_to_lower(ev->keyval);
  if (mods == GDK_CONTROL_MASK && k == GDK_KEY_q) g_application_quit(g_application_get_default());
  else if (mods == GDK_CONTROL_MASK && k == GDK_KEY_Up) { if (begin()) mpd_run_change_volume(conn, 5), end(); }
  else if (mods == GDK_CONTROL_MASK && k == GDK_KEY_Down) { if (begin()) mpd_run_change_volume(conn, -5), end(); }
  else if (typing) return FALSE;
  else if (!mods && k == GDK_KEY_space) player_toggle();
  else if (mods == GDK_CONTROL_MASK && k == GDK_KEY_Left) player_prev();
  else if (mods == GDK_CONTROL_MASK && k == GDK_KEY_Right) player_next();
  else if (mods == GDK_SHIFT_MASK && (k == GDK_KEY_Left || k == GDK_KEY_Right)) {
    if (begin()) mpd_run_seek_current(conn, k == GDK_KEY_Left ? -10 : 10, true), end();
  } else return FALSE;
  return TRUE;
}

// The main window's own shortcuts, then typing anywhere outside a text field searches.
static gboolean on_key(GtkWidget *w, GdkEvent *ev, GtkWidget *search) {
  static const char *tabs[] = { "playlists", "songs", "artists", "albums", "genres" };
  guint mods = ev->key.state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK), k = gdk_keyval_to_lower(ev->key.keyval);
  gboolean typing = GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(w)));
  if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && k == GDK_KEY_m) mini_toggle();  // as in iTunes
  else if (mods == GDK_CONTROL_MASK && k == GDK_KEY_f) gtk_widget_grab_focus(search);
  else if (mods == GDK_CONTROL_MASK && k >= GDK_KEY_1 && k <= GDK_KEY_5) gtk_stack_set_visible_child_name(GTK_STACK(stack), tabs[k - GDK_KEY_1]);
  else if (player_key(&ev->key, typing)) ;
  else if (typing) return GDK_EVENT_PROPAGATE;
  else if (gtk_search_entry_handle_event(GTK_SEARCH_ENTRY(search), ev)) {
    gtk_widget_grab_focus(search);
    gtk_editable_set_position(GTK_EDITABLE(search), -1);
  } else return GDK_EVENT_PROPAGATE;
  return GDK_EVENT_STOP;
}

GtkWidget *icon_button(const char *icon, const char *tip, GtkIconSize size, GCallback cb) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon, size);
  gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(b, tip);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  g_signal_connect(b, "clicked", cb, NULL);
  return b;
}

GtkWidget *text_button(const char *text, const char *cls, GCallback cb, gpointer data) {
  GtkWidget *b = gtk_button_new_with_label(text);
  gtk_style_context_add_class(gtk_widget_get_style_context(b), cls);
  gtk_widget_set_halign(b, GTK_ALIGN_START);
  g_signal_connect_swapped(b, "clicked", cb, data);
  return b;
}

static GtkWidget *mode_button(const char *icon, const char *tip, GCallback cb) {  // shuffle, repeat: lit while on
  GtkWidget *b = gtk_toggle_button_new();
  gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR));
  gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(b, tip);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(b), "mode");
  g_signal_connect(b, "toggled", cb, NULL);
  return b;
}

static gboolean on_close(GtkWidget *w, GdkEvent *ev, GApplication *app) {
  (void)w, (void)ev;
  g_application_quit(app);
  return GDK_EVENT_STOP;
}

static void activate(GtkApplication *app) {
  if (win) { if (!mini_present()) gtk_window_present(GTK_WINDOW(win)); return; }
  settings_init();
  theme_init();
  // the app icon: from the icon theme once installed, else data/ember.svg beside the binary
  if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "ember")) gtk_window_set_default_icon_name("ember");
  else {
    char *exe = g_file_read_link("/proc/self/exe", NULL), *dir = exe ? g_path_get_dirname(exe) : NULL;
    char *svg = dir ? g_build_filename(dir, "data", "ember.svg", NULL) : NULL;
    if (svg) gtk_window_set_default_icon_from_file(svg, NULL);
    g_free(exe), g_free(dir), g_free(svg);
  }
  mpris_init(G_APPLICATION(app));
  coverart_init(discord_art_found);
  win = gtk_application_window_new(app);
  // clicking a slider jumps to that spot, instead of stepping towards it
  g_object_set(gtk_settings_get_default(), "gtk-primary-button-warps-slider", TRUE, NULL);
  gtk_window_set_title(GTK_WINDOW(win), "Ember");
  int w = 960, h = 600;  // as it was left
  char *size = settings_string("window");
  if (size) sscanf(size, "%dx%d", &w, &h);
  gtk_window_set_default_size(GTK_WINDOW(win), w, h);
  g_free(size);

  // Top bar, as on the iPad: transport and volume on the left, now playing in the middle.
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(bar), "nowbar");
  GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *play = icon_button("media-playback-start-symbolic", "Play/Pause", GTK_ICON_SIZE_LARGE_TOOLBAR, G_CALLBACK(player_toggle));
  play_icon = gtk_button_get_image(GTK_BUTTON(play));
  gtk_box_pack_start(GTK_BOX(transport), icon_button("media-skip-backward-symbolic", "Previous", GTK_ICON_SIZE_LARGE_TOOLBAR, G_CALLBACK(player_prev)), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(transport), play, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(transport), icon_button("media-skip-forward-symbolic", "Next", GTK_ICON_SIZE_LARGE_TOOLBAR, G_CALLBACK(player_next)), FALSE, FALSE, 0);
  volume = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
  gtk_scale_set_draw_value(GTK_SCALE(volume), FALSE);
  gtk_widget_set_size_request(volume, 120, -1);
  g_signal_connect(volume, "change-value", G_CALLBACK(on_volume), NULL);
  gtk_box_pack_start(GTK_BOX(transport), volume, FALSE, FALSE, 8);
  shuffle = mode_button("media-playlist-shuffle-symbolic", "Shuffle", G_CALLBACK(player_shuffle));
  repeat = mode_button("media-playlist-repeat-symbolic", "Repeat: Off", G_CALLBACK(on_repeat));
  gtk_box_pack_start(GTK_BOX(transport), shuffle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(transport), repeat, FALSE, FALSE, 0);

  GtkWidget *now = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  title = gtk_label_new(NULL);
  subtitle = gtk_label_new(NULL);
  g_signal_connect(subtitle, "activate-link", G_CALLBACK(on_link), NULL);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
  GtkWidget *pos = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  elapsed_l = gtk_label_new(NULL);
  remain_l = gtk_label_new(NULL);
  gtk_label_set_width_chars(GTK_LABEL(elapsed_l), 6);
  gtk_label_set_width_chars(GTK_LABEL(remain_l), 6);
  gtk_label_set_xalign(GTK_LABEL(elapsed_l), 1);
  gtk_label_set_xalign(GTK_LABEL(remain_l), 0);
  for (GtkWidget **w = (GtkWidget *[]){ subtitle, elapsed_l, remain_l, NULL }; *w; w++)
    gtk_style_context_add_class(gtk_widget_get_style_context(*w), "dim");
  scrub = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
  gtk_scale_set_draw_value(GTK_SCALE(scrub), FALSE);
  g_signal_connect(scrub, "change-value", G_CALLBACK(player_scrub), NULL);
  gtk_box_pack_start(GTK_BOX(pos), elapsed_l, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(pos), scrub, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(pos), remain_l, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(now), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(now), subtitle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(now), pos, FALSE, FALSE, 0);

  GtkWidget *search = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search");
  gtk_entry_set_width_chars(GTK_ENTRY(search), 18);
  gtk_widget_set_valign(search, GTK_ALIGN_CENTER);
  g_signal_connect(search, "search-changed", G_CALLBACK(on_search), NULL);
  g_signal_connect(search, "stop-search", G_CALLBACK(on_stop_search), NULL);
  g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), search);

  gtk_box_pack_start(GTK_BOX(bar), transport, FALSE, FALSE, 0);
  gtk_box_set_center_widget(GTK_BOX(bar), now);
  gtk_box_pack_end(GTK_BOX(bar), settings_button(), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(bar), search, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(bar), queue_button(), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(bar), icon_button("view-restore-symbolic", "Mini Player (Ctrl+Shift+M)", GTK_ICON_SIZE_LARGE_TOOLBAR,
                                              G_CALLBACK(mini_toggle)), FALSE, FALSE, 0);
  gtk_widget_set_size_request(now, 420, -1);

  // Bottom bar: the views, as tabs.
  stack = library_new();
  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(tabs), "tabbar");
  GtkWidget *switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
  gtk_box_set_center_widget(GTK_BOX(tabs), switcher);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), stack, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), tabs, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(win), root);
  gtk_widget_show_all(win);
  char *tab = settings_string("tab");  // only works once the pages are shown
  gtk_stack_set_visible_child_name(GTK_STACK(stack), tab && gtk_stack_get_child_by_name(GTK_STACK(stack), tab) ? tab : "albums");
  g_free(tab);
  // show_all also showed the switcher's buttons for the untitled album and artist pages
  GList *pages = gtk_container_get_children(GTK_CONTAINER(stack)), *buttons = gtk_container_get_children(GTK_CONTAINER(switcher));
  for (GList *p = pages, *t = buttons; p && t; p = p->next, t = t->next) {
    char *name;
    gtk_container_child_get(GTK_CONTAINER(stack), p->data, "title", &name, NULL);
    if (!name) gtk_widget_hide(t->data);
    g_free(name);
  }
  g_list_free(pages), g_list_free(buttons);

  mini_init(app, win);
  // the hidden mini windows would keep the app alive after the main one closes
  g_signal_connect(win, "delete-event", G_CALLBACK(on_close), app);
  connect_mpd(TRUE);
}

// Closing Ember stops the music unless the user chose otherwise. MPD on Ember's own socket (the
// one Ember starts) is told to exit as well; it saves its place and resumes, paused, next time.
static void on_shutdown(void) {
  if (win) {  // reopen as it was left: the size and the tab (not an album or artist page, empty at start)
    int w, h;
    gtk_window_get_size(GTK_WINDOW(win), &w, &h);
    char *size = g_strdup_printf("%dx%d", w, h);
    const char *tab = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    settings_set_string("window", size);
    if (tab && strcmp(tab, "album") && strcmp(tab, "artist")) settings_set_string("tab", tab);
    g_free(size);
    mini_save();
  }
  if (!conn || settings_get("keep-playing") || !begin()) return;
  mpd_run_pause(conn, true);
  if (!g_getenv("MPD_HOST") && mpd_send_command(conn, "kill", NULL))
    mpd_response_finish(conn);  // wait until MPD hangs up; closing first can lose the command
  mpd_connection_free(conn);  // no end(): nothing to go back to idle for
  conn = NULL;
}

int main(int argc, char **argv) {
  GtkApplication *app = gtk_application_new("org.kindling.Ember", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  int rc = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return rc;
}
