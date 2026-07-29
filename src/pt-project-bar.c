#include "pt-project-bar.h"
#include "pt-accent.h"
#include <string.h>

struct _PtProjectBar {
  GtkWidget parent_instance;
  GtkWidget *handle;   /* GtkWindowHandle — the bar drags the window */
  GtkWidget *name;
  GtkWidget *path;
  GtkWidget *chip;     /* branch + dirty count; hidden when not a repo */
};

G_DEFINE_FINAL_TYPE(PtProjectBar, pt_project_bar, GTK_TYPE_WIDGET)

/* Only a whole leading path component matches, so "/home/metoo" is left
 * alone. See the header for the contract. */
char *pt_path_home_abbrev(const char *path) {
  const char *home = g_get_home_dir();
  if (path == NULL) return g_strdup("");
  if (home == NULL || home[0] == '\0') return g_strdup(path);
  gsize n = strlen(home);
  while (n > 1 && home[n - 1] == '/') n--;      /* tolerate a trailing slash */
  if (strncmp(path, home, n) != 0) return g_strdup(path);
  if (path[n] == '\0') return g_strdup("~");
  if (path[n] != '/') return g_strdup(path);
  return g_strconcat("~", path + n, NULL);
}

void pt_project_bar_update(PtProjectBar *b, const char *name, const char *path,
                           const PtGitStatus *git, int accent) {
  g_return_if_fail(PT_IS_PROJECT_BAR(b));

  gtk_label_set_text(GTK_LABEL(b->name), name != NULL ? name : "pt");

  char *shown = pt_path_home_abbrev(path);
  gtk_label_set_text(GTK_LABEL(b->path), shown);
  g_free(shown);

  /* Same formatter as the sidebar row's branch label — one spelling, one
   * place. Empty text is exactly the "nothing to show" case. */
  char chip[192];
  pt_git_format_chip(git, chip, sizeof chip);
  gboolean has_branch = chip[0] != '\0';
  if (has_branch) gtk_label_set_text(GTK_LABEL(b->chip), chip);
  gtk_widget_set_visible(b->chip, has_branch);

  /* The chip is the bar's only accented element. */
  pt_accent_set_class(b->chip, accent);
}

static void pt_project_bar_dispose(GObject *obj) {
  PtProjectBar *b = PT_PROJECT_BAR(obj);
  g_clear_pointer(&b->handle, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_project_bar_parent_class)->dispose(obj);
}

static void pt_project_bar_class_init(PtProjectBarClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_project_bar_dispose;
  gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(klass),
                                           GTK_TYPE_BIN_LAYOUT);
}

static GtkWidget *hint_pill(const char *text) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "pt-hint-pill");
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  return l;
}

static void pt_project_bar_init(PtProjectBar *b) {
  b->handle = gtk_window_handle_new();
  gtk_widget_set_parent(b->handle, GTK_WIDGET(b));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "pt-projectbar");
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(b->handle), box);

  b->name = gtk_label_new("pt");
  gtk_label_set_xalign(GTK_LABEL(b->name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(b->name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(b->name, "pt-proj-name");
  gtk_box_append(GTK_BOX(box), b->name);

  b->path = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(b->path), 0.0f);
  /* the tail of a path identifies it, so drop the head when space is tight */
  gtk_label_set_ellipsize(GTK_LABEL(b->path), PANGO_ELLIPSIZE_START);
  gtk_widget_set_hexpand(b->path, FALSE);
  gtk_widget_add_css_class(b->path, "pt-proj-path");
  gtk_box_append(GTK_BOX(box), b->path);

  b->chip = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(b->chip), PANGO_ELLIPSIZE_END);
  gtk_widget_set_valign(b->chip, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(b->chip, "pt-chip");
  gtk_widget_set_visible(b->chip, FALSE);
  gtk_box_append(GTK_BOX(box), b->chip);

  GtkWidget *spacer = gtk_label_new(NULL);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(box), spacer);

  gtk_box_append(GTK_BOX(box), hint_pill("^⇧S split"));
  gtk_box_append(GTK_BOX(box), hint_pill("^K"));

  GtkWidget *controls = gtk_window_controls_new(GTK_PACK_END);
  gtk_widget_set_valign(controls, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), controls);
}

GtkWidget *pt_project_bar_new(void) {
  return g_object_new(PT_TYPE_PROJECT_BAR, NULL);
}
