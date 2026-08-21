#include "pt-search-bar.h"

/* The find-in-scrollback bar: a GtkSearchEntry, the N/M count, and the two
 * match-stepping buttons, in a revealer so opening and closing slide. The
 * bar owns no search state of its own — it collects input and emits; the
 * window decides which pane is being searched and what happens on close.
 *
 * GtkSearchEntry already gives this widget its keyboard story for free:
 * Return fires "next-match", Shift+Return "previous-match", and Escape
 * "stop-search" — which is exactly the Enter/Shift+Enter/Esc contract the
 * bar promises, so no key controller of its own is needed. */

enum {
  SIG_CHANGED,
  SIG_NEXT,
  SIG_PREV,
  SIG_STOP,
  N_SIGNALS,
};
static guint signals[N_SIGNALS];

struct _PtSearchBar {
  GtkWidget parent_instance;
  GtkWidget *revealer;
  GtkWidget *entry;
  GtkWidget *count;
};

G_DEFINE_FINAL_TYPE(PtSearchBar, pt_search_bar, GTK_TYPE_WIDGET)

static void pt_search_bar_dispose(GObject *obj) {
  PtSearchBar *sb = PT_SEARCH_BAR(obj);
  g_clear_pointer(&sb->revealer, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_search_bar_parent_class)->dispose(obj);
}

static void pt_search_bar_class_init(PtSearchBarClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = pt_search_bar_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);

  signals[SIG_CHANGED] =
      g_signal_new("changed", PT_TYPE_SEARCH_BAR, G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_NEXT] =
      g_signal_new("next-match", PT_TYPE_SEARCH_BAR, G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_PREV] =
      g_signal_new("previous-match", PT_TYPE_SEARCH_BAR, G_SIGNAL_RUN_LAST,
                   0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_STOP] =
      g_signal_new("stopped", PT_TYPE_SEARCH_BAR, G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void on_entry_changed(GtkSearchEntry *e, gpointer user) {
  g_signal_emit(user, signals[SIG_CHANGED], 0,
                gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_next(GtkSearchEntry *e, gpointer user) {
  (void)e;
  g_signal_emit(user, signals[SIG_NEXT], 0);
}

static void on_prev(GtkSearchEntry *e, gpointer user) {
  (void)e;
  g_signal_emit(user, signals[SIG_PREV], 0);
}

static void on_stop(GtkSearchEntry *e, gpointer user) {
  (void)e;
  /* Esc in the entry asks to close; the window clears the highlights and
     hands focus back to the terminal when this comes back. */
  g_signal_emit(user, signals[SIG_STOP], 0);
}

GtkWidget *pt_search_bar_new(void) {
  return g_object_new(PT_TYPE_SEARCH_BAR, NULL);
}

static void pt_search_bar_init(PtSearchBar *sb) {
  gtk_widget_add_css_class(GTK_WIDGET(sb), "pt-search-bar");
  sb->revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(sb->revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_widget_set_parent(sb->revealer, GTK_WIDGET(sb));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(box, "pt-search-bar-box");
  gtk_revealer_set_child(GTK_REVEALER(sb->revealer), box);

  sb->entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(sb->entry, TRUE);
  gtk_box_append(GTK_BOX(box), sb->entry);
  g_signal_connect(sb->entry, "search-changed",
                   G_CALLBACK(on_entry_changed), sb);
  g_signal_connect(sb->entry, "next-match", G_CALLBACK(on_next), sb);
  g_signal_connect(sb->entry, "previous-match", G_CALLBACK(on_prev), sb);
  g_signal_connect(sb->entry, "stop-search", G_CALLBACK(on_stop), sb);

  sb->count = gtk_label_new(NULL);
  gtk_widget_add_css_class(sb->count, "pt-search-count");
  gtk_label_set_xalign(GTK_LABEL(sb->count), 1.0f);
  gtk_box_append(GTK_BOX(box), sb->count);

  struct { const char *label; const char *tip; GCallback cb; } buttons[] = {
    { "↑", "previous match (Shift+Return)", G_CALLBACK(on_prev) },
    { "↓", "next match (Return)", G_CALLBACK(on_next) },
  };
  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) {
    GtkWidget *b = gtk_button_new_with_label(buttons[i].label);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, buttons[i].tip);
    gtk_box_append(GTK_BOX(box), b);
    /* Plain connect: the handlers ignore their first argument and emit on
       user data, which is the bar here exactly as it is from the entry. */
    g_signal_connect(b, "clicked", buttons[i].cb, sb);
  }
}

void pt_search_bar_open(PtSearchBar *sb) {
  g_return_if_fail(PT_IS_SEARCH_BAR(sb));
  gtk_revealer_set_reveal_child(GTK_REVEALER(sb->revealer), TRUE);
  gtk_widget_grab_focus(sb->entry);
  /* Whatever was searched before is usually about to be searched again:
     select it all so the first keystroke replaces it. */
  gtk_editable_select_region(GTK_EDITABLE(sb->entry), 0, -1);
}

void pt_search_bar_close(PtSearchBar *sb) {
  g_return_if_fail(PT_IS_SEARCH_BAR(sb));
  gtk_revealer_set_reveal_child(GTK_REVEALER(sb->revealer), FALSE);
}

gboolean pt_search_bar_is_open(PtSearchBar *sb) {
  g_return_val_if_fail(PT_IS_SEARCH_BAR(sb), FALSE);
  return gtk_revealer_get_reveal_child(GTK_REVEALER(sb->revealer));
}

const char *pt_search_bar_text(PtSearchBar *sb) {
  g_return_val_if_fail(PT_IS_SEARCH_BAR(sb), "");
  const char *text = gtk_editable_get_text(GTK_EDITABLE(sb->entry));
  return text != NULL ? text : "";
}

void pt_search_bar_set_count(PtSearchBar *sb, int current, int count) {
  g_return_if_fail(PT_IS_SEARCH_BAR(sb));
  char *txt;
  if (count <= 0)
    txt = g_strdup(current >= 0 ? "no results" : NULL);
  else
    txt = g_strdup_printf("%d/%d", current < 0 ? 1 : current + 1, count);
  gtk_label_set_text(GTK_LABEL(sb->count), txt != NULL ? txt : "");
  g_free(txt);
}
