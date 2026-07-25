#include "pt-tab-strip.h"
#include "pt-session.h"   /* PT_ACCENT_COUNT */

enum { SIG_SELECTED, SIG_NEW, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtTabStrip {
  GtkWidget parent_instance;
  GtkWidget *box;
  /* Last-rendered state. Output-driven refreshes fire several times a second,
   * and a button destroyed between press and release cancels its gesture — the
   * click is silently swallowed. So re-render only when something changed. */
  PtTabInfo *last;         /* owned; last[i].title is g_strdup'd */
  int last_n, last_active;
  gboolean rendered;
};

G_DEFINE_FINAL_TYPE(PtTabStrip, pt_tab_strip, GTK_TYPE_WIDGET)

static void on_tab_clicked(GtkButton *btn, gpointer user) {
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pt-index"));
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_SELECTED], 0, idx);
}

static void on_new_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_TAB_STRIP(user), signals[SIG_NEW], 0);
}

static void clear_box(GtkWidget *box) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(box)) != NULL)
    gtk_box_remove(GTK_BOX(box), child);
}

static void add_accent_class(GtkWidget *wdg, int accent) {
  int a = accent % PT_ACCENT_COUNT;
  if (a < 0) a += PT_ACCENT_COUNT;
  char cls[8];
  g_snprintf(cls, sizeof(cls), "pt-a%d", a);
  gtk_widget_add_css_class(wdg, cls);
}

static void free_snapshot(PtTabStrip *s) {
  for (int i = 0; i < s->last_n; i++)
    g_free((char *)s->last[i].title);
  g_clear_pointer(&s->last, g_free);
  s->last_n = 0;
}

static gboolean same_as_rendered(PtTabStrip *s, const PtTabInfo *tabs, int n,
                                 int active) {
  if (!s->rendered || n != s->last_n || active != s->last_active) return FALSE;
  for (int i = 0; i < n; i++) {
    const PtTabInfo *a = &s->last[i], *b = &tabs[i];
    if (a->running != b->running || a->last_exit != b->last_exit ||
        a->unread != b->unread || a->accent != b->accent ||
        g_strcmp0(a->title, b->title) != 0)
      return FALSE;
  }
  return TRUE;
}

static void take_snapshot(PtTabStrip *s, const PtTabInfo *tabs, int n,
                          int active) {
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
  s->rendered = TRUE;
}

/* Full rebuild whenever the state moved: at ≤ ~10 tabs that is cheaper than
 * tracking per-child widgets, and the strip already rebuilt on every tab
 * switch. Identical state is a no-op (see the snapshot rationale above). */
void pt_tab_strip_set_tabs(PtTabStrip *s, const PtTabInfo *tabs, int n,
                           int active) {
  if (same_as_rendered(s, tabs, n, active)) return;
  take_snapshot(s, tabs, n, active);

  clear_box(s->box);
  for (int i = 0; i < n; i++) {
    const PtTabInfo *info = &tabs[i];
    gboolean is_active = (i == active);

    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "pt-tab");
    if (is_active) gtk_widget_add_css_class(btn, "active");
    g_object_set_data(G_OBJECT(btn), "pt-index", GINT_TO_POINTER(i));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);

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

    /* The active tab is being watched, so its unread count is meaningless. */
    if (info->unread > 0 && !is_active) {
      char txt[16];
      g_snprintf(txt, sizeof(txt), "%d", info->unread);
      GtkWidget *badge = gtk_label_new(txt);
      gtk_widget_add_css_class(badge, "pt-badge");
      add_accent_class(badge, info->accent);
      gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(row), badge);
    }

    gtk_button_set_child(GTK_BUTTON(btn), row);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked), s);
    gtk_box_append(GTK_BOX(s->box), btn);
  }

  GtkWidget *plus = gtk_button_new_with_label("+");
  gtk_widget_add_css_class(plus, "flat");
  gtk_widget_add_css_class(plus, "pt-tab-new");
  gtk_widget_set_valign(plus, GTK_ALIGN_CENTER);
  g_signal_connect(plus, "clicked", G_CALLBACK(on_new_clicked), s);
  gtk_box_append(GTK_BOX(s->box), plus);
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
}

static void pt_tab_strip_init(PtTabStrip *s) {
  gtk_widget_add_css_class(GTK_WIDGET(s), "pt-tabstrip");
  s->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(s->box, GTK_ALIGN_CENTER);
  gtk_widget_set_parent(s->box, GTK_WIDGET(s));
}

GtkWidget *pt_tab_strip_new(void) {
  return g_object_new(PT_TYPE_TAB_STRIP, NULL);
}
