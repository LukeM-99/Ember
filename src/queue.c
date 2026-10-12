// queue.c: Up Next, the songs after the current one, on a card under the top bar's list button.
// It loads only while open, and shows at most 500 songs: after "play everything" the queue
// can be the whole library.
#include "ember.h"

#define SHOWN 500

static GtkWidget *button, *card, *list, *count, *empty;
static GtkListStore *store;    // COL_SONG, COL_GROUP (NULL)
static GPtrArray *songs;       // owns the Songs in store
static unsigned first;         // queue position of the first row

void queue_load(void) {
  if (!card || !gtk_widget_get_visible(card) || !begin()) return;
  GPtrArray *found = g_ptr_array_new_with_free_func(song_free);
  unsigned total = 0;
  struct mpd_status *st = mpd_run_status(conn);
  if (st) {
    total = mpd_status_get_queue_length(st);
    first = mpd_status_get_song_pos(st) + 1;  // -1 + 1 when stopped: the whole queue is up next
    mpd_status_free(st);
  }
  if (first < total && mpd_send_list_queue_range_meta(conn, first, MIN(total, first + SHOWN))) {
    struct mpd_song *m;
    while ((m = mpd_recv_song(conn))) g_ptr_array_add(found, song_new(m)), mpd_song_free(m);
    mpd_response_finish(conn);
  }
  end();

  gtk_tree_view_set_model(GTK_TREE_VIEW(list), NULL);  // rows point into songs
  gtk_list_store_clear(store);
  if (songs) g_ptr_array_unref(songs);
  songs = found;
  for (guint i = 0; i < found->len; i++) gtk_list_store_insert_with_values(store, NULL, -1, COL_SONG, found->pdata[i], -1);
  gtk_tree_view_set_model(GTK_TREE_VIEW(list), GTK_TREE_MODEL(store));

  unsigned left = total > first ? total - first : 0;
  char *c = left > SHOWN ? g_strdup_printf("%u songs, showing the next %d", left, SHOWN)
                         : g_strdup_printf("%u song%s", left, left == 1 ? "" : "s");
  gtk_label_set_text(GTK_LABEL(count), c);
  g_free(c);
  gtk_widget_set_visible(empty, !left);
  gtk_widget_set_visible(gtk_widget_get_parent(list), left > 0);
}

static void on_activate(GtkTreeView *v, GtkTreePath *p) {  // jump to that song; the queue stays
  (void)v;
  if (begin()) mpd_run_play_pos(conn, first + gtk_tree_path_get_indices(p)[0]), end();
}

static void on_remove(GtkMenuItem *item, gpointer unused) {
  (void)item, (void)unused;
  GList *rows = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), NULL);
  if (begin()) {
    mpd_command_list_begin(conn, false);
    for (GList *r = g_list_last(rows); r; r = r->prev)  // last first, so positions stay put
      mpd_send_delete(conn, first + gtk_tree_path_get_indices(r->data)[0]);
    mpd_command_list_end(conn);
    mpd_response_finish(conn);
    end();  // MPD reports the change, and queue_load picks it up
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
}

static void menu_extra(GtkWidget *menu, guint n) {
  GtkWidget *i = gtk_menu_item_new_with_label(n == 1 ? "Remove from Up Next" : "Remove Songs from Up Next");
  g_signal_connect(i, "activate", G_CALLBACK(on_remove), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
}

static void on_clear(void) {  // the upcoming songs only; the current one keeps playing
  if (begin()) mpd_run_delete_range(conn, first, (unsigned)-1), end();
}

static void on_save(void) {
  char *name = ask_name("Save Queue as Playlist", "", "Save");
  if (name && begin()) mpd_run_save(conn, name), end();
  g_free(name);
}

static void on_toggle(GtkToggleButton *b) {
  if (gtk_toggle_button_get_active(b)) {
    gtk_popover_popup(GTK_POPOVER(card));
    queue_load();
  } else gtk_popover_popdown(GTK_POPOVER(card));
}

static void on_closed(void) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE); }

static GtkWidget *text_button(const char *label, GCallback cb) {
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_style_context_add_class(gtk_widget_get_style_context(b), "textbutton");
  g_signal_connect(b, "clicked", cb, NULL);
  return b;
}

GtkWidget *queue_button(void) {
  button = gtk_toggle_button_new();
  gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name("view-list-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR));
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(button, "Up Next");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  g_signal_connect(button, "toggled", G_CALLBACK(on_toggle), NULL);

  store = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  static const int cols[] = { NAME, TIME, ARTIST };
  list = song_list(GTK_TREE_MODEL(store), cols, 3, FALSE);
  g_object_set_data(G_OBJECT(list), "on-activate", on_activate);
  g_object_set_data(G_OBJECT(list), "menu-extra", menu_extra);

  GtkWidget *title = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(title), "<big><b>Up Next</b></big>");
  count = gtk_label_new(NULL);
  gtk_style_context_add_class(gtk_widget_get_style_context(count), "dim");
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(head), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(head), count, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(head), text_button("Clear", G_CALLBACK(on_clear)), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(head), text_button("Save as Playlist…", G_CALLBACK(on_save)), FALSE, FALSE, 0);

  empty = gtk_label_new("Nothing up next. Right-click songs to add them.");
  gtk_style_context_add_class(gtk_widget_get_style_context(empty), "dim");
  g_object_set(empty, "margin", 24, NULL);

  GtkWidget *sw = scrolled(list);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 460);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  g_object_set(box, "margin", 12, NULL);
  gtk_widget_set_size_request(box, 520, -1);
  gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), empty, FALSE, FALSE, 0);
  gtk_widget_show_all(box);
  gtk_widget_set_no_show_all(empty, TRUE);
  gtk_widget_set_no_show_all(sw, TRUE);

  card = gtk_popover_new(button);
  gtk_container_add(GTK_CONTAINER(card), box);
  g_signal_connect(card, "closed", G_CALLBACK(on_closed), NULL);
  return button;
}
