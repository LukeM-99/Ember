// playlists.c: the Playlists tab (MPD's stored playlists) and the right-click menu that adds
// songs to them. MPD 0.23 can't store an empty playlist, so a new one lives here as a draft
// until its first song is added.
#include "ember.h"
#include <string.h>

static GtkListStore *names;          // G_TYPE_STRING
static GtkListStore *tracks;         // COL_SONG, COL_GROUP (always NULL)
static GPtrArray *track_songs;       // owns the Songs in tracks
static GtkTreeView *list;
static GtkWidget *detail, *menu;
static GPtrArray *drafts;            // names not yet in MPD

static char *selected(void) {
  GtkTreeIter it;
  char *name = NULL;
  if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(list), NULL, &it))
    gtk_tree_model_get(GTK_TREE_MODEL(names), &it, 0, &name, -1);
  return name;
}

static void select_name(const char *name) {
  GtkTreeModel *m = GTK_TREE_MODEL(names);
  GtkTreeIter it;
  for (gboolean more = gtk_tree_model_get_iter_first(m, &it); more; more = gtk_tree_model_iter_next(m, &it)) {
    char *n;
    gtk_tree_model_get(m, &it, 0, &n, -1);
    gboolean hit = !g_strcmp0(n, name);
    g_free(n);
    if (hit) {
      gtk_tree_selection_select_iter(gtk_tree_view_get_selection(list), &it);
      return;
    }
  }
}

static void load_tracks(void) {
  char *name = selected();
  GPtrArray *found = g_ptr_array_new_with_free_func(song_free);
  if (name && !g_ptr_array_find_with_equal_func(drafts, name, g_str_equal, NULL) && begin()) {
    if (mpd_send_list_playlist_meta(conn, name)) {
      struct mpd_song *m;
      while ((m = mpd_recv_song(conn))) g_ptr_array_add(found, song_new(m)), mpd_song_free(m);
      mpd_response_finish(conn);
    }
    end();
  }
  g_free(name);
  gtk_tree_view_set_model(GTK_TREE_VIEW(detail), NULL);  // rows point into track_songs
  gtk_list_store_clear(tracks);
  if (track_songs) g_ptr_array_unref(track_songs);
  track_songs = found;
  for (guint i = 0; i < found->len; i++)
    gtk_list_store_insert_with_values(tracks, NULL, -1, COL_SONG, found->pdata[i], -1);
  gtk_tree_view_set_model(GTK_TREE_VIEW(detail), GTK_TREE_MODEL(tracks));
}

static void on_pick(GtkTreeSelection *sel, gpointer unused) {
  (void)sel, (void)unused;
  load_tracks();
}

static int by_name(gconstpointer a, gconstpointer b) {
  return g_utf8_collate(*(char **)a, *(char **)b);
}

void playlists_load(void) {
  if (!begin()) return;
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  if (mpd_send_list_playlists(conn)) {
    struct mpd_playlist *p;
    while ((p = mpd_recv_playlist(conn))) g_ptr_array_add(found, g_strdup(mpd_playlist_get_path(p))), mpd_playlist_free(p);
    mpd_response_finish(conn);
  }
  if (!ok()) { g_ptr_array_unref(found); end(); return; }
  for (guint i = drafts->len; i-- > 0; ) {  // a draft that MPD now has is a real playlist
    if (g_ptr_array_find_with_equal_func(found, drafts->pdata[i], g_str_equal, NULL)) g_ptr_array_remove_index(drafts, i);
    else g_ptr_array_add(found, g_strdup(drafts->pdata[i]));
  }
  g_ptr_array_sort(found, by_name);

  char *was = selected();
  gtk_tree_view_set_model(list, NULL);
  gtk_list_store_clear(names);
  for (guint i = 0; i < found->len; i++) gtk_list_store_insert_with_values(names, NULL, -1, 0, found->pdata[i], -1);
  gtk_tree_view_set_model(list, GTK_TREE_MODEL(names));
  select_name(was);
  if (!was) {
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(names), &it))
      gtk_tree_selection_select_iter(gtk_tree_view_get_selection(list), &it);
  }
  g_free(was);
  g_ptr_array_unref(found);
  load_tracks();  // the open playlist may have changed too
  end();
}

// ---- asking for a name

static void name_changed(GtkEntry *e, GtkDialog *d) {
  char *t = g_strstrip(g_strdup(gtk_entry_get_text(e)));  // MPD refuses a slash or a newline
  gtk_dialog_set_response_sensitive(d, GTK_RESPONSE_OK, *t && !strpbrk(t, "/\n\r"));
  g_free(t);
}

char *ask_name(const char *title, const char *initial, const char *ok_label) {
  GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(list))),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Cancel", GTK_RESPONSE_CANCEL, ok_label, GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
  GtkWidget *e = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(e), initial);
  gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Playlist Name");
  gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
  gtk_entry_set_width_chars(GTK_ENTRY(e), 30);
  g_object_set(e, "margin", 12, NULL);
  g_signal_connect(e, "changed", G_CALLBACK(name_changed), d);
  name_changed(GTK_ENTRY(e), GTK_DIALOG(d));
  gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), e);
  gtk_widget_show_all(d);
  char *name = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK ? g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e)))) : NULL;
  gtk_widget_destroy(d);
  return name;
}

static gboolean confirm(const char *question, const char *detail_text, const char *ok_label) {
  GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(list))),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question);
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail_text);
  gtk_dialog_add_buttons(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL, ok_label, GTK_RESPONSE_OK, NULL);
  GtkWidget *b = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
  gtk_style_context_add_class(gtk_widget_get_style_context(b), "destructive-action");
  gboolean yes = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
  gtk_widget_destroy(d);
  return yes;
}

// ---- changing playlists

static void add_to(const char *name, char **uris) {
  if (!begin()) return;
  guint n = g_strv_length(uris);
  for (guint i = 0; i < n && ok(); i += 500) {  // MPD caps a command list at 2 MB
    mpd_command_list_begin(conn, false);
    for (guint j = i; j < n && j < i + 500; j++) mpd_send_playlist_add(conn, name, uris[j]);
    mpd_command_list_end(conn);
    mpd_response_finish(conn);
  }
  end();  // MPD reports the change, and playlists_load picks it up
}

static void on_add(GtkMenuItem *item, char **uris) {
  add_to(g_object_get_data(G_OBJECT(item), "name"), uris);
}

static void on_add_new(GtkMenuItem *item, char **uris) {
  (void)item;
  char *name = ask_name("New Playlist", "", "Create");
  if (name) add_to(name, uris);
  g_free(name);
}

// Play Next puts the songs right after the current one, in order; Add to Up Next appends them.
static void enqueue(char **uris, gboolean next) {
  if (!begin()) return;
  int at = -1;
  struct mpd_status *st = next ? mpd_run_status(conn) : NULL;
  if (st) at = mpd_status_get_song_pos(st), mpd_status_free(st);
  guint n = g_strv_length(uris);
  for (guint i = 0; i < n && ok(); i += 500) {
    mpd_command_list_begin(conn, false);
    for (guint j = i; j < n && j < i + 500; j++)
      if (at >= 0) mpd_send_add_whence(conn, uris[j], at + 1 + j, MPD_POSITION_ABSOLUTE);
      else mpd_send_add(conn, uris[j]);
    mpd_command_list_end(conn);
    mpd_response_finish(conn);
  }
  end();
}

static void on_play_next(GtkMenuItem *item, char **uris) { (void)item; enqueue(uris, TRUE); }
static void on_up_next(GtkMenuItem *item, char **uris) { (void)item; enqueue(uris, FALSE); }

static void on_remove(GtkMenuItem *item, gpointer unused) {
  (void)item, (void)unused;
  char *name = selected();
  GList *rows = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(detail)), NULL);
  if (name && begin()) {
    mpd_command_list_begin(conn, false);
    for (GList *r = g_list_last(rows); r; r = r->prev)  // last first, so positions stay put
      mpd_send_playlist_delete(conn, name, gtk_tree_path_get_indices(r->data)[0]);
    mpd_command_list_end(conn);
    mpd_response_finish(conn);
    end();
  }
  g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
  g_free(name);
}

static void on_new(GtkButton *b, gpointer unused) {
  (void)b, (void)unused;
  char *name = ask_name("New Playlist", "", "Create");
  if (!name) return;
  g_ptr_array_add(drafts, name);
  playlists_load();
  select_name(name);
}

static void on_rename(GtkMenuItem *item, gpointer unused) {
  (void)item, (void)unused;
  char *old = selected(), *name = old ? ask_name("Rename Playlist", old, "Rename") : NULL;
  guint i;
  if (name && g_ptr_array_find_with_equal_func(drafts, old, g_str_equal, &i)) {
    g_ptr_array_remove_index(drafts, i);
    g_ptr_array_add(drafts, g_strdup(name));
    playlists_load();
  } else if (name && begin()) {
    mpd_run_rename(conn, old, name);
    end();
  }
  if (name) select_name(name);
  g_free(old), g_free(name);
}

static void on_delete(GtkMenuItem *item, gpointer unused) {
  (void)item, (void)unused;
  char *name = selected();
  char *q = name ? g_strdup_printf("Delete “%s”?", name) : NULL;
  guint i;
  if (q && confirm(q, "The songs stay in your library.", "Delete")) {
    if (g_ptr_array_find_with_equal_func(drafts, name, g_str_equal, &i)) g_ptr_array_remove_index(drafts, i), playlists_load();
    else if (begin()) mpd_run_rm(conn, name), end();
  }
  g_free(q), g_free(name);
}

static void on_play(GtkMenuItem *item, gpointer unused) {
  (void)item, (void)unused;
  GtkTreeIter it;
  if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tracks), &it)) {
    GtkTreePath *p = gtk_tree_model_get_path(GTK_TREE_MODEL(tracks), &it);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(detail), p, NULL);
    gtk_tree_path_free(p);
  }
}

// ---- menus

static GtkWidget *new_menu(GtkWidget *from) {
  if (menu) gtk_widget_destroy(menu);  // one at a time; it lives until the next is opened
  menu = gtk_menu_new();
  g_signal_connect(menu, "destroy", G_CALLBACK(gtk_widget_destroyed), &menu);
  gtk_menu_attach_to_widget(GTK_MENU(menu), from, NULL);
  return menu;
}

static GtkWidget *item(GtkWidget *m, const char *label, GCallback cb, gpointer data) {
  GtkWidget *i = gtk_menu_item_new_with_label(label);
  if (cb) g_signal_connect(i, "activate", cb, data);
  gtk_menu_shell_append(GTK_MENU_SHELL(m), i);
  return i;
}

static void on_show(GtkMenuItem *i, const char *what) {
  const char *v = g_object_get_data(G_OBJECT(gtk_widget_get_parent(GTK_WIDGET(i))), what);
  if (!strcmp(what, "album")) focus_album(v);
  else focus_artist(v);
}

static void menu_extra(GtkWidget *m, guint n) {  // on this tab's songs
  item(m, n == 1 ? "Remove from Playlist" : "Remove Songs from Playlist", G_CALLBACK(on_remove), NULL);
}

void song_menu(GPtrArray *songs, GdkEvent *ev, GtkWidget *from) {
  if (!songs->len) return;
  GtkWidget *m = new_menu(from);
  char **uris = g_new0(char *, songs->len + 1);  // copied: the library may reload while the menu is open
  for (guint i = 0; i < songs->len; i++) uris[i] = g_strdup(((Song *)songs->pdata[i])->uri);
  g_object_set_data_full(G_OBJECT(m), "uris", uris, (GDestroyNotify)g_strfreev);

  GtkWidget *sub = gtk_menu_new();
  item(sub, "New Playlist…", G_CALLBACK(on_add_new), uris);
  GtkTreeModel *nm = GTK_TREE_MODEL(names);
  GtkTreeIter it;
  if (gtk_tree_model_get_iter_first(nm, &it)) gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
  for (gboolean more = gtk_tree_model_get_iter_first(nm, &it); more; more = gtk_tree_model_iter_next(nm, &it)) {
    char *n;
    gtk_tree_model_get(nm, &it, 0, &n, -1);
    g_object_set_data_full(G_OBJECT(item(sub, n, G_CALLBACK(on_add), uris)), "name", n, g_free);
  }
  if (songs->len == 1) {  // where this song lives
    Song *s = songs->pdata[0];
    char *key = g_strconcat(s->albumartist, "\x1f", s->album, NULL);
    g_object_set_data_full(G_OBJECT(m), "album", key, g_free);
    g_object_set_data_full(G_OBJECT(m), "artist", g_strdup(s->albumartist), g_free);
    if (library_album(key)) item(m, "Show Album", G_CALLBACK(on_show), "album");
    if (library_artist(s->albumartist)) item(m, "Show Artist", G_CALLBACK(on_show), "artist");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
  }
  item(m, "Play Next", G_CALLBACK(on_play_next), uris);
  item(m, "Add to Up Next", G_CALLBACK(on_up_next), uris);
  char *label = g_strdup_printf("Add %s to Playlist", songs->len == 1 ? "Song" : "Songs");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(item(m, label, NULL, NULL)), sub);
  g_free(label);
  void (*extra)(GtkWidget *, guint) = g_object_get_data(G_OBJECT(from), "menu-extra");
  if (extra) {
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    extra(m, songs->len);
  }

  gtk_widget_show_all(m);
  gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
}

static gboolean on_name_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
  (void)unused;
  GtkTreePath *path;
  if (!gdk_event_triggers_context_menu((GdkEvent *)ev) ||
      !gtk_tree_view_get_path_at_pos(list, ev->x, ev->y, &path, NULL, NULL, NULL)) return GDK_EVENT_PROPAGATE;
  gtk_tree_selection_select_path(gtk_tree_view_get_selection(list), path);
  gtk_tree_path_free(path);
  GtkWidget *m = new_menu(w);
  item(m, "Play", G_CALLBACK(on_play), NULL);
  item(m, "Rename…", G_CALLBACK(on_rename), NULL);
  item(m, "Delete…", G_CALLBACK(on_delete), NULL);
  gtk_widget_show_all(m);
  gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)ev);
  return GDK_EVENT_STOP;
}

static void on_name_activated(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *c, gpointer unused) {
  (void)v, (void)p, (void)c, (void)unused;
  on_play(NULL, NULL);
}

GtkWidget *playlists_page(void) {
  drafts = g_ptr_array_new_with_free_func(g_free);
  names = gtk_list_store_new(1, G_TYPE_STRING);
  list = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(names)));
  gtk_tree_view_set_headers_visible(list, FALSE);
  gtk_tree_view_set_enable_search(list, FALSE);
  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 8, "ypad", 6, NULL);
  gtk_tree_view_append_column(list, gtk_tree_view_column_new_with_attributes("", r, "text", 0, NULL));
  g_signal_connect(gtk_tree_view_get_selection(list), "changed", G_CALLBACK(on_pick), NULL);
  g_signal_connect(list, "button-press-event", G_CALLBACK(on_name_press), NULL);
  g_signal_connect(list, "row-activated", G_CALLBACK(on_name_activated), NULL);

  GtkWidget *add = gtk_button_new_with_label("+ New Playlist");
  gtk_style_context_add_class(gtk_widget_get_style_context(add), "newlist");
  g_signal_connect(add, "clicked", G_CALLBACK(on_new), NULL);
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(left), scrolled(GTK_WIDGET(list)), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(left), add, FALSE, FALSE, 4);
  gtk_widget_set_size_request(left, 240, -1);

  tracks = gtk_list_store_new(2, G_TYPE_POINTER, G_TYPE_POINTER);
  static const int cols[] = { NAME, TIME, ARTIST, ALBUM };
  detail = song_list(GTK_TREE_MODEL(tracks), cols, 4, TRUE);
  g_object_set_data(G_OBJECT(detail), "menu-extra", menu_extra);

  GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_pack1(GTK_PANED(pane), left, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(pane), scrolled(detail), TRUE, FALSE);
  return pane;
}
