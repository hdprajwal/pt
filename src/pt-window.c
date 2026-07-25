#include "pt-window.h"
#include "pt-terminal.h"
#include "pt-sidebar.h"
#include "pt-tab-strip.h"
#include "pt-statusline.h"
#include "pt-project-bar.h"
#include "pt-pane-grid.h"
#include "pt-palette.h"
#include "pt-session.h"
#include "pt-git-parse.h"
#include "pt-git-monitor.h"

typedef struct {
  char *title;
  GtkWidget *grid;      /* PtPaneGrid, owned ref */
  int unread;           /* output bursts seen while this tab was unwatched */
  gint64 unread_stamp;  /* monotonic time of the last counted burst */
} PtTabUI;

typedef struct {
  char *name;
  char *path;
  GPtrArray *tabs;      /* PtTabUI* */
  int active_tab;
  gboolean missing;
  PtGitStatus git;
  gboolean is_repo;
  int accent;           /* 0..5 index into the fixed accent cycle */
  PtGitMonitor *monitor;
  PtWindow *window;     /* back-pointer; Task 11 wires git monitors through it */
} PtProjectUI;

struct _PtWindow {
  AdwApplicationWindow parent_instance;
  GPtrArray *projects;  /* PtProjectUI* */
  int active_project;
  GtkWidget *sidebar, *tabstrip, *content, *statusline, *projectbar;
  GtkWidget *palette;   /* overlay child; hidden unless ⌃K is up */
  guint save_source;    /* debounce timer; used from Task 12 */
  guint status_source;  /* 500ms progress poll for the status bar */
  gboolean close_confirm_open;  /* a close-shell dialog is up; do not stack */
};

G_DEFINE_FINAL_TYPE(PtWindow, pt_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---------- helpers ---------- */
static PtProjectUI *active_project(PtWindow *w) {
  /* NULL after dispose: a dialog response or a queued grid signal can still
   * land on the window after its projects are gone. */
  if (w->projects == NULL || w->active_project < 0 ||
      (guint)w->active_project >= w->projects->len)
    return NULL;
  return g_ptr_array_index(w->projects, w->active_project);
}

static PtTabUI *active_tab(PtProjectUI *p) {
  if (p == NULL || p->active_tab < 0 ||
      (guint)p->active_tab >= p->tabs->len)
    return NULL;
  return g_ptr_array_index(p->tabs, p->active_tab);
}

static void mark_dirty(PtWindow *w);   /* persistence hook; body in Task 12 */

static void refresh_projectbar(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  pt_project_bar_update(PT_PROJECT_BAR(w->projectbar),
      p != NULL ? p->name : "pt",
      p != NULL ? p->path : "",
      (p != NULL && p->is_repo) ? p->git.branch : NULL,
      p != NULL ? p->git.changed : 0,
      p != NULL ? p->accent : 0);
}

static void refresh_sidebar(PtWindow *w) {
  int n = (int)w->projects->len;
  PtSidebarRow *rows = g_new0(PtSidebarRow, n);
  for (int i = 0; i < n; i++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, i);
    rows[i].name = p->name;
    rows[i].path = p->path;
    rows[i].missing = p->missing;
    rows[i].is_repo = p->is_repo;
    rows[i].changed = p->git.changed;
    g_strlcpy(rows[i].branch, p->git.branch, sizeof(rows[i].branch));
    rows[i].accent = p->accent;
    rows[i].shell_count = (int)p->tabs->len;
    int running = 0;
    for (guint j = 0; j < p->tabs->len; j++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, j);
      if (pt_pane_grid_any_running(PT_PANE_GRID(t->grid))) running++;
    }
    rows[i].running = running;
  }
  pt_sidebar_set_projects(PT_SIDEBAR(w->sidebar), rows, n, w->active_project);
  g_free(rows);
  PtProjectUI *ap = active_project(w);
  char *top = ap != NULL ? g_strdup_printf("pt :: %s", ap->name)
                         : g_strdup("pt");
  /* GTK4 does not dedupe this; each call is a Wayland round-trip, and this
   * runs on every foreground-command change. */
  if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), top) != 0)
    gtk_window_set_title(GTK_WINDOW(w), top);
  g_free(top);
  /* cheap enough to redo unconditionally, and never drifts out of sync */
  refresh_projectbar(w);
}

static void refresh_tabstrip(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  int n = p != NULL ? (int)p->tabs->len : 0;
  /* infos[].title borrows each tab's live string; the strip copies it before
   * this returns, and nothing here can free a title in between. */
  PtTabInfo *infos = g_new0(PtTabInfo, n);
  for (int i = 0; i < n; i++) {
    PtTabUI *t = g_ptr_array_index(p->tabs, i);
    PtPaneGrid *grid = PT_PANE_GRID(t->grid);
    PtTerminal *foc = pt_pane_grid_focused_terminal(grid);
    infos[i].title = t->title;
    infos[i].running = pt_pane_grid_any_running(grid);
    infos[i].last_exit = foc != NULL ? pt_terminal_last_exit(foc) : -1;
    infos[i].unread = t->unread;
    infos[i].accent = p->accent;
  }
  pt_tab_strip_set_tabs(PT_TAB_STRIP(w->tabstrip), infos, n,
                        p != NULL ? p->active_tab : -1);
  g_free(infos);
}

/* The pane the status bar speaks for: focused pane of the active tab of the
 * active project. NULL when there is no project or no tab. */
static PtTerminal *focused_terminal(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  return t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid))
                   : NULL;
}

static void refresh_statusline(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  PtTerminal *term = focused_terminal(w);
  gboolean running = term != NULL && pt_terminal_running(term);
  int last_exit = term != NULL ? pt_terminal_last_exit(term) : -1;
  PtProgress prog;
  gboolean has_prog = FALSE;
  const char *task = NULL;
  /* Only a running command can have progress, and scraping the grid is not
   * free — so this whole block is skipped while the pane sits at a prompt. */
  if (running) {
    task = pt_terminal_last_command(term);
    char *grid = pt_term_core_grid_text(pt_terminal_core(term));
    if (grid != NULL) {
      /* Progress counters live on the last non-empty row of the visible grid;
       * anything above it is scrollback of an older state. */
      char **lines = g_strsplit(grid, "\n", -1);
      for (int i = (int)g_strv_length(lines) - 1; i >= 0; i--) {
        g_strchomp(lines[i]);
        if (lines[i][0] != '\0') {
          has_prog = pt_progress_parse_line(lines[i], &prog);
          break;
        }
      }
      g_strfreev(lines);
      g_free(grid);
    }
  }
  pt_statusline_update(PT_STATUSLINE(w->statusline), running, last_exit,
                       has_prog ? &prog : NULL, task,
                       p != NULL ? p->accent : 0);
}

/* Keeps the bar moving mid-task: a long build emits no signal per line, so the
 * only way to see 128/214 tick over is to look. Deliberately does nothing when
 * the focused pane is idle — the falling edge is covered by "command-changed",
 * which fires when the foreground program goes back to the shell. */
static gboolean tick_statusline(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  PtTerminal *term = focused_terminal(w);
  if (term != NULL && pt_terminal_running(term))
    refresh_statusline(w);
  return G_SOURCE_CONTINUE;
}

static void show_active_grid(PtWindow *w) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(w->content)) != NULL)
    gtk_box_remove(GTK_BOX(w->content), child);
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) {
    t->unread = 0;
    gtk_box_append(GTK_BOX(w->content), t->grid);
    pt_pane_grid_focus_terminal(PT_PANE_GRID(t->grid));
  } else {
    PtProjectUI *ap = active_project(w);
    const char *msg =
        (ap != NULL && ap->missing)
            ? "project directory missing — × to remove"
            : "no project";
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_vexpand(empty, TRUE);
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    GtkWidget *hint = gtk_label_new(msg);
    gtk_widget_add_css_class(hint, "pt-empty");
    gtk_box_append(GTK_BOX(empty), hint);
    GtkWidget *keys = gtk_label_new(
        (ap != NULL && ap->missing)
            ? "^⇧W remove · ^1…9 switch project"
            : "[+ project] to add · ^1…9 switch · ^⇧T new tab");
    gtk_widget_add_css_class(keys, "pt-empty-hint");
    gtk_box_append(GTK_BOX(empty), keys);
    gtk_box_append(GTK_BOX(w->content), empty);
  }
  refresh_tabstrip(w);
  refresh_statusline(w);
}

/* ---------- tab/grid plumbing ---------- */
/* Every grid handler below opens with this. A grid outlives the window in the
 * shutdown window: pt-pane-grid's close idle holds its own ref, so a shell that
 * exits in the same main-loop frame as the window close emits into a window
 * whose dispose already dropped w->projects. NULL there means "gone" — bail
 * before touching the array (or re-arming a save through mark_dirty). */
static void on_grid_structure(PtPaneGrid *g, gpointer user) {
  (void)g;
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  refresh_statusline(w);
  mark_dirty(w);
}

static void on_grid_focus(PtPaneGrid *g, gpointer user) {
  (void)g;
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  refresh_statusline(w);
}

/* Output on an unwatched tab bumps its unread counter. Background projects
 * count too, so scan every project; only the tab the user is actually looking
 * at (active project AND active tab) is exempt. Bursts are coalesced to at
 * most one bump per 300ms so a chatty command reads as a few units of news
 * rather than hundreds. */
static void on_grid_activity(PtPaneGrid *g, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  for (guint pi = 0; pi < w->projects->len; pi++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, pi);
    gboolean p_active = ((int)pi == w->active_project);
    for (guint ti = 0; ti < p->tabs->len; ti++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, ti);
      if (t->grid != GTK_WIDGET(g)) continue;
      if (p_active && (int)ti == p->active_tab) return;   /* being watched */
      gint64 now = g_get_monotonic_time();
      if (now - t->unread_stamp > 300 * G_TIME_SPAN_MILLISECOND) {
        t->unread++;
        t->unread_stamp = now;
        if (p_active) refresh_tabstrip(w);
      }
      return;
    }
  }
}

/* Last pane in a grid closed via a clean shell exit → drop the owning tab.
 * The grid may belong to a background project/tab (a background shell can exit),
 * so scan every project. The emitting grid survives this (the idle in
 * pt-pane-grid holds its own ref) even though tab removal unrefs it here. */
static void on_grid_emptied(PtPaneGrid *g, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  for (guint pi = 0; pi < w->projects->len; pi++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, pi);
    for (guint ti = 0; ti < p->tabs->len; ti++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, ti);
      if (t->grid != GTK_WIDGET(g)) continue;
      g_ptr_array_remove_index(p->tabs, ti);
      if (p->active_tab >= (int)p->tabs->len)
        p->active_tab = (int)p->tabs->len - 1;
      if ((int)pi == w->active_project)
        show_active_grid(w);
      mark_dirty(w);
      return;
    }
  }
}

/* Focused pane's foreground program changed → relabel the owning tab live.
 * A comm change is exactly when run-state flips, so refresh the strip (dots)
 * and the sidebar (run counters) regardless of whether the title moved; both
 * are throttled by the 700ms comm poll upstream.
 * Deliberately does NOT mark_dirty: command churn must not spam saves; the tab
 * title is captured on the next structural save anyway. */
static void on_grid_command(PtPaneGrid *g, const char *comm, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  for (guint pi = 0; pi < w->projects->len; pi++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, pi);
    for (guint ti = 0; ti < p->tabs->len; ti++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, ti);
      if (t->grid != GTK_WIDGET(g)) continue;
      g_free(t->title);
      t->title = g_strdup(comm);
      if ((int)pi == w->active_project) {
        refresh_tabstrip(w);
        /* run-state just flipped for a pane of the visible project: this is the
         * edge the 500ms poll deliberately does not cover (it only runs while
         * something is running), so the "✓ / ✗ exit N" settle happens here. */
        refresh_statusline(w);
      }
      refresh_sidebar(w);
      return;
    }
  }
}

/* The prompt smuggles the last exit code out through the terminal title, so a
 * title change is the earliest moment the "✓ / ✗ exit N" marker can settle —
 * a failing builtin (`false`) never moves the foreground comm, so waiting for
 * "command-changed" would leave the bar stale until the next poll. Both
 * renderers dedupe internally, so refreshing on every title is cheap. */
static void on_grid_title(PtPaneGrid *g, const char *title, gpointer user) {
  (void)title;
  PtWindow *w = PT_WINDOW(user);
  if (w->projects == NULL) return;
  PtTabUI *t = active_tab(active_project(w));
  if (t == NULL || t->grid != GTK_WIDGET(g)) return;
  refresh_statusline(w);
  refresh_tabstrip(w);
}

/* PT_BRANCH has to be right for a project's *first* shells, but the git
 * monitor's first poll is async and lands long after they spawn — without this
 * every restored shell (i.e. every shell at app start) got PT_BRANCH="".
 * Reading .git/HEAD is a single small file read, no subprocess; the monitor
 * overwrites p->git with the authoritative status shortly after.
 * Leaves the branch untouched when there is no .git/HEAD (non-repo, or a
 * worktree/submodule whose .git is a gitdir: pointer — the monitor covers it). */
static void seed_git_branch(PtProjectUI *p) {
  static const char ref_prefix[] = "ref: refs/heads/";
  char *head = g_build_filename(p->path, ".git", "HEAD", NULL);
  char *txt = NULL;
  if (g_file_get_contents(head, &txt, NULL, NULL)) {
    g_strstrip(txt);
    if (g_str_has_prefix(txt, ref_prefix))
      g_strlcpy(p->git.branch, txt + sizeof(ref_prefix) - 1,
                sizeof p->git.branch);
    else if (txt[0] != '\0')
      g_strlcpy(p->git.branch, "(detached)", sizeof p->git.branch);
  }
  g_free(txt);
  g_free(head);
}

/* Terminals are spawned deep inside the pane grid (leaves become terminals
 * during rebuild), so the project context reaches the child through the
 * module-level default env instead of a parameter. Every path that can create
 * a terminal for project p calls this immediately before doing so; the grid
 * builds synchronously, so the default is never read for a different project
 * than the one that just set it. */
static void set_spawn_env_for(PtProjectUI *p) {
  /* Mirrors the accent hexes in style.css (.pt-a0 .. .pt-a5). */
  static const char *accents[PT_ACCENT_COUNT] =
      { "#6ee7a0", "#8ab4f8", "#f2b25c", "#c99bf0", "#5ed3c4", "#e0849b" };
  char *proj = g_strdup_printf("PT_PROJECT=%s", p->name);
  char *acc  = g_strdup_printf("PT_ACCENT=%s",
                               accents[((p->accent % PT_ACCENT_COUNT) +
                                        PT_ACCENT_COUNT) % PT_ACCENT_COUNT]);
  char *br   = g_strdup_printf("PT_BRANCH=%s", p->git.branch);
  const char *pairs[] = { proj, acc, br, NULL };
  pt_terminal_set_default_env(pairs);
  g_free(proj); g_free(acc); g_free(br);
}

static PtTabUI *tab_ui_new(PtWindow *w, const char *title, PtSplitNode *tree) {
  PtTabUI *t = g_new0(PtTabUI, 1);
  t->title = g_strdup(title);
  t->grid = pt_pane_grid_new(tree);
  g_object_ref_sink(t->grid);
  g_signal_connect(t->grid, "structure-changed",
                   G_CALLBACK(on_grid_structure), w);
  g_signal_connect(t->grid, "focus-changed", G_CALLBACK(on_grid_focus), w);
  g_signal_connect(t->grid, "command-changed", G_CALLBACK(on_grid_command), w);
  g_signal_connect(t->grid, "title-changed", G_CALLBACK(on_grid_title), w);
  g_signal_connect(t->grid, "activity", G_CALLBACK(on_grid_activity), w);
  g_signal_connect(t->grid, "emptied", G_CALLBACK(on_grid_emptied), w);
  return t;
}

static void tab_ui_free(gpointer data) {
  PtTabUI *t = data;
  g_free(t->title);
  if (t->grid != NULL) {
    if (gtk_widget_get_parent(t->grid) != NULL)
      gtk_widget_unparent(t->grid);
    g_object_unref(t->grid);
  }
  g_free(t);
}

static void on_git_update(const PtGitStatus *st, gboolean is_repo,
                          gpointer user) {
  /* user is the PtProjectUI; find its window via stored back-pointer */
  PtProjectUI *p = user;
  p->git = *st;
  p->is_repo = is_repo;
  /* No refresh_statusline here: the status bar stopped speaking for git in the
   * rebuild (that moved to the project bar), and scraping the terminal grid on
   * every git poll would be pure waste. */
  refresh_sidebar(p->window);
}

static PtProjectUI *project_ui_new(PtWindow *w, const char *name,
                                   const char *path) {
  PtProjectUI *p = g_new0(PtProjectUI, 1);
  p->name = g_strdup(name);
  p->path = g_strdup(path);
  p->window = w;
  /* called before the project joins the array, so len is its future index */
  p->accent = (int)(w->projects->len % PT_ACCENT_COUNT);
  p->tabs = g_ptr_array_new_with_free_func(tab_ui_free);
  p->missing = !g_file_test(path, G_FILE_TEST_IS_DIR);
  if (!p->missing) {
    seed_git_branch(p);   /* the monitor has not polled yet; read HEAD directly */
    set_spawn_env_for(p);
    g_ptr_array_add(p->tabs, tab_ui_new(w, "shell", pt_split_leaf_new(path)));
    p->monitor = pt_git_monitor_new(path, on_git_update, p);
  }
  return p;
}

static void project_ui_free(gpointer data) {
  PtProjectUI *p = data;
  pt_git_monitor_free(p->monitor);
  g_free(p->name);
  g_free(p->path);
  g_ptr_array_free(p->tabs, TRUE);
  g_free(p);
}

/* ---------- actions ---------- */
static void action_switch_project(PtWindow *w, int idx) {
  if (idx < 0 || (guint)idx >= w->projects->len) return;
  w->active_project = idx;
  refresh_sidebar(w);
  show_active_grid(w);
  mark_dirty(w);
}

static void action_switch_tab(PtWindow *w, int idx) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || idx < 0 || (guint)idx >= p->tabs->len) return;
  p->active_tab = idx;
  show_active_grid(w);
  mark_dirty(w);
}

static void action_next_tab(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || p->tabs->len <= 1) return;
  int len = (int)p->tabs->len;
  p->active_tab = (p->active_tab + 1 + len) % len;
  show_active_grid(w);
  mark_dirty(w);
}

static void action_prev_tab(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || p->tabs->len <= 1) return;
  int len = (int)p->tabs->len;
  p->active_tab = (p->active_tab - 1 + len) % len;
  show_active_grid(w);
  mark_dirty(w);
}

static void action_new_tab(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || p->missing) return;
  set_spawn_env_for(p);
  g_ptr_array_add(p->tabs, tab_ui_new(w, "shell", pt_split_leaf_new(p->path)));
  p->active_tab = (int)p->tabs->len - 1;
  show_active_grid(w);
  mark_dirty(w);
}

/* Locate the tab that owns a grid. Confirmation is async, so by response time
 * the grid may have been dropped already (its last shell exited cleanly →
 * on_grid_emptied removed the tab) — a plain "close the active tab's pane"
 * would then hit whatever tab slid into that slot. */
static gboolean find_grid(PtWindow *w, PtPaneGrid *g, guint *out_pi,
                          guint *out_ti) {
  if (w->projects == NULL || g == NULL) return FALSE;
  for (guint pi = 0; pi < w->projects->len; pi++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, pi);
    for (guint ti = 0; ti < p->tabs->len; ti++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, ti);
      if (t->grid != GTK_WIDGET(g)) continue;
      if (out_pi != NULL) *out_pi = pi;
      if (out_ti != NULL) *out_ti = ti;
      return TRUE;
    }
  }
  return FALSE;
}

/* Close the focused pane of grid g (not of whatever happens to be active now).
 * No-op when g is no longer owned by any tab. */
static void do_close_pane(PtWindow *w, PtPaneGrid *g) {
  guint pi = 0, ti = 0;
  if (!find_grid(w, g, &pi, &ti)) return;
  PtProjectUI *p = g_ptr_array_index(w->projects, pi);
  if (!pt_pane_grid_close_focused(g)) {
    /* last pane closed → close the owning tab, whichever one it is */
    g_ptr_array_remove_index(p->tabs, ti);
    if (p->active_tab > (int)ti) p->active_tab--;
    if (p->active_tab >= (int)p->tabs->len)
      p->active_tab = (int)p->tabs->len - 1;
    if ((int)pi == w->active_project) show_active_grid(w);
  }
  mark_dirty(w);
}

/* Response-callback payload: keeps the target grid alive (the tab holding it
 * can be dropped while the dialog is up) and the window resolvable. Freed
 * through the closure's GDestroyNotify, so it survives a dialog that is torn
 * down without ever emitting a response. */
typedef struct {
  PtWindow *window;   /* owned ref */
  PtPaneGrid *grid;   /* owned ref */
} PtCloseCtx;

static void close_ctx_free(gpointer data, GClosure *closure) {
  (void)closure;
  PtCloseCtx *c = data;
  c->window->close_confirm_open = FALSE;
  g_object_unref(c->grid);
  g_object_unref(c->window);
  g_free(c);
}

static void on_close_pane_response(AdwAlertDialog *dlg, const char *response,
                                   gpointer user) {
  (void)dlg;
  PtCloseCtx *c = user;
  if (g_strcmp0(response, "close") == 0) {
    do_close_pane(c->window, c->grid);
    return;
  }
  /* Cancelled (or dismissed): hand the keyboard back to the pane we asked
   * about, if that grid is still on screen. */
  if (find_grid(c->window, c->grid, NULL, NULL))
    pt_pane_grid_focus_terminal(c->grid);
}

/* Closing a pane that is mid-command kills the command. Only ask when there is
 * actually something to lose — at a bare prompt this closes silently. */
static void action_close_pane(PtWindow *w) {
  if (w->close_confirm_open) return;   /* repeated ⌃⇧W must not stack dialogs */
  PtTabUI *t = active_tab(active_project(w));
  if (t == NULL) return;
  PtPaneGrid *grid = PT_PANE_GRID(t->grid);
  PtTerminal *term = pt_pane_grid_focused_terminal(grid);
  if (term == NULL || !pt_terminal_running(term)) {
    do_close_pane(w, grid);
    return;
  }
  AdwDialog *dlg = adw_alert_dialog_new(
      "Close shell?", "A process is still running in this shell.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                 "cancel", "Cancel", "close", "Close", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "close",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
  PtCloseCtx *c = g_new0(PtCloseCtx, 1);
  c->window = g_object_ref(w);
  c->grid = g_object_ref(grid);
  w->close_confirm_open = TRUE;
  g_signal_connect_data(dlg, "response", G_CALLBACK(on_close_pane_response), c,
                        close_ctx_free, 0);
  adw_dialog_present(dlg, GTK_WIDGET(w));
}

static void action_split(PtWindow *w, PtSplitKind kind) {
  PtProjectUI *p = active_project(w);
  PtTabUI *t = active_tab(p);
  if (t == NULL) return;
  set_spawn_env_for(p);
  pt_pane_grid_split(PT_PANE_GRID(t->grid), kind);
}

static void action_focus_next(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_focus_next(PT_PANE_GRID(t->grid));
}

static void action_focus_direction(PtWindow *w, PtPaneDirection dir) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_focus_direction(PT_PANE_GRID(t->grid), dir);
}

static void action_focus_prev(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_focus_prev(PT_PANE_GRID(t->grid));
}

static void action_paste(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  PtTerminal *term =
      t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid)) : NULL;
  if (term != NULL) pt_terminal_paste(term);
}

static void action_copy(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  PtTerminal *term =
      t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid)) : NULL;
  if (term != NULL) pt_terminal_copy(term);
}

/* ---------- project add/remove ---------- */
static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
                                                     res, NULL);
  if (file == NULL) return;
  char *path = g_file_get_path(file);
  char *name = g_path_get_basename(path);
  g_ptr_array_add(w->projects, project_ui_new(w, name, path));
  w->active_project = (int)w->projects->len - 1;
  g_free(name);
  g_free(path);
  g_object_unref(file);
  refresh_sidebar(w);
  show_active_grid(w);
  mark_dirty(w);
}

/* Shared by the sidebar's [+ project] button and ⌃N. */
static void action_add_project(PtWindow *w) {
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(w), NULL,
                                on_folder_chosen, w);
  g_object_unref(dlg);
}

static void on_project_add(PtSidebar *sb, gpointer user) {
  (void)sb;
  action_add_project(PT_WINDOW(user));
}

static void on_project_remove(PtSidebar *sb, int idx, gpointer user) {
  (void)sb;
  PtWindow *w = PT_WINDOW(user);
  if (idx < 0 || (guint)idx >= w->projects->len) return;
  g_ptr_array_remove_index(w->projects, idx);
  if (w->active_project >= (int)w->projects->len)
    w->active_project = (int)w->projects->len - 1;
  refresh_sidebar(w);
  show_active_grid(w);
  mark_dirty(w);
}

static void on_project_selected(PtSidebar *sb, int idx, gpointer user) {
  (void)sb;
  action_switch_project(PT_WINDOW(user), idx);
}

/* Hands the keyboard back to whatever pane is on screen. Anything that steals
 * focus for a transient UI (sidebar search, command palette) ends here. */
static void focus_active_terminal(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_focus_terminal(PT_PANE_GRID(t->grid));
}

/* Escape in the sidebar search hands the keyboard back to the terminal. */
static void on_search_escape(PtSidebar *sb, gpointer user) {
  (void)sb;
  focus_active_terminal(PT_WINDOW(user));
}

static void on_tab_selected(PtTabStrip *s, int idx, gpointer user) {
  (void)s;
  action_switch_tab(PT_WINDOW(user), idx);
}

static void on_tab_new(PtTabStrip *s, gpointer user) {
  (void)s;
  action_new_tab(PT_WINDOW(user));
}

/* ---------- command palette ---------- */
/* Every project, each followed by its shells. The palette ranks this flat list
 * and hands back the (project, tab) pair the user picked. */
static void action_open_palette(PtWindow *w) {
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(PtPaletteItem));
  for (guint i = 0; i < w->projects->len; i++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, i);
    /* Same spelling as the project bar: home-abbreviated path, plain branch
     * text. The ⑂ glyph is reserved for the terminal's own identity line. */
    char *shown_path = pt_path_home_abbrev(p->path);
    PtPaletteItem it = {
      .name = g_strdup(p->name),
      .detail = p->is_repo
          ? g_strdup_printf("%s · %s", shown_path, p->git.branch)
          : g_strdup(shown_path),
      .shortcut = i < 9 ? g_strdup_printf("^%u", i + 1) : NULL,
      .accent = p->accent, .is_shell = FALSE,
      .project_idx = (int)i, .tab_idx = -1,
    };
    g_free(shown_path);
    g_array_append_val(arr, it);
    for (guint j = 0; j < p->tabs->len; j++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, j);
      PtPaletteItem sh = {
        .name = g_strdup(t->title),
        .detail = g_strdup(p->name),
        .shortcut = NULL, .accent = p->accent, .is_shell = TRUE,
        .project_idx = (int)i, .tab_idx = (int)j,
      };
      g_array_append_val(arr, sh);
    }
  }
  int n = (int)arr->len;
  pt_palette_open(PT_PALETTE(w->palette),
                  (PtPaletteItem *)g_array_free(arr, FALSE), n);
}

static void on_palette_activated(PtPalette *pal, int project_idx, int tab_idx,
                                 gpointer user) {
  (void)pal;
  PtWindow *w = PT_WINDOW(user);
  action_switch_project(w, project_idx);
  if (tab_idx >= 0) action_switch_tab(w, tab_idx);
}

static void on_palette_closed(PtPalette *pal, gpointer user) {
  (void)pal;
  focus_active_terminal(PT_WINDOW(user));
}

/* ---------- shortcuts ---------- */

typedef struct { PtWindow *w; int arg; } ShortcutCtx;

/* The shortcut controller below runs in the CAPTURE phase on the window, so it
 * sees every accelerator before the palette's own key controller does. Letting
 * one through while the palette is up would strand it: almost all of these
 * actions end in pt_pane_grid_focus_terminal, focus would land on a terminal in
 * a sibling subtree, the palette would drop out of the key propagation path
 * entirely, and the overlay would sit there visible with Escape dead. So while
 * the palette is open every accelerator reports "not handled" and falls through
 * to it — ⌃K alone still acts, as a toggle. */
static gboolean palette_blocks(PtWindow *w) {
  return w->palette != NULL && pt_palette_is_open(PT_PALETTE(w->palette));
}

static gboolean sc_project(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget; (void)args;
  ShortcutCtx *c = user;
  if (palette_blocks(c->w)) return FALSE;
  action_switch_project(c->w, c->arg);
  return TRUE;
}
static gboolean sc_tab(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget; (void)args;
  ShortcutCtx *c = user;
  if (palette_blocks(c->w)) return FALSE;
  action_switch_tab(c->w, c->arg);
  return TRUE;
}
static gboolean sc_new_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_new_tab(PT_WINDOW(u)); return TRUE;
}
/* The one accelerator that stays live: ⌃K opens the palette and ⌃K closes it. */
static gboolean sc_palette(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  PtWindow *w = PT_WINDOW(u);
  if (palette_blocks(w)) pt_palette_close(PT_PALETTE(w->palette));
  else action_open_palette(w);
  return TRUE;
}
static gboolean sc_add_project(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_add_project(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_next_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_next_tab(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_prev_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_prev_tab(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_split_h(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_split(PT_WINDOW(u), PT_SPLIT_H); return TRUE;
}
static gboolean sc_split_v(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_split(PT_WINDOW(u), PT_SPLIT_V); return TRUE;
}
static gboolean sc_close_pane(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_close_pane(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_focus_next(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_focus_next(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_paste(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_paste(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_copy(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_copy(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_zoom_in(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  pt_terminal_set_font_size(pt_terminal_font_size() + 1);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_zoom_out(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  pt_terminal_set_font_size(pt_terminal_font_size() - 1);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_zoom_reset(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  pt_terminal_set_font_size(PT_FONT_SIZE_DEFAULT);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_focus_dir(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  ShortcutCtx *c = u;
  if (palette_blocks(c->w)) return FALSE;
  action_focus_direction(c->w, (PtPaneDirection)c->arg);
  return TRUE;
}
static gboolean sc_focus_prev(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  if (palette_blocks(PT_WINDOW(u))) return FALSE;
  action_focus_prev(PT_WINDOW(u)); return TRUE;
}

static void add_shortcut(GtkShortcutController *ctl, const char *accel,
                         GtkShortcutFunc fn, gpointer data,
                         GDestroyNotify destroy) {
  GtkShortcutTrigger *trig = gtk_shortcut_trigger_parse_string(accel);
  g_warn_if_fail(trig != NULL);   /* never ship a silent, unparseable binding */
  gtk_shortcut_controller_add_shortcut(ctl,
      gtk_shortcut_new(trig, gtk_callback_action_new(fn, data, destroy)));
}

/* Bind a raw keyval+mods (used for ISO_Left_Tab, which some setups deliver for
 * Shift+Tab and which does not always round-trip through parse_string). */
static void add_keyval_shortcut(GtkShortcutController *ctl, guint keyval,
                                GdkModifierType mods, GtkShortcutFunc fn,
                                gpointer data, GDestroyNotify destroy) {
  gtk_shortcut_controller_add_shortcut(ctl,
      gtk_shortcut_new(gtk_keyval_trigger_new(keyval, mods),
                       gtk_callback_action_new(fn, data, destroy)));
}

static void install_shortcuts(PtWindow *w) {
  GtkShortcutController *ctl =
      GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ctl),
                                             GTK_PHASE_CAPTURE);
  for (int i = 0; i < 9; i++) {
    ShortcutCtx *pc = g_new(ShortcutCtx, 1);
    pc->w = w; pc->arg = i;
    char accel[32];
    g_snprintf(accel, sizeof(accel), "<Control>%d", i + 1);
    add_shortcut(ctl, accel, sc_project, pc, g_free);
    ShortcutCtx *tc = g_new(ShortcutCtx, 1);
    tc->w = w; tc->arg = i;
    g_snprintf(accel, sizeof(accel), "<Alt>%d", i + 1);
    add_shortcut(ctl, accel, sc_tab, tc, g_free);
  }
  add_shortcut(ctl, "<Control>k", sc_palette, w, NULL);
  add_shortcut(ctl, "<Control>n", sc_add_project, w, NULL);
  add_shortcut(ctl, "<Control>t", sc_new_tab, w, NULL);
  add_shortcut(ctl, "<Control><Shift>t", sc_new_tab, w, NULL);
  add_shortcut(ctl, "<Control>Tab", sc_next_tab, w, NULL);
  add_shortcut(ctl, "<Control><Shift>Tab", sc_prev_tab, w, NULL);
  add_keyval_shortcut(ctl, GDK_KEY_ISO_Left_Tab, GDK_CONTROL_MASK,
                      sc_prev_tab, w, NULL);
  /* ⌥⇥ / ⌥⇧⇥ cycle tabs when the compositor does not claim them first; ⌃⇥
   * stays the reliable fallback. No grab is attempted. */
  add_shortcut(ctl, "<Alt>Tab", sc_next_tab, w, NULL);
  add_shortcut(ctl, "<Alt><Shift>Tab", sc_prev_tab, w, NULL);
  add_keyval_shortcut(ctl, GDK_KEY_ISO_Left_Tab, GDK_ALT_MASK,
                      sc_prev_tab, w, NULL);
  add_shortcut(ctl, "<Control><Shift>d", sc_split_h, w, NULL);
  add_shortcut(ctl, "<Control><Shift>s", sc_split_v, w, NULL);
  add_shortcut(ctl, "<Control><Shift>w", sc_close_pane, w, NULL);
  add_shortcut(ctl, "<Control><Shift>o", sc_focus_next, w, NULL);
  /* Ghostty parity: Ctrl+Super+] / [ cycle next / previous pane. */
  add_shortcut(ctl, "<Control><Super>bracketright", sc_focus_next, w, NULL);
  add_shortcut(ctl, "<Control><Super>bracketleft", sc_focus_prev, w, NULL);
  {
    static const struct { const char *accel; PtPaneDirection dir; } dirs[] = {
      { "<Control><Alt>Left",  PT_PANE_DIR_LEFT  },
      { "<Control><Alt>Right", PT_PANE_DIR_RIGHT },
      { "<Control><Alt>Up",    PT_PANE_DIR_UP    },
      { "<Control><Alt>Down",  PT_PANE_DIR_DOWN  },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(dirs); i++) {
      ShortcutCtx *dc = g_new(ShortcutCtx, 1);
      dc->w = w; dc->arg = (int)dirs[i].dir;
      add_shortcut(ctl, dirs[i].accel, sc_focus_dir, dc, g_free);
    }
  }
  add_shortcut(ctl, "<Control><Shift>v", sc_paste, w, NULL);
  add_shortcut(ctl, "<Control><Shift>c", sc_copy, w, NULL);
  /* Font zoom: cover =, shifted + (both plain and explicit-shift forms),
   * and the keypad. */
  add_shortcut(ctl, "<Control>equal", sc_zoom_in, w, NULL);
  add_shortcut(ctl, "<Control>plus", sc_zoom_in, w, NULL);
  add_shortcut(ctl, "<Control><Shift>plus", sc_zoom_in, w, NULL);
  add_shortcut(ctl, "<Control>KP_Add", sc_zoom_in, w, NULL);
  add_shortcut(ctl, "<Control>minus", sc_zoom_out, w, NULL);
  add_shortcut(ctl, "<Control>underscore", sc_zoom_out, w, NULL);
  add_shortcut(ctl, "<Control>KP_Subtract", sc_zoom_out, w, NULL);
  add_shortcut(ctl, "<Control>0", sc_zoom_reset, w, NULL);
  gtk_widget_add_controller(GTK_WIDGET(w), GTK_EVENT_CONTROLLER(ctl));
}

/* ---------- persistence ---------- */
static PtSessionState *capture_state(PtWindow *w) {
  PtSessionState *s = pt_session_state_new();
  s->active_project = w->active_project;
  s->font_size = pt_terminal_font_size();
  for (guint i = 0; i < w->projects->len; i++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, i);
    PtProjectState *ps = pt_project_state_new(p->name, p->path);
    ps->active_tab = p->active_tab;
    ps->accent = p->accent;
    for (guint j = 0; j < p->tabs->len; j++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, j);
      PtPaneGrid *grid = PT_PANE_GRID(t->grid);
      pt_pane_grid_sync_cwds(grid);
      /* Deep-copy the live tree via JSON round-trip (cheap, reuses code). */
      JsonNode *j_tree = pt_split_to_json(pt_pane_grid_tree(grid));
      PtSplitNode *copy = pt_split_from_json(j_tree);
      json_node_unref(j_tree);
      g_ptr_array_add(ps->tabs, pt_tab_state_new(t->title, copy));
    }
    g_ptr_array_add(s->projects, ps);
  }
  return s;
}

static gboolean do_save(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  w->save_source = 0;
  PtSessionState *s = capture_state(w);
  char *path = pt_session_default_path();
  GError *err = NULL;
  if (!pt_session_save(s, path, &err)) {
    g_warning("pt: state save failed: %s", err->message);
    g_clear_error(&err);
  }
  g_free(path);
  pt_session_state_free(s);
  return G_SOURCE_REMOVE;
}

static void mark_dirty(PtWindow *w) {
  if (w->save_source != 0) g_source_remove(w->save_source);
  w->save_source = g_timeout_add_seconds(1, do_save, w);
}

static void restore_state(PtWindow *w) {
  char *path = pt_session_default_path();
  PtSessionState *s = pt_session_load(path);
  g_free(path);
  if (s == NULL) return;
  pt_terminal_set_font_size(s->font_size);   /* before any terminal exists */
  for (guint i = 0; i < s->projects->len; i++) {
    PtProjectState *ps = g_ptr_array_index(s->projects, i);
    PtProjectUI *p = g_new0(PtProjectUI, 1);
    p->window = w;
    p->name = g_strdup(ps->name);
    p->path = g_strdup(ps->path);
    p->accent = ps->accent;
    p->tabs = g_ptr_array_new_with_free_func(tab_ui_free);
    p->missing = !g_file_test(ps->path, G_FILE_TEST_IS_DIR);
    if (!p->missing) {
      /* per project, not once for the whole restore: each project's shells must
       * see their own PT_PROJECT/PT_ACCENT/PT_BRANCH */
      seed_git_branch(p);
      set_spawn_env_for(p);
      for (guint j = 0; j < ps->tabs->len; j++) {
        PtTabState *ts = g_ptr_array_index(ps->tabs, j);
        /* steal the tree from the session copy */
        g_ptr_array_add(p->tabs, tab_ui_new(w, ts->title, ts->tree));
        ts->tree = NULL;
      }
      if (p->tabs->len == 0)
        g_ptr_array_add(p->tabs,
                        tab_ui_new(w, "shell", pt_split_leaf_new(p->path)));
      p->active_tab = CLAMP(ps->active_tab, 0, (int)p->tabs->len - 1);
      p->monitor = pt_git_monitor_new(p->path, on_git_update, p);
    }
    g_ptr_array_add(w->projects, p);
  }
  if (w->projects->len > 0)
    w->active_project = CLAMP(s->active_project, 0,
                              (int)w->projects->len - 1);
  pt_session_state_free(s);
}

static gboolean on_close_request(GtkWindow *win, gpointer user) {
  (void)user;
  PtWindow *w = PT_WINDOW(win);
  if (w->save_source != 0) { g_source_remove(w->save_source); w->save_source = 0; }
  do_save(w);          /* synchronous final save */
  w->save_source = 0;
  return FALSE;        /* proceed with close */
}

/* ---------- construction ---------- */
static void pt_window_dispose(GObject *obj) {
  PtWindow *w = PT_WINDOW(obj);
  if (w->save_source != 0) { g_source_remove(w->save_source); w->save_source = 0; }
  if (w->status_source != 0) {
    g_source_remove(w->status_source);
    w->status_source = 0;
  }
  g_clear_pointer(&w->projects, g_ptr_array_unref);
  G_OBJECT_CLASS(pt_window_parent_class)->dispose(obj);
}

static void pt_window_class_init(PtWindowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_window_dispose;
}

static void pt_window_init(PtWindow *w) {
  w->projects = g_ptr_array_new_with_free_func(project_ui_free);
  w->active_project = -1;

  /* The sidebar runs the full window height, so the project bar (which owns the
   * window controls and the drag handle) sits atop the content column only. */
  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand(body, TRUE);
  w->sidebar = pt_sidebar_new();
  gtk_box_append(GTK_BOX(body), w->sidebar);

  GtkWidget *main_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(main_col, TRUE);
  w->projectbar = pt_project_bar_new();
  gtk_box_append(GTK_BOX(main_col), w->projectbar);
  w->tabstrip = pt_tab_strip_new();
  gtk_box_append(GTK_BOX(main_col), w->tabstrip);
  w->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(w->content, TRUE);
  gtk_box_append(GTK_BOX(main_col), w->content);
  w->statusline = pt_statusline_new();
  gtk_box_append(GTK_BOX(main_col), w->statusline);
  gtk_box_append(GTK_BOX(body), main_col);

  /* The palette floats over everything, sidebar included, so it wraps the whole
   * body rather than the content column. GtkOverlay leaves overlay children out
   * of its size request, so a hidden palette costs the layout nothing. */
  GtkWidget *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), body);
  w->palette = pt_palette_new();
  gtk_widget_set_visible(w->palette, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->palette);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(w), overlay);

  g_signal_connect(w->palette, "activated",
                   G_CALLBACK(on_palette_activated), w);
  g_signal_connect(w->palette, "closed", G_CALLBACK(on_palette_closed), w);
  g_signal_connect(w->sidebar, "project-selected",
                   G_CALLBACK(on_project_selected), w);
  g_signal_connect(w->sidebar, "project-add", G_CALLBACK(on_project_add), w);
  g_signal_connect(w->sidebar, "project-remove",
                   G_CALLBACK(on_project_remove), w);
  g_signal_connect(w->sidebar, "search-escape",
                   G_CALLBACK(on_search_escape), w);
  g_signal_connect(w->tabstrip, "tab-selected",
                   G_CALLBACK(on_tab_selected), w);
  g_signal_connect(w->tabstrip, "tab-new", G_CALLBACK(on_tab_new), w);

  install_shortcuts(w);
  g_signal_connect(w, "close-request", G_CALLBACK(on_close_request), NULL);
  restore_state(w);
  refresh_sidebar(w);
  show_active_grid(w);

  /* Plain pointer, no ref: the source must not keep the window alive. Removed
   * in dispose, which is the only place the window can go away from. */
  w->status_source = g_timeout_add(500, tick_statusline, w);
}

GtkWidget *pt_window_new(AdwApplication *app) {
  return g_object_new(PT_TYPE_WINDOW, "application", app,
                      "title", "pt",
                      "default-width", 1100, "default-height", 700, NULL);
}
