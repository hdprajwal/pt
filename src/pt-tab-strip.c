#include "pt-tab-strip.h"

enum { SIG_SELECTED, SIG_NEW, SIG_CLOSE, SIG_OPEN_EDITOR, SIG_TOGGLE_PANEL,
       N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtTabStrip {
  GtkWidget parent_instance;
  GtkWidget *box;
  /* Last-rendered state. Output-driven refreshes fire several times a second,
   * and a button destroyed between press and release cancels its gesture — the
   * click is silently swallowed. So re-render only when something changed. */
  PtTabInfo *last;         /* owned; last[i].title is g_strdup'd */
  int last_n, last_active, last_accent;
  gboolean rendered;
  /* Probed once in init, not per rebuild: the strip rebuilds several times a
   * second and PATH does not move underneath a running window. */
  gboolean has_zed;
};

G_DEFINE_FINAL_TYPE(PtTabStrip, pt_tab_strip, GTK_TYPE_WIDGET)

/* The tab is a box, not a button: a GtkButton nested in a GtkButton is fragile,
 * so selection rides a click gesture (same shape as the sidebar's project row)
 * and the × stays a real button.
 *
 * A press on the × must NOT also select: selecting rebuilds the strip on the
 * spot, and a button destroyed between press and release never emits "clicked"
 * (the same hazard the rebuild dedupe exists for) — the close would be eaten by
 * the switch. The button does not claim the sequence early enough to stop this
 * gesture, so hit-test the press and stand down when it landed on the ×. */
static void on_tab_pressed(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n;
  GtkWidget *tab = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
  GtkWidget *close = g_object_get_data(G_OBJECT(tab), "pt-close");
  GtkWidget *hit = gtk_widget_pick(tab, x, y, GTK_PICK_DEFAULT);
  if (close != NULL && hit != NULL &&
      (hit == close || gtk_widget_is_ancestor(hit, close)))
    return;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tab), "pt-index"));
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_SELECTED], 0, idx);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer user) {
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pt-index"));
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_CLOSE], 0, idx);
}

static void on_new_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_NEW], 0);
}

static void on_zed_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_OPEN_EDITOR], 0);
}

static void on_panel_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_TOGGLE_PANEL], 0);
}

static void clear_box(GtkWidget *box) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(box)) != NULL)
    gtk_box_remove(GTK_BOX(box), child);
}

static void free_snapshot(PtTabStrip *s) {
  for (int i = 0; i < s->last_n; i++)
    g_free((char *)s->last[i].title);
  g_clear_pointer(&s->last, g_free);
  s->last_n = 0;
}

static gboolean same_as_rendered(PtTabStrip *s, const PtTabInfo *tabs, int n,
                                 int active, int accent) {
  if (!s->rendered || n != s->last_n || active != s->last_active ||
      accent != s->last_accent)
    return FALSE;
  for (int i = 0; i < n; i++) {
    const PtTabInfo *a = &s->last[i], *b = &tabs[i];
    if (a->running != b->running || a->last_exit != b->last_exit ||
        g_strcmp0(a->title, b->title) != 0)
      return FALSE;
  }
  return TRUE;
}

static void take_snapshot(PtTabStrip *s, const PtTabInfo *tabs, int n,
                          int active, int accent) {
  free_snapshot(s);
  if (n > 0) {
    s->last = g_new0(PtTabInfo, n);
    s->last_n = n;
    for (int i = 0; i < n; i++) {
      s->last[i] = tabs[i];
      s->last[i].title = g_strdup(tabs[i].title);
    }
  }
  s->last_active = active;
  s->last_accent = accent;
  s->rendered = TRUE;
}

/* Full rebuild whenever the state moved: at ≤ ~10 tabs that is cheaper than
 * tracking per-child widgets, and the strip already rebuilt on every tab
 * switch. Identical state is a no-op (see the snapshot rationale above). */
void pt_tab_strip_set_tabs(PtTabStrip *s, const PtTabInfo *tabs, int n,
                           int active, int accent) {
  if (same_as_rendered(s, tabs, n, active, accent)) return;
  take_snapshot(s, tabs, n, active, accent);

  clear_box(s->box);
  for (int i = 0; i < n; i++) {
    const PtTabInfo *info = &tabs[i];
    gboolean is_active = (i == active);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_add_css_class(row, "pt-tab");
    if (is_active) gtk_widget_add_css_class(row, "active");
    g_object_set_data(G_OBJECT(row), "pt-index", GINT_TO_POINTER(i));

    GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "pt-dot");
    gtk_widget_add_css_class(dot, "pt-dot-6");
    gtk_widget_add_css_class(dot, info->running     ? "running"
                                  : info->last_exit > 0 ? "error"
                                                        : "idle");
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), dot);

    GtkWidget *lbl = gtk_label_new(info->title != NULL ? info->title : "");
    gtk_widget_add_css_class(lbl, "pt-tab-label");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
    gtk_box_append(GTK_BOX(row), lbl);

    /* Hidden (opacity 0) until the tab is hovered — see .pt-tab-close in
     * style.css. It closes the whole tab, panes and all. */
    GtkWidget *close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "pt-tab-close");
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(close), "pt-index", GINT_TO_POINTER(i));
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), s);
    gtk_box_append(GTK_BOX(row), close);
    g_object_set_data(G_OBJECT(row), "pt-close", close);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), s);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(s->box), row);

    /* Zed-style tab bar: a full-height 1px border after every tab, the
     * active one included (see .pt-tab-sep). */
    GtkWidget *sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sep, "pt-tab-sep");
    gtk_box_append(GTK_BOX(s->box), sep);
  }

  /* Empty expanding box so the + sits at the strip's right edge, like Zed. */
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(s->box), spacer);

  GtkWidget *plus = gtk_button_new_with_label("+");
  gtk_widget_add_css_class(plus, "flat");
  gtk_widget_add_css_class(plus, "pt-tab-new");
  gtk_widget_set_valign(plus, GTK_ALIGN_CENTER);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_new_clicked), s);
  gtk_box_append(GTK_BOX(s->box), plus);

  /* Opens the active project in Zed. Omitted entirely when zed is not on
   * PATH — a button that cannot do anything is worse than no button. */
  if (s->has_zed) {
    GtkWidget *zed = gtk_button_new_from_icon_name("pt-zed-symbolic");
    gtk_widget_add_css_class(zed, "flat");
    gtk_widget_add_css_class(zed, "pt-tab-zed");
    gtk_widget_set_valign(zed, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(zed, "Open in Zed");
    g_signal_connect(zed, "clicked", G_CALLBACK(on_zed_clicked), s);
    gtk_box_append(GTK_BOX(s->box), zed);
  }

  /* Last in the cluster: toggles the info panel, same as ⌃I. Unlike the Zed
   * button this is never gated — the panel is always there to show. */
  GtkWidget *panel = gtk_button_new_from_icon_name("pt-panel-right-symbolic");
  gtk_widget_add_css_class(panel, "flat");
  gtk_widget_add_css_class(panel, "pt-tab-panel");
  gtk_widget_set_valign(panel, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(panel, "Toggle info panel  ^I");
  g_signal_connect(panel, "clicked", G_CALLBACK(on_panel_clicked), s);
  gtk_box_append(GTK_BOX(s->box), panel);
}

static void pt_tab_strip_dispose(GObject *obj) {
  PtTabStrip *s = PT_TAB_STRIP(obj);
  free_snapshot(s);
  g_clear_pointer(&s->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_tab_strip_parent_class)->dispose(obj);
}

static void pt_tab_strip_class_init(PtTabStripClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_tab_strip_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_SELECTED] = g_signal_new("tab-selected", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_NEW] = g_signal_new("tab-new", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_CLOSE] = g_signal_new("tab-close", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_OPEN_EDITOR] = g_signal_new("open-editor", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_TOGGLE_PANEL] = g_signal_new("toggle-panel", PT_TYPE_TAB_STRIP,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void pt_tab_strip_init(PtTabStrip *s) {
  gtk_widget_add_css_class(GTK_WIDGET(s), "pt-tabstrip");
  char *zed = g_find_program_in_path("zed");
  s->has_zed = (zed != NULL);
  g_free(zed);
  s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(s->box, GTK_ALIGN_CENTER);
  gtk_widget_set_parent(s->box, GTK_WIDGET(s));
}

GtkWidget *pt_tab_strip_new(void) {
  return g_object_new(PT_TYPE_TAB_STRIP, NULL);
}
