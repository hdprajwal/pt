#include "pt-statusline.h"

struct _PtStatusline {
  GtkWidget parent_instance;
  GtkWidget *label;
};

G_DEFINE_FINAL_TYPE(PtStatusline, pt_statusline, GTK_TYPE_WIDGET)

void pt_statusline_update(PtStatusline *sl, const char *project,
                          const char *branch, int changed,
                          int tab_idx, int tab_count,
                          int pane_idx, int pane_count) {
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "%s", project != NULL ? project : "-");
  if (branch != NULL && branch[0] != '\0') {
    g_string_append_printf(s, "   %s", branch);
    if (changed > 0) g_string_append_printf(s, " ●%d", changed);
    else g_string_append(s, " ✓");
  }
  g_string_append_printf(s, "   tab %d/%d · pane %d/%d",
                         tab_idx + 1, tab_count, pane_idx + 1, pane_count);
  gtk_label_set_text(GTK_LABEL(sl->label), s->str);
  g_string_free(s, TRUE);
}

static void pt_statusline_dispose(GObject *obj) {
  PtStatusline *sl = PT_STATUSLINE(obj);
  g_clear_pointer(&sl->label, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_statusline_parent_class)->dispose(obj);
}

static void pt_statusline_class_init(PtStatuslineClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_statusline_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
}

static void pt_statusline_init(PtStatusline *sl) {
  gtk_widget_add_css_class(GTK_WIDGET(sl), "pt-statusline");
  sl->label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(sl->label), 0.0f);
  gtk_widget_set_parent(sl->label, GTK_WIDGET(sl));
}

GtkWidget *pt_statusline_new(void) {
  return g_object_new(PT_TYPE_STATUSLINE, NULL);
}
