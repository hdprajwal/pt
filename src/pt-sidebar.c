#include "pt-sidebar.h"

enum { SIG_SELECTED, SIG_ADD, SIG_REMOVE, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtSidebar {
  GtkWidget parent_instance;
  GtkWidget *box;      /* vertical: heading, rows..., stretch, add button */
  GtkWidget *rows_box;
};

G_DEFINE_FINAL_TYPE(PtSidebar, pt_sidebar, GTK_TYPE_WIDGET)

static void on_row_clicked(GtkGestureClick *g, int n, double x, double y,
                           gpointer user) {
  (void)n; (void)x; (void)y;
  GtkWidget *row = gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(g));
  PtSidebar *sb = user;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "pt-index"));
  g_signal_emit(sb, signals[SIG_SELECTED], 0, idx);
}

static void on_remove_clicked(GtkButton *btn, gpointer user) {
  PtSidebar *sb = user;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pt-index"));
  g_signal_emit(sb, signals[SIG_REMOVE], 0, idx);
}

static void on_add_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_SIDEBAR(user), signals[SIG_ADD], 0);
}

void pt_sidebar_set_projects(PtSidebar *sb, const PtSidebarRow *rows,
                             int n_rows, int active) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(sb->rows_box)) != NULL)
    gtk_box_remove(GTK_BOX(sb->rows_box), child);

  for (int i = 0; i < n_rows; i++) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(row, "pt-project-row");
    if (i == active) gtk_widget_add_css_class(row, "active");
    g_object_set_data(G_OBJECT(row), "pt-index", GINT_TO_POINTER(i));

    /* Two-line column: project name on top, branch (+ dirty state) below. */
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *name = gtk_label_new(rows[i].name);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(col), name);

    if (rows[i].missing) {
      GtkWidget *branch = gtk_label_new("[missing]");
      gtk_label_set_xalign(GTK_LABEL(branch), 0.0f);
      gtk_widget_add_css_class(branch, "pt-branch");
      gtk_widget_add_css_class(branch, "pt-badge-dirty");
      gtk_box_append(GTK_BOX(col), branch);
    } else if (rows[i].is_repo) {
      char *btxt = rows[i].changed > 0
          ? g_strdup_printf("%s ●%d", rows[i].branch, rows[i].changed)
          : g_strdup(rows[i].branch);
      GtkWidget *branch = gtk_label_new(btxt);
      g_free(btxt);
      gtk_label_set_xalign(GTK_LABEL(branch), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(branch), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class(branch, "pt-branch");
      if (rows[i].changed > 0) gtk_widget_add_css_class(branch, "dirty");
      gtk_box_append(GTK_BOX(col), branch);
    }
    gtk_box_append(GTK_BOX(row), col);

    /* spacer keeps the column left and right-aligns hint + remove */
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);

    if (i < 9) {
      char khint[8];
      g_snprintf(khint, sizeof(khint), "^%d", i + 1);
      GtkWidget *kbd = gtk_label_new(khint);
      gtk_widget_add_css_class(kbd, "pt-kbd-hint");
      gtk_widget_set_valign(kbd, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(row), kbd);
    }

    GtkWidget *rm = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(rm, "flat");
    gtk_widget_add_css_class(rm, "pt-remove");
    gtk_widget_set_valign(rm, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(rm), "pt-index", GINT_TO_POINTER(i));
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_clicked), sb);
    gtk_box_append(GTK_BOX(row), rm);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_row_clicked), sb);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(sb->rows_box), row);
  }
}

static void pt_sidebar_dispose(GObject *obj) {
  PtSidebar *sb = PT_SIDEBAR(obj);
  g_clear_pointer(&sb->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_sidebar_parent_class)->dispose(obj);
}

static void pt_sidebar_class_init(PtSidebarClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_sidebar_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
  signals[SIG_SELECTED] = g_signal_new("project-selected", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIG_ADD] = g_signal_new("project-add", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REMOVE] = g_signal_new("project-remove", PT_TYPE_SIDEBAR,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void pt_sidebar_init(PtSidebar *sb) {
  gtk_widget_add_css_class(GTK_WIDGET(sb), "pt-sidebar");
  gtk_widget_set_size_request(GTK_WIDGET(sb), 190, -1);
  sb->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(sb->box, GTK_WIDGET(sb));

  GtkWidget *heading = gtk_label_new("PROJECTS");
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_widget_add_css_class(heading, "pt-sidebar-heading");
  gtk_box_append(GTK_BOX(sb->box), heading);

  sb->rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(sb->rows_box, TRUE);
  gtk_box_append(GTK_BOX(sb->box), sb->rows_box);

  GtkWidget *add = gtk_button_new_with_label("+ project");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_add_css_class(add, "pt-add-project");
  gtk_widget_set_halign(add, GTK_ALIGN_FILL);
  GtkWidget *add_label = gtk_button_get_child(GTK_BUTTON(add));
  if (GTK_IS_LABEL(add_label))
    gtk_label_set_xalign(GTK_LABEL(add_label), 0.0f);
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_clicked), sb);
  gtk_box_append(GTK_BOX(sb->box), add);
}

GtkWidget *pt_sidebar_new(void) {
  return g_object_new(PT_TYPE_SIDEBAR, NULL);
}
