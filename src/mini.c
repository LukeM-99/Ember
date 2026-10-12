// mini.c: the mini and micro players, after iTunes 11's MiniPlayer. Each is a small borderless
// window above the others; drag anywhere to move it, hover to see the controls.
//   mini:  cover, then title and "artist — album" (on hover: prev, play, next), progress under.
//   micro: just the cover; on hover a shade with the controls and the size buttons.
#include "ember.h"

enum { FULL, MINI, MICRO };
static int mode = FULL;
static GtkWidget *main_win, *mini, *micro;
static GtkWidget *mini_art, *mini_title, *mini_sub, *mini_stack, *mini_play, *mini_pos, *mini_hover;
static GtkWidget *micro_art, *micro_shade, *micro_play, *micro_pos;
static char *key;  // the playing song's cover key
static int mini_size, micro_size;
static int pos[3][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };  // where each window was left

static GtkWidget *window_of(int m) { return m == FULL ? main_win : m == MINI ? mini : micro; }

static void show_mode(int m) {
  if (m == mode) return;
  GtkWidget *from = window_of(mode), *to = window_of(m);
  gtk_window_get_position(GTK_WINDOW(from), &pos[mode][0], &pos[mode][1]);
  if (pos[m][0] >= 0) gtk_window_move(GTK_WINDOW(to), pos[m][0], pos[m][1]);
  mode = m;
  gtk_widget_show(to);
  gtk_window_present(GTK_WINDOW(to));
  gtk_widget_hide(from);
}

void mini_toggle(void) { show_mode(mode == FULL ? MINI : FULL); }

gboolean mini_present(void) {
  if (mode == FULL) return FALSE;
  gtk_window_present(GTK_WINDOW(window_of(mode)));
  return TRUE;
}

const char *mini_album(void) { return key; }

void mini_art_changed(void) {
  GdkPixbuf *small = key ? art_peek(key) : NULL, *big = key ? art_peek_size(key, micro_size) : NULL;
  if (!small) small = cover_placeholder();
  if (!big) big = small;  // until the big one arrives
  GdkPixbuf *a = art_fit(small, mini_size), *b = art_fit(big, micro_size);
  gtk_image_set_from_pixbuf(GTK_IMAGE(mini_art), a);
  gtk_image_set_from_pixbuf(GTK_IMAGE(micro_art), b);
  g_object_unref(a), g_object_unref(b);
}

void mini_song(const char *title, const char *sub, const struct mpd_song *song) {
  if (!mini) return;
  gtk_label_set_text(GTK_LABEL(mini_title), title);
  gtk_label_set_text(GTK_LABEL(mini_sub), sub);
  char *tip = *sub ? g_strconcat(title, "\n", sub, NULL) : g_strdup(title);
  gtk_widget_set_tooltip_text(micro_art, tip);
  g_free(tip);
  char *k = song ? album_key(song) : NULL;
  if (g_strcmp0(k, key)) {
    g_free(key), key = k;
    if (key) art_want(key, mpd_song_get_uri(song)), art_want_size(key, mpd_song_get_uri(song), micro_size);
    mini_art_changed();
  } else g_free(k);
}

void mini_progress(gboolean playing, double at, unsigned duration) {
  if (!mini) return;
  const char *icon = playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(mini_play))), icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(micro_play))), icon, GTK_ICON_SIZE_DND);
  for (GtkWidget **s = (GtkWidget *[]){ mini_pos, micro_pos, NULL }; *s; s++) {
    gtk_range_set_range(GTK_RANGE(*s), 0, MAX(duration, 1));
    gtk_range_set_value(GTK_RANGE(*s), at);
    gtk_widget_set_sensitive(*s, duration > 0);
  }
}

// ---- building

static void hover(GtkWidget *w, gboolean in) {
  if (w == mini) {
    gtk_stack_set_visible_child_name(GTK_STACK(mini_stack), in ? "controls" : "info");
    gtk_widget_set_visible(mini_hover, in);
  } else gtk_widget_set_visible(micro_shade, in);
}

static gboolean on_cross(GtkWidget *w, GdkEventCrossing *ev, gpointer unused) {
  (void)unused;
  if (ev->detail != GDK_NOTIFY_INFERIOR) hover(w, ev->type == GDK_ENTER_NOTIFY);
  return GDK_EVENT_PROPAGATE;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {  // presses no button took
  (void)unused;
  if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) return GDK_EVENT_PROPAGATE;
  gtk_window_begin_move_drag(GTK_WINDOW(w), 1, ev->x_root, ev->y_root, ev->time);
  return GDK_EVENT_STOP;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer unused) {
  (void)w, (void)unused;
  if (player_key(ev, FALSE)) return GDK_EVENT_STOP;
  if ((ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && (ev->keyval == GDK_KEY_M || ev->keyval == GDK_KEY_m)) {
    show_mode(FULL);
    return GDK_EVENT_STOP;
  }
  return GDK_EVENT_PROPAGATE;
}

static gboolean quit(void) {  // also the windows' delete-event, which needs the gboolean; on_shutdown follows Keep Playing
  g_application_quit(g_application_get_default());
  return GDK_EVENT_STOP;
}
static void to_full(void) { show_mode(FULL); }
static void to_mini(void) { show_mode(MINI); }
static void to_micro(void) { show_mode(MICRO); }

static GtkWidget *transport(GtkIconSize size, GtkWidget **play) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  *play = icon_button("media-playback-start-symbolic", "Play/Pause", size, G_CALLBACK(player_toggle));
  gtk_box_pack_start(GTK_BOX(box), icon_button("media-skip-backward-symbolic", "Previous", size, G_CALLBACK(player_prev)), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), *play, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), icon_button("media-skip-forward-symbolic", "Next", size, G_CALLBACK(player_next)), FALSE, FALSE, 0);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  return box;
}

static GtkWidget *progress(void) {
  GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
  gtk_scale_set_draw_value(GTK_SCALE(s), FALSE);
  g_signal_connect(s, "change-value", G_CALLBACK(player_scrub), NULL);
  return s;
}

static GtkWidget *small_window(GtkApplication *app, const char *cls) {
  GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_application(GTK_WINDOW(w), app);
  gtk_window_set_title(GTK_WINDOW(w), "Ember");
  gtk_window_set_decorated(GTK_WINDOW(w), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(w), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
  gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
  gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(w, "button-press-event", G_CALLBACK(on_press), NULL);
  g_signal_connect(w, "enter-notify-event", G_CALLBACK(on_cross), NULL);
  g_signal_connect(w, "leave-notify-event", G_CALLBACK(on_cross), NULL);
  g_signal_connect(w, "key-press-event", G_CALLBACK(on_key), NULL);
  g_signal_connect(w, "delete-event", G_CALLBACK(quit), NULL);
  return w;
}

static void build_mini(GtkApplication *app) {
  mini = small_window(app, "mini");
  mini_art = gtk_image_new();
  gtk_widget_set_size_request(mini_art, mini_size, mini_size);

  // over the cover, on hover: close, and back to the full window
  mini_hover = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(mini_hover), "shade");
  gtk_box_pack_start(GTK_BOX(mini_hover), icon_button("window-close-symbolic", "Quit Ember", GTK_ICON_SIZE_MENU, G_CALLBACK(quit)), TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(mini_hover), icon_button("window-maximize-symbolic", "Full Window", GTK_ICON_SIZE_MENU, G_CALLBACK(to_full)), TRUE, FALSE, 0);
  GtkWidget *cover = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(cover), mini_art);
  gtk_overlay_add_overlay(GTK_OVERLAY(cover), mini_hover);

  mini_title = gtk_label_new(NULL);
  mini_sub = gtk_label_new(NULL);
  gtk_style_context_add_class(gtk_widget_get_style_context(mini_title), "title");
  gtk_style_context_add_class(gtk_widget_get_style_context(mini_sub), "dim");
  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(info, GTK_ALIGN_CENTER);
  for (GtkWidget **l = (GtkWidget *[]){ mini_title, mini_sub, NULL }; *l; l++) {
    gtk_label_set_ellipsize(GTK_LABEL(*l), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(info), *l, FALSE, FALSE, 0);
  }
  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_center_widget(GTK_BOX(controls), transport(GTK_ICON_SIZE_LARGE_TOOLBAR, &mini_play));
  gtk_box_pack_end(GTK_BOX(controls), icon_button("view-restore-symbolic", "Micro Player", GTK_ICON_SIZE_MENU, G_CALLBACK(to_micro)), FALSE, FALSE, 0);
  mini_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(mini_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(GTK_STACK(mini_stack), 120);
  gtk_stack_set_homogeneous(GTK_STACK(mini_stack), TRUE);
  gtk_stack_add_named(GTK_STACK(mini_stack), info, "info");
  gtk_stack_add_named(GTK_STACK(mini_stack), controls, "controls");

  mini_pos = progress();
  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(right), mini_stack, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(right), mini_pos, FALSE, FALSE, 0);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(row), cover, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), right, TRUE, TRUE, 0);
  gtk_widget_set_size_request(row, mini_size * 6, -1);
  gtk_container_add(GTK_CONTAINER(mini), row);
  gtk_widget_show_all(row);
  gtk_widget_set_no_show_all(mini_hover, TRUE);
  gtk_widget_hide(mini_hover);
}

static void build_micro(GtkApplication *app) {
  micro = small_window(app, "micro");
  micro_art = gtk_image_new();
  gtk_widget_set_size_request(micro_art, micro_size, micro_size);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(top), icon_button("window-close-symbolic", "Quit Ember", GTK_ICON_SIZE_MENU, G_CALLBACK(quit)), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(top), icon_button("window-maximize-symbolic", "Full Window", GTK_ICON_SIZE_MENU, G_CALLBACK(to_full)), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(top), icon_button("view-fullscreen-symbolic", "Mini Player", GTK_ICON_SIZE_MENU, G_CALLBACK(to_mini)), FALSE, FALSE, 0);
  micro_pos = progress();
  micro_shade = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(micro_shade), "shade");
  gtk_box_pack_start(GTK_BOX(micro_shade), top, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(micro_shade), transport(GTK_ICON_SIZE_DND, &micro_play), TRUE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(micro_shade), micro_pos, FALSE, FALSE, 0);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), micro_art);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), micro_shade);
  gtk_container_add(GTK_CONTAINER(micro), over);
  gtk_widget_show_all(over);
  gtk_widget_set_no_show_all(micro_shade, TRUE);
  gtk_widget_hide(micro_shade);
}

void mini_init(GtkApplication *app, GtkWidget *main_window) {
  main_win = main_window;
  mini_size = art_size / 2;          // 64 px at 96 dpi
  micro_size = art_size * 3 / 2;     // 192 px
  build_mini(app);
  build_micro(app);
  mini_art_changed();
  char *saved = settings_string("mini-positions");  // "x,y x,y" for mini, micro
  if (saved && sscanf(saved, "%d,%d %d,%d", &pos[MINI][0], &pos[MINI][1], &pos[MICRO][0], &pos[MICRO][1]) == 4)
    for (int m = MINI; m <= MICRO; m++) {
      GdkRectangle r;  // on a monitor since unplugged: let the window manager place it instead
      gdk_monitor_get_geometry(gdk_display_get_monitor_at_point(gdk_display_get_default(), pos[m][0], pos[m][1]), &r);
      if (pos[m][0] < r.x || pos[m][1] < r.y || pos[m][0] >= r.x + r.width || pos[m][1] >= r.y + r.height) pos[m][0] = pos[m][1] = -1;
    }
  g_free(saved);
}

void mini_save(void) {  // at quit, for next time
  if (mode != FULL) gtk_window_get_position(GTK_WINDOW(window_of(mode)), &pos[mode][0], &pos[mode][1]);
  char *s = g_strdup_printf("%d,%d %d,%d", pos[MINI][0], pos[MINI][1], pos[MICRO][0], pos[MICRO][1]);
  settings_set_string("mini-positions", s);
  g_free(s);
}
