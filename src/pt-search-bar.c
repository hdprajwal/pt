#include "pt-search-bar.h"

/* The find-in-scrollback bar: a GtkSearchEntry, the N/M count, and the two
 * match-stepping buttons, in a revealer so opening and closing slide. The
 * bar owns no search state of its own — it collects input and emits; the
 * window decides which pane is being searched and what happens on close.
 *
 * GtkSearchEntry gives away two thirds of this widget's keyboard story:
 * Escape fires "stop-search", and the entry's own delayed "search-changed"
 * exists for exactly the debounce a search wants. It does NOT give away
 * Enter. Its default binding for "next-match" is Ctrl+G and for
 * "previous-match" Ctrl+Shift+G; Enter emits "activate" instead, and
 * Shift+Enter matches no binding at all (GTK's Enter triggers carry an
 * empty modifier mask). Connecting the two match signals and calling it
 * done left the Enter and Shift+Enter the README promises doing nothing
 * whatsoever, which is the whole point of a find bar.
 *
 * So the Enter half is spelled out below with a key controller, in the
 * CAPTURE phase and answering GDK_EVENT_STOP: capture reaches this widget
 * before the inner GtkText and before this class's own bubble-phase
 * bindings, so exactly one step happens per press whatever GTK decides
 * Enter means. The Ctrl+G pair still works — those signals are still
 * connected, and they are somebody's muscle memory.
 *
 * None of this is under test: a key controller needs real GDK key events
 * and a display, which the suite has none of. */

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

/* Enter steps to the next match, Shift+Enter to the previous one. Exactly
   those two chords: an Enter carrying Ctrl, Alt or Super belongs to whoever
   bound it and travels on. */
static gboolean on_entry_key(GtkEventControllerKey *ctl, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer user) {
  (void)ctl; (void)keycode;
  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter &&
      keyval != GDK_KEY_ISO_Enter)
    return GDK_EVENT_PROPAGATE;
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
  if ((mods & ~GDK_SHIFT_MASK) != 0) return GDK_EVENT_PROPAGATE;
  g_signal_emit(user, signals[(mods & GDK_SHIFT_MASK) ? SIG_PREV : SIG_NEXT],
                0);
  return GDK_EVENT_STOP;
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
  /* The entry's own 150ms delay on top of the window's 100ms debounce is a
     quarter second before the first highlight, for one throttle's worth of
     benefit. The window's is the one that matters — it is the one that knows
     a query walks every grid ref — so this hands "changed" over per
     keystroke and lets that one do the waiting. It also makes a pending
     debounce there mean exactly "the typed text has not been searched yet",
     which is what Enter checks before it steps. */
  gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(sb->entry), 0);
  gtk_box_append(GTK_BOX(box), sb->entry);
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), sb);
  gtk_widget_add_controller(sb->entry, keys);
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
