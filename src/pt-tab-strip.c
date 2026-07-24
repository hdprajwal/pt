#include "pt-tab-strip.h"

enum { SIG_SELECTED, SIG_NEW, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtTabStrip {
  GtkWidget parent_instance;
  GtkWidget *box;
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

void pt_tab_strip_set_tabs(PtTabStrip *s, GPtrArray *titles, int active) {
  clear_box(s->box);
  for (guint i = 0; i < titles->len; i++) {
    char *label = g_strdup_printf("%u:%s", i + 1,
                                  (const char *)g_ptr_array_index(titles, i));
    GtkWidget *btn = gtk_button_new_with_label(label);
    g_free(label);
    gtk_widget_add_css_class(btn, "pt-tab");
    if ((int)i == active) gtk_widget_add_css_class(btn, "active");
    g_object_set_data(G_OBJECT(btn), "pt-index", GINT_TO_POINTER((int)i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_clicked), s);
    gtk_box_append(GTK_BOX(s->box), btn);
  }
  GtkWidget *plus = gtk_button_new_with_label("+");
  gtk_widget_add_css_class(plus, "pt-tab");
  g_signal_connect(plus, "clicked", G_CALLBACK(on_new_clicked), s);
  gtk_box_append(GTK_BOX(s->box), plus);
}

void pt_tab_strip_set_activity(PtTabStrip *s, int index, gboolean on) {
  GtkWidget *child = gtk_widget_get_first_child(s->box);
  for (int i = 0; child != NULL && i < index; i++)
    child = gtk_widget_get_next_sibling(child);
  if (child == NULL) return;
  if (on) gtk_widget_add_css_class(child, "activity");
  else gtk_widget_remove_css_class(child, "activity");
}

static void pt_tab_strip_dispose(GObject *obj) {
  PtTabStrip *s = PT_TAB_STRIP(obj);
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
  gtk_widget_set_parent(s->box, GTK_WIDGET(s));
}

GtkWidget *pt_tab_strip_new(void) {
  return g_object_new(PT_TYPE_TAB_STRIP, NULL);
}
