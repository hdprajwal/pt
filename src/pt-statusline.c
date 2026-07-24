#include "pt-statusline.h"

struct _PtStatusline {
  GtkWidget parent_instance;
  GtkWidget *box;
  GtkWidget *label;   /* start: colored markup segments */
  GtkWidget *hint;    /* end: muted keybinding hint */
};

G_DEFINE_FINAL_TYPE(PtStatusline, pt_statusline, GTK_TYPE_WIDGET)

void pt_statusline_update(PtStatusline *sl, const char *project,
                          const char *branch, int changed,
                          int tab_idx, int tab_count,
                          int pane_idx, int pane_count) {
  /* project and branch are user-controlled — escape before markup. */
  char *proj_esc = g_markup_escape_text(project != NULL ? project : "-", -1);
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "<span foreground=\"#33d17a\">%s</span>", proj_esc);
  g_free(proj_esc);
  if (branch != NULL && branch[0] != '\0') {
    char *br_esc = g_markup_escape_text(branch, -1);
    g_string_append_printf(s, "   <span foreground=\"#c7c7c7\">%s</span>",
                           br_esc);
    g_free(br_esc);
    if (changed > 0)
      g_string_append_printf(s,
          " <span foreground=\"#e5c07b\">●%d</span>", changed);
    else
      g_string_append(s, " <span foreground=\"#33d17a\">✓</span>");
  }
  g_string_append_printf(s,
      "   <span foreground=\"#777777\">tab %d/%d · pane %d/%d</span>",
      tab_idx + 1, tab_count, pane_idx + 1, pane_count);
  gtk_label_set_markup(GTK_LABEL(sl->label), s->str);
  g_string_free(s, TRUE);

  gtk_label_set_text(GTK_LABEL(sl->hint),
                     "^1..9 projects · ^⇧T new tab");
}

static void pt_statusline_dispose(GObject *obj) {
  PtStatusline *sl = PT_STATUSLINE(obj);
  g_clear_pointer(&sl->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_statusline_parent_class)->dispose(obj);
}

static void pt_statusline_class_init(PtStatuslineClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_statusline_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
}

static void pt_statusline_init(PtStatusline *sl) {
  gtk_widget_add_css_class(GTK_WIDGET(sl), "pt-statusline");
  sl->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_parent(sl->box, GTK_WIDGET(sl));

  sl->label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(sl->label), 0.0f);
  gtk_box_append(GTK_BOX(sl->box), sl->label);

  GtkWidget *spacer = gtk_label_new(NULL);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(sl->box), spacer);

  sl->hint = gtk_label_new("");
  gtk_widget_add_css_class(sl->hint, "pt-status-hint");
  gtk_box_append(GTK_BOX(sl->box), sl->hint);
}

GtkWidget *pt_statusline_new(void) {
  return g_object_new(PT_TYPE_STATUSLINE, NULL);
}
