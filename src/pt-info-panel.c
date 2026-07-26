#include "pt-info-panel.h"
#include "pt-accent.h"

#include <string.h>

/* Mirror of the sidebar rail on the other edge of the window. */
#define PT_INFO_PANEL_WIDTH 266

enum { SIG_OPEN_EDITOR, SIG_OPEN_FILES, SIG_COPY_PATH, SIG_REFRESH,
       N_SIGNALS };
static guint signals[N_SIGNALS];

struct _PtInfoPanel {
  GtkWidget parent_instance;
  GtkWidget *box;                  /* vertical: info section, git section */
  GtkWidget *dot, *shell, *pid, *dir, *zed;
  /* The git section's header row IS the branch line: icon, branch, count,
   * ahead/behind — or the icon and `not_repo` when there is no repo. */
  GtkWidget *branch_icon, *branch, *count, *ab, *not_repo;
  GtkWidget *scroller, *files_box;
  GPtrArray *files;                /* PtGitFile*, the list currently rendered */
  int accent;                      /* -1 until the first set_info */
};

G_DEFINE_FINAL_TYPE(PtInfoPanel, pt_info_panel, GTK_TYPE_WIDGET)

/* ---------- callbacks ---------- */
static void on_refresh_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_REFRESH], 0);
}

static void on_zed_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_OPEN_EDITOR], 0);
}

static void on_files_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_OPEN_FILES], 0);
}

static void on_copy_clicked(GtkButton *btn, gpointer user) {
  (void)btn;
  g_signal_emit(PT_INFO_PANEL(user), signals[SIG_COPY_PATH], 0);
}

/* ---------- file list ---------- */
/* M and R are edits, A additions, D removals, ?? untracked; anything else
 * (a typechange, a half-resolved conflict) falls back to dim rather than
 * borrowing a colour that would misreport it. */
static const char *status_class(const char *xy) {
  switch (xy[0]) {
    case 'M': case 'R': return "m";
    case 'A':           return "a";
    case 'D':           return "d";
    default:            return "u";
  }
}

static void rebuild_files(PtInfoPanel *ip) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(ip->files_box)) != NULL)
    gtk_box_remove(GTK_BOX(ip->files_box), child);

  for (guint i = 0; i < ip->files->len; i++) {
    const PtGitFile *f = g_ptr_array_index(ip->files, i);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(row, "pt-info-file");

    GtkWidget *st = gtk_label_new(f->xy);
    gtk_widget_add_css_class(st, "pt-info-st");
    gtk_widget_add_css_class(st, status_class(f->xy));
    gtk_label_set_xalign(GTK_LABEL(st), 0.5f);
    gtk_box_append(GTK_BOX(row), st);

    const char *slash = strrchr(f->path, '/');
    if (slash != NULL) {
      char *dir = g_strndup(f->path, (gsize)(slash - f->path) + 1);
      GtkWidget *dl = gtk_label_new(dir);
      g_free(dir);
      gtk_widget_add_css_class(dl, "pt-info-fdir");
      gtk_label_set_ellipsize(GTK_LABEL(dl), PANGO_ELLIPSIZE_START);
      gtk_box_append(GTK_BOX(row), dl);
    }
    /* The name takes the slack and ellipsizes: the counts must keep their
     * place at the right edge of a 266px rail, whatever the path length. */
    GtkWidget *name = gtk_label_new(slash != NULL ? slash + 1 : f->path);
    gtk_widget_add_css_class(name, "pt-info-fname");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(row), name);

    /* Untracked files never reach the diff, and binary ones report no lines:
     * both leave the counts off entirely rather than printing a fake 0. */
    if (f->add >= 0 && f->del >= 0) {
      char buf[24];
      g_snprintf(buf, sizeof(buf), "+%d", f->add);
      GtkWidget *add = gtk_label_new(buf);
      gtk_widget_add_css_class(add, "pt-info-add");
      gtk_box_append(GTK_BOX(row), add);
      g_snprintf(buf, sizeof(buf), "−%d", f->del);
      GtkWidget *del = gtk_label_new(buf);
      gtk_widget_add_css_class(del, "pt-info-del");
      gtk_box_append(GTK_BOX(row), del);
    }

    gtk_box_append(GTK_BOX(ip->files_box), row);
  }
}

static gboolean same_files(PtInfoPanel *ip, GPtrArray *files) {
  guint n = files != NULL ? files->len : 0;
  if (n != ip->files->len) return FALSE;
  for (guint i = 0; i < n; i++) {
    const PtGitFile *a = g_ptr_array_index(ip->files, i);
    const PtGitFile *b = g_ptr_array_index(files, i);
    /* Counts too: they arrive one poll behind the paths, and skipping the
     * rebuild for them would leave every row blank forever. */
    if (strcmp(a->xy, b->xy) != 0 || g_strcmp0(a->path, b->path) != 0 ||
        a->add != b->add || a->del != b->del)
      return FALSE;
  }
  return TRUE;
}

/* ---------- public API ---------- */
void pt_info_panel_set_info(PtInfoPanel *ip, const char *shell, int pid,
                            const char *dir, int accent) {
  gtk_label_set_text(GTK_LABEL(ip->shell), shell != NULL ? shell : "");
  char buf[32] = "";
  if (pid > 0) g_snprintf(buf, sizeof(buf), "pid %d", pid);
  gtk_label_set_text(GTK_LABEL(ip->pid), buf);
  gtk_label_set_text(GTK_LABEL(ip->dir), dir != NULL ? dir : "");
  if (accent != ip->accent) {
    pt_accent_set_class(ip->dot, accent);
    ip->accent = accent;
  }
}

/* The window refreshes this twice a second, so the file rows are rebuilt only
 * when the list itself moved — same reasoning as the sidebar's row dedupe. */
void pt_info_panel_set_git(PtInfoPanel *ip, const PtGitStatus *st,
                           gboolean is_repo, GPtrArray *files) {
  gtk_widget_set_visible(ip->branch, is_repo);
  gtk_widget_set_visible(ip->scroller, is_repo);
  gtk_widget_set_visible(ip->not_repo, !is_repo);
  /* The icon stays put in both states and only drops to the ghost tone. */
  if (is_repo) gtk_widget_remove_css_class(ip->branch_icon, "empty");
  else         gtk_widget_add_css_class(ip->branch_icon, "empty");

  char buf[32] = "";
  if (is_repo && st != NULL && st->changed > 0)
    g_snprintf(buf, sizeof(buf), "%d", st->changed);
  gtk_label_set_text(GTK_LABEL(ip->count), buf);
  gtk_widget_set_visible(ip->count, buf[0] != '\0');

  char ab[32] = "";
  if (is_repo && st != NULL) {
    gtk_label_set_text(GTK_LABEL(ip->branch), st->branch);
    if (st->ahead > 0 && st->behind > 0)
      g_snprintf(ab, sizeof(ab), "↑%d ↓%d", st->ahead, st->behind);
    else if (st->ahead > 0)  g_snprintf(ab, sizeof(ab), "↑%d", st->ahead);
    else if (st->behind > 0) g_snprintf(ab, sizeof(ab), "↓%d", st->behind);
    gtk_label_set_text(GTK_LABEL(ip->ab), ab);
  }
  gtk_widget_set_visible(ip->ab, ab[0] != '\0');

  if (!is_repo) files = NULL;
  if (same_files(ip, files)) return;
  g_ptr_array_unref(ip->files);
  ip->files = pt_git_files_copy(files);
  rebuild_files(ip);
}

void pt_info_panel_set_has_zed(PtInfoPanel *ip, gboolean has_zed) {
  gtk_widget_set_visible(ip->zed, has_zed);
}

/* ---------- GObject ---------- */
static void pt_info_panel_dispose(GObject *obj) {
  PtInfoPanel *ip = PT_INFO_PANEL(obj);
  g_clear_pointer(&ip->files, g_ptr_array_unref);
  g_clear_pointer(&ip->box, gtk_widget_unparent);
  G_OBJECT_CLASS(pt_info_panel_parent_class)->dispose(obj);
}

/* Same fix as the sidebar: a size request only raises the minimum, and the
 * wrapping directory label reports its full text width as natural, which would
 * let a deep path widen the panel and steal that width from the terminal.
 * Reporting minimum == natural pins the rail. */
static void pt_info_panel_measure(GtkWidget *widget, GtkOrientation orientation,
                                  int for_size, int *minimum, int *natural,
                                  int *minimum_baseline,
                                  int *natural_baseline) {
  (void)for_size;
  PtInfoPanel *ip = PT_INFO_PANEL(widget);
  *minimum_baseline = *natural_baseline = -1;
  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
    *minimum = *natural = PT_INFO_PANEL_WIDTH;
    return;
  }
  if (ip->box == NULL) { *minimum = *natural = 0; return; }
  gtk_widget_measure(ip->box, orientation, PT_INFO_PANEL_WIDTH,
                     minimum, natural, minimum_baseline, natural_baseline);
}

static void pt_info_panel_size_allocate(GtkWidget *widget, int width,
                                        int height, int baseline) {
  PtInfoPanel *ip = PT_INFO_PANEL(widget);
  if (ip->box != NULL)
    gtk_widget_allocate(ip->box, width, height, baseline, NULL);
}

static void pt_info_panel_class_init(PtInfoPanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_info_panel_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  wc->measure = pt_info_panel_measure;
  wc->size_allocate = pt_info_panel_size_allocate;
  signals[SIG_OPEN_EDITOR] = g_signal_new("open-editor", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_OPEN_FILES] = g_signal_new("open-files", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_COPY_PATH] = g_signal_new("copy-path", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIG_REFRESH] = g_signal_new("refresh", PT_TYPE_INFO_PANEL,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static GtkWidget *section_head(const char *text) {
  GtkWidget *l = gtk_label_new(text);
  gtk_widget_add_css_class(l, "pt-info-head");
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  return l;
}

/* `icon` NULL keeps the plain text button; a name puts the symbolic icon and
 * the label side by side in one child box. */
static GtkWidget *action_button(PtInfoPanel *ip, const char *icon,
                                const char *label, GCallback cb) {
  GtkWidget *b;
  if (icon == NULL) {
    b = gtk_button_new_with_label(label);
  } else {
    b = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), img);
    gtk_box_append(GTK_BOX(row), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), row);
  }
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_add_css_class(b, "pt-info-btn");
  g_signal_connect(b, "clicked", cb, ip);
  return b;
}

static void pt_info_panel_init(PtInfoPanel *ip) {
  gtk_widget_add_css_class(GTK_WIDGET(ip), "pt-infopanel");
  gtk_widget_set_size_request(GTK_WIDGET(ip), PT_INFO_PANEL_WIDTH, -1);
  ip->accent = -1;
  ip->files = pt_git_files_copy(NULL);   /* empty, with the right free func */

  ip->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_parent(ip->box, GTK_WIDGET(ip));

  /* ---- INFO ---- */
  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(info, "pt-info-sect");

  GtkWidget *info_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(info_head), section_head("INFO"));
  GtkWidget *refresh = gtk_button_new_with_label("↻");
  gtk_widget_add_css_class(refresh, "flat");
  gtk_widget_add_css_class(refresh, "pt-info-refresh");
  gtk_widget_set_hexpand(refresh, TRUE);
  gtk_widget_set_halign(refresh, GTK_ALIGN_END);
  gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(refresh, "Refresh");
  g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), ip);
  gtk_box_append(GTK_BOX(info_head), refresh);
  gtk_box_append(GTK_BOX(info), info_head);

  GtkWidget *ident = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  ip->dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(ip->dot, "pt-dot");
  gtk_widget_add_css_class(ip->dot, "pt-dot-7");
  gtk_widget_set_valign(ip->dot, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(ident), ip->dot);
  ip->shell = gtk_label_new("");
  gtk_widget_add_css_class(ip->shell, "pt-info-shell");
  gtk_label_set_ellipsize(GTK_LABEL(ip->shell), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(ident), ip->shell);
  ip->pid = gtk_label_new("");
  gtk_widget_add_css_class(ip->pid, "pt-info-pid");
  gtk_box_append(GTK_BOX(ident), ip->pid);
  gtk_box_append(GTK_BOX(info), ident);

  ip->dir = gtk_label_new("");
  gtk_widget_add_css_class(ip->dir, "pt-info-path");
  gtk_label_set_xalign(GTK_LABEL(ip->dir), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(ip->dir), TRUE);
  /* A path has no spaces to break on, so CHAR is the only mode that wraps it
   * instead of forcing the panel wider. */
  gtk_label_set_wrap_mode(GTK_LABEL(ip->dir), PANGO_WRAP_WORD_CHAR);
  gtk_box_append(GTK_BOX(info), ip->dir);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(actions, GTK_ALIGN_START);
  ip->zed = action_button(ip, "pt-zed-symbolic", "Zed",
                          G_CALLBACK(on_zed_clicked));
  gtk_box_append(GTK_BOX(actions), ip->zed);
  gtk_box_append(GTK_BOX(actions),
                 action_button(ip, NULL, "⊞ Files",
                               G_CALLBACK(on_files_clicked)));
  gtk_box_append(GTK_BOX(actions),
                 action_button(ip, NULL, "⧉ Copy",
                               G_CALLBACK(on_copy_clicked)));
  gtk_box_append(GTK_BOX(info), actions);
  gtk_box_append(GTK_BOX(ip->box), info);

  /* ---- GIT ---- */
  GtkWidget *git = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(git, "pt-info-sect");
  gtk_widget_add_css_class(git, "git");
  gtk_widget_set_vexpand(git, TRUE);

  /* The branch line doubles as the section header: no "GIT" caption above it,
   * the icon says which section this is. */
  GtkWidget *git_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  ip->branch_icon = gtk_image_new_from_icon_name("pt-git-branch-symbolic");
  gtk_widget_add_css_class(ip->branch_icon, "pt-info-branch-icon");
  gtk_widget_set_valign(ip->branch_icon, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(git_head), ip->branch_icon);
  ip->branch = gtk_label_new("");
  gtk_widget_add_css_class(ip->branch, "pt-info-branch");
  gtk_label_set_ellipsize(GTK_LABEL(ip->branch), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(git_head), ip->branch);
  ip->count = gtk_label_new("");
  gtk_widget_add_css_class(ip->count, "pt-info-count");
  gtk_widget_set_visible(ip->count, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->count);
  ip->ab = gtk_label_new("");
  gtk_widget_add_css_class(ip->ab, "pt-info-ab");
  gtk_widget_set_visible(ip->ab, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->ab);
  ip->not_repo = gtk_label_new("no git repo found");
  gtk_widget_add_css_class(ip->not_repo, "pt-info-empty");
  gtk_widget_set_visible(ip->not_repo, FALSE);
  gtk_box_append(GTK_BOX(git_head), ip->not_repo);
  gtk_box_append(GTK_BOX(git), git_head);

  /* EXTERNAL horizontally: no h-scrollbar, and the file rows contribute
   * nothing to the panel's width request (see the measure override). */
  ip->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ip->scroller),
                                 GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(ip->scroller, TRUE);
  ip->files_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ip->scroller),
                                ip->files_box);
  gtk_box_append(GTK_BOX(git), ip->scroller);
  gtk_box_append(GTK_BOX(ip->box), git);
}

GtkWidget *pt_info_panel_new(void) {
  return g_object_new(PT_TYPE_INFO_PANEL, NULL);
}
