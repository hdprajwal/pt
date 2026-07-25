#include "pt-window.h"
#include "pt-terminal.h"
#include "pt-sidebar.h"
#include "pt-tab-strip.h"
#include "pt-statusline.h"
#include "pt-pane-grid.h"
#include "pt-session.h"
#include "pt-git-parse.h"
#include "pt-git-monitor.h"

typedef struct {
  char *title;
  GtkWidget *grid;      /* PtPaneGrid, owned ref */
  gboolean activity;
} PtTabUI;

typedef struct {
  char *name;
  char *path;
  GPtrArray *tabs;      /* PtTabUI* */
  int active_tab;
  gboolean missing;
  PtGitStatus git;
  gboolean is_repo;
  PtGitMonitor *monitor;
  PtWindow *window;     /* back-pointer; Task 11 wires git monitors through it */
} PtProjectUI;

struct _PtWindow {
  AdwApplicationWindow parent_instance;
  GPtrArray *projects;  /* PtProjectUI* */
  int active_project;
  GtkWidget *sidebar, *tabstrip, *content, *statusline, *topbar_label;
  guint save_source;    /* debounce timer; used from Task 12 */
};

G_DEFINE_FINAL_TYPE(PtWindow, pt_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---------- helpers ---------- */
static PtProjectUI *active_project(PtWindow *w) {
  if (w->active_project < 0 ||
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
    /* accent field lands with the project bar task */
    rows[i].accent = i % PT_ACCENT_COUNT;
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
  gtk_label_set_text(GTK_LABEL(w->topbar_label), top);
  gtk_window_set_title(GTK_WINDOW(w), top);
  g_free(top);
}

static void refresh_tabstrip(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  GPtrArray *titles = g_ptr_array_new();
  if (p != NULL)
    for (guint i = 0; i < p->tabs->len; i++)
      g_ptr_array_add(titles,
          ((PtTabUI *)g_ptr_array_index(p->tabs, i))->title);
  pt_tab_strip_set_tabs(PT_TAB_STRIP(w->tabstrip), titles,
                        p != NULL ? p->active_tab : -1);
  if (p != NULL)
    for (guint i = 0; i < p->tabs->len; i++)
      pt_tab_strip_set_activity(PT_TAB_STRIP(w->tabstrip), (int)i,
          ((PtTabUI *)g_ptr_array_index(p->tabs, i))->activity);
  g_ptr_array_free(titles, TRUE);
}

static void refresh_statusline(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  PtTabUI *t = active_tab(p);
  PtPaneGrid *grid = t != NULL ? PT_PANE_GRID(t->grid) : NULL;
  pt_statusline_update(PT_STATUSLINE(w->statusline),
      p != NULL ? p->name : NULL,
      (p != NULL && p->is_repo) ? p->git.branch : NULL,
      p != NULL ? p->git.changed : 0,
      p != NULL ? p->active_tab : 0,
      p != NULL ? (int)p->tabs->len : 0,
      grid != NULL ? pt_pane_grid_focused_index(grid) : 0,
      grid != NULL ? pt_pane_grid_pane_count(grid) : 0);
}

static void show_active_grid(PtWindow *w) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(w->content)) != NULL)
    gtk_box_remove(GTK_BOX(w->content), child);
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) {
    t->activity = FALSE;
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
            ? "^⇧W remove · ^1..9 switch project"
            : "[+ project] to add · ^1..9 switch · ^⇧T new tab");
    gtk_widget_add_css_class(keys, "pt-empty-hint");
    gtk_box_append(GTK_BOX(empty), keys);
    gtk_box_append(GTK_BOX(w->content), empty);
  }
  refresh_tabstrip(w);
  refresh_statusline(w);
}

/* ---------- tab/grid plumbing ---------- */
static void on_grid_structure(PtPaneGrid *g, gpointer user) {
  (void)g;
  PtWindow *w = PT_WINDOW(user);
  refresh_statusline(w);
  mark_dirty(w);
}

static void on_grid_focus(PtPaneGrid *g, gpointer user) {
  (void)g;
  refresh_statusline(PT_WINDOW(user));
}

static void on_grid_activity(PtPaneGrid *g, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  PtProjectUI *p = active_project(w);
  if (p == NULL) return;
  for (guint i = 0; i < p->tabs->len; i++) {
    PtTabUI *t = g_ptr_array_index(p->tabs, i);
    if (t->grid == GTK_WIDGET(g) && (int)i != p->active_tab) {
      t->activity = TRUE;
      pt_tab_strip_set_activity(PT_TAB_STRIP(w->tabstrip), (int)i, TRUE);
    }
  }
}

/* Last pane in a grid closed via a clean shell exit → drop the owning tab.
 * The grid may belong to a background project/tab (a background shell can exit),
 * so scan every project. The emitting grid survives this (the idle in
 * pt-pane-grid holds its own ref) even though tab removal unrefs it here. */
static void on_grid_emptied(PtPaneGrid *g, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
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
 * Deliberately does NOT mark_dirty: command churn must not spam saves; the tab
 * title is captured on the next structural save anyway. */
static void on_grid_command(PtPaneGrid *g, const char *comm, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  for (guint pi = 0; pi < w->projects->len; pi++) {
    PtProjectUI *p = g_ptr_array_index(w->projects, pi);
    for (guint ti = 0; ti < p->tabs->len; ti++) {
      PtTabUI *t = g_ptr_array_index(p->tabs, ti);
      if (t->grid != GTK_WIDGET(g)) continue;
      g_free(t->title);
      t->title = g_strdup(comm);
      if ((int)pi == w->active_project)
        refresh_tabstrip(w);
      return;
    }
  }
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
  PtWindow *w = p->window;
  refresh_sidebar(w);
  refresh_statusline(w);
}

static PtProjectUI *project_ui_new(PtWindow *w, const char *name,
                                   const char *path) {
  PtProjectUI *p = g_new0(PtProjectUI, 1);
  p->name = g_strdup(name);
  p->path = g_strdup(path);
  p->window = w;
  p->tabs = g_ptr_array_new_with_free_func(tab_ui_free);
  p->missing = !g_file_test(path, G_FILE_TEST_IS_DIR);
  if (!p->missing) {
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
  g_ptr_array_add(p->tabs, tab_ui_new(w, "shell", pt_split_leaf_new(p->path)));
  p->active_tab = (int)p->tabs->len - 1;
  show_active_grid(w);
  mark_dirty(w);
}

static void action_close_pane(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  PtTabUI *t = active_tab(p);
  if (t == NULL) return;
  if (!pt_pane_grid_close_focused(PT_PANE_GRID(t->grid))) {
    /* last pane closed → close the tab */
    g_ptr_array_remove_index(p->tabs, p->active_tab);
    if (p->active_tab >= (int)p->tabs->len)
      p->active_tab = (int)p->tabs->len - 1;
    show_active_grid(w);
  }
  mark_dirty(w);
}

static void action_split(PtWindow *w, PtSplitKind kind) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_split(PT_PANE_GRID(t->grid), kind);
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

static void on_project_add(PtSidebar *sb, gpointer user) {
  (void)sb;
  PtWindow *w = PT_WINDOW(user);
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(w), NULL,
                                on_folder_chosen, w);
  g_object_unref(dlg);
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

/* Escape in the sidebar search hands the keyboard back to the terminal. */
static void on_search_escape(PtSidebar *sb, gpointer user) {
  (void)sb;
  PtWindow *w = PT_WINDOW(user);
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_focus_terminal(PT_PANE_GRID(t->grid));
}

static void on_tab_selected(PtTabStrip *s, int idx, gpointer user) {
  (void)s;
  action_switch_tab(PT_WINDOW(user), idx);
}

static void on_tab_new(PtTabStrip *s, gpointer user) {
  (void)s;
  action_new_tab(PT_WINDOW(user));
}

/* ---------- shortcuts ---------- */
typedef struct { PtWindow *w; int arg; } ShortcutCtx;

static gboolean sc_project(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget; (void)args;
  ShortcutCtx *c = user;
  action_switch_project(c->w, c->arg);
  return TRUE;
}
static gboolean sc_tab(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget; (void)args;
  ShortcutCtx *c = user;
  action_switch_tab(c->w, c->arg);
  return TRUE;
}
static gboolean sc_new_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_new_tab(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_next_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_next_tab(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_prev_tab(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_prev_tab(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_split_h(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_split(PT_WINDOW(u), PT_SPLIT_H); return TRUE;
}
static gboolean sc_split_v(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_split(PT_WINDOW(u), PT_SPLIT_V); return TRUE;
}
static gboolean sc_close_pane(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_close_pane(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_focus_next(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_focus_next(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_paste(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_paste(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_copy(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_copy(PT_WINDOW(u)); return TRUE;
}
static gboolean sc_zoom_in(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  pt_terminal_set_font_size(pt_terminal_font_size() + 1);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_zoom_out(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  pt_terminal_set_font_size(pt_terminal_font_size() - 1);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_zoom_reset(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  pt_terminal_set_font_size(PT_FONT_SIZE_DEFAULT);
  mark_dirty(PT_WINDOW(u));
  return TRUE;
}
static gboolean sc_focus_dir(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  ShortcutCtx *c = u;
  action_focus_direction(c->w, (PtPaneDirection)c->arg);
  return TRUE;
}
static gboolean sc_focus_prev(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a; action_focus_prev(PT_WINDOW(u)); return TRUE;
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
  add_shortcut(ctl, "<Control><Shift>t", sc_new_tab, w, NULL);
  add_shortcut(ctl, "<Control>Tab", sc_next_tab, w, NULL);
  add_shortcut(ctl, "<Control><Shift>Tab", sc_prev_tab, w, NULL);
  add_keyval_shortcut(ctl, GDK_KEY_ISO_Left_Tab, GDK_CONTROL_MASK,
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
    p->tabs = g_ptr_array_new_with_free_func(tab_ui_free);
    p->missing = !g_file_test(ps->path, G_FILE_TEST_IS_DIR);
    if (!p->missing) {
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
  g_clear_pointer(&w->projects, g_ptr_array_unref);
  G_OBJECT_CLASS(pt_window_parent_class)->dispose(obj);
}

static void pt_window_class_init(PtWindowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_window_dispose;
}

static void pt_window_init(PtWindow *w) {
  w->projects = g_ptr_array_new_with_free_func(project_ui_free);
  w->active_project = -1;

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* top bar: "pt :: <project>" left, no window controls, draggable */
  GtkWidget *handle = gtk_window_handle_new();
  GtkWidget *topbar = gtk_center_box_new();
  gtk_widget_add_css_class(topbar, "pt-topbar");
  w->topbar_label = gtk_label_new("pt");
  gtk_center_box_set_start_widget(GTK_CENTER_BOX(topbar), w->topbar_label);
  gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), topbar);
  gtk_box_append(GTK_BOX(outer), handle);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_vexpand(body, TRUE);
  w->sidebar = pt_sidebar_new();
  gtk_box_append(GTK_BOX(body), w->sidebar);

  GtkWidget *main_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(main_col, TRUE);
  w->tabstrip = pt_tab_strip_new();
  gtk_box_append(GTK_BOX(main_col), w->tabstrip);
  w->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(w->content, TRUE);
  gtk_box_append(GTK_BOX(main_col), w->content);
  w->statusline = pt_statusline_new();
  gtk_box_append(GTK_BOX(main_col), w->statusline);
  gtk_box_append(GTK_BOX(body), main_col);
  gtk_box_append(GTK_BOX(outer), body);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(w), outer);

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
}

GtkWidget *pt_window_new(AdwApplication *app) {
  return g_object_new(PT_TYPE_WINDOW, "application", app,
                      "title", "pt",
                      "default-width", 1100, "default-height", 700, NULL);
}
