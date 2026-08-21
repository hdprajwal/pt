#include "pt-window.h"

#include <glib/gstdio.h>   /* g_mkdir_with_parents */
#include <string.h>        /* strrchr */

#include "pt-terminal.h"
#include "pt-term-core.h"  /* pt_term_core_set_term, the `term` config key */
#include "pt-sidebar.h"
#include "pt-info-panel.h"
#include "pt-tab-strip.h"
#include "pt-statusline.h"
#include "pt-project-bar.h"
#include "pt-pane-grid.h"
#include "pt-command-palette.h"
#include "pt-settings.h"
#include "pt-session.h"
#include "pt-git-parse.h"
#include "pt-git-monitor.h"
#include "pt-path.h"
#include "pt-config.h"
#include "pt-theme.h"
#include "pt-style.h"
#include "pt-workspace.h"
#include "pt-agent-session.h"
#include "pt-agent-latch.h"

/* The window no longer keeps the project/tab structure itself: PtWorkspace
 * (ptcore, headless) owns order and selection, addressed by stable ids. The
 * structs below are the widget half — what a workspace id hangs its UI on,
 * stored in the id's pt_workspace_set_data slot. The window frees them; the
 * workspace only drops the slot. */

typedef struct {
  PtWsId id;            /* this tab in the workspace */
  char *title;
  GtkWidget *grid;      /* PtPaneGrid, owned ref */
  PtWindow *window;     /* back-pointer; the data the grid handlers carry */
} PtTabUI;

typedef struct {
  PtWsId id;            /* this project in the workspace */
  gboolean missing;
  PtGitStatus git;
  GPtrArray *git_files;  /* PtGitFile*, owned copy of the monitor's list */
  gboolean is_repo;
  PtGitMonitor *monitor;
  PtWindow *window;     /* back-pointer; Task 11 wires git monitors through it */
} PtProjectUI;

struct _PtWindow {
  AdwApplicationWindow parent_instance;
  PtWorkspace *ws;      /* NULL after dispose, like projects used to be */
  GtkWidget *sidebar, *tabstrip, *content, *statusline, *projectbar;
  GtkWidget *infopanel;  /* right rail; hidden until ⌃I */
  PtAgentMonitor *agents; /* the panel's agent-usage block; polls only while shown */
  GtkWidget *palette;   /* overlay child; hidden unless ⌃K is up */
  GtkWidget *settings;  /* overlay child, same stack as the palette; ⌃, */
  guint save_source;    /* debounce timer; used from Task 12 */
  guint status_source;  /* 500ms progress poll for the status bar */
  guint sidebar_idle;   /* pending coalesced refresh_sidebar; 0 = none */
  gboolean close_confirm_open;  /* a close-shell dialog is up; do not stack */
  PtConfig *config;
  GFileMonitor *config_monitor;
  GFileMonitor *theme_monitor;
  GFileMonitor *agent_report_monitor;  /* watches the agent-sessions dir */
  PtAgentLatch *agent_notified;        /* per-pane dedupe for lifecycle reports */
  gboolean limit_notified;             /* a limit-hit episode is latched */
  /* Separate debounces: a themes-dir event must never swallow a pending
   * config reload (or the config edit that armed it would be lost). */
  guint config_reload_source;
  guint theme_reload_source;
  guint config_save_source;     /* debounce */
  /* The accent hexes handed to spawned shells as PT_ACCENT, in "#rrggbb" form.
   * Filled from the resolved theme's accent-0..5 tokens on every render, which
   * is once per config; a project's env is built per spawn and reads whichever
   * one its accent points at. Seeded with pt-dark's accents in init so an env
   * built before the first render still carries a sane value. */
  char accent_hex[PT_ACCENT_COUNT][8];
};

G_DEFINE_FINAL_TYPE(PtWindow, pt_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---------- helpers ---------- */
static PtProjectUI *active_project(PtWindow *w) {
  /* NULL after dispose: a dialog response or a queued grid signal can still
   * land on the window after its workspace is gone. */
  if (w->ws == NULL) return NULL;
  return pt_workspace_get_data(w->ws, pt_workspace_active_project(w->ws));
}

static PtTabUI *active_tab(PtProjectUI *p) {
  if (p == NULL || p->window->ws == NULL) return NULL;
  return pt_workspace_get_data(p->window->ws,
                               pt_workspace_active_tab(p->window->ws, p->id));
}

/* The project fields the workspace owns, read back through the UI struct that
 * hangs off it. Borrowed strings; valid while the project is. */
static const char *proj_name(const PtProjectUI *p) {
  return pt_workspace_project_name(p->window->ws, p->id);
}

static const char *proj_path(const PtProjectUI *p) {
  return pt_workspace_project_path(p->window->ws, p->id);
}

static int proj_accent(const PtProjectUI *p) {
  return pt_workspace_project_accent(p->window->ws, p->id);
}

/* Grid → owning tab, the same trick pt-pane-grid plays with "pt-leaf" on a
 * terminal. Grid signals arrive carrying the grid and nothing else while every
 * handler wants its tab, so without this each one walked every project's tabs
 * to find it. Set in tab_ui_new and cleared in tab_ui_free: a grid can outlive
 * its tab (pt-pane-grid's idle holds its own ref), and an unset back-pointer is
 * exactly the "no tab owns this grid any more" answer the close paths want,
 * where a stale one would be a read after free. */
#define PT_GRID_TAB_KEY "pt-grid-tab"

static PtTabUI *grid_tab(PtPaneGrid *g) {
  return g != NULL ? g_object_get_data(G_OBJECT(g), PT_GRID_TAB_KEY) : NULL;
}

/* The tab that owns a grid, provided the workspace still knows it. NULL when
 * no live tab owns it: confirmation is async, so by response time the tab may
 * have been dropped already (its last shell exited cleanly → on_grid_emptied
 * removed it) — its id is then dead, and a dead id resolves to nothing rather
 * than to whatever slid into the old slot. No scan: the tab pointer comes from
 * the grid, the liveness check is one id lookup. */
static PtTabUI *find_grid_tab(PtWindow *w, PtPaneGrid *g) {
  PtTabUI *t = grid_tab(g);
  if (w->ws == NULL || t == NULL) return NULL;
  if (pt_workspace_tab_project(w->ws, t->id) == PT_WS_ID_NONE) return NULL;
  return t;
}

static void mark_dirty(PtWindow *w);   /* persistence hook; body in Task 12 */
/* Wired up in tab_ui_new, but written down with the rest of the notification
 * code further on — it needs find_grid and the project/tab switches. */
static void on_grid_notification(PtPaneGrid *g, guint64 pane_id,
                                 const char *title, const char *body,
                                 gpointer user);

/* ---------- config ---------- */

/* The last theme parse, kept between renders: the settings dialog emits
 * "changed" at key-repeat rate when an arrow is held on a row, and re-reading
 * and re-parsing the theme file per repeat is pure waste when neither the
 * name nor the file moved. Keyed by name + file stamp, and dropped outright
 * when the themes-dir monitor fires — belt over the stamp's braces for a
 * same-size rewrite on a filesystem without subsecond mtimes. Module-level
 * rather than per-window: the themes dir is per-user. */
static char *theme_cache_name;
static char *theme_cache_stamp;   /* NULL = no file backing (builtin/missing) */
static PtTheme *theme_cache_theme;

static void theme_cache_drop(void) {
  g_clear_pointer(&theme_cache_name, g_free);
  g_clear_pointer(&theme_cache_stamp, g_free);
  g_clear_pointer(&theme_cache_theme, pt_theme_free);
}

/* mtime.usec+size of the named theme's file — subsecond, same fingerprint the
 * pt_theme_is_dark cache uses, so a same-size in-place rewrite within one
 * second still moves the stamp and a settings preview racing the monitor
 * delivery cannot render a stale parse. NULL when there is no file (the
 * builtin, or a missing name — both render the same fallback text every time,
 * so "no file" is itself a valid stamp for the cached name). */
static char *theme_file_stamp(const char *dir, const char *name) {
  char *path = g_build_filename(dir, name, NULL);
  GFile *f = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(
      f,
      G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC
      "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_object_unref(f);
  g_free(path);
  if (info == NULL) return NULL;
  char *stamp = g_strdup_printf(
      "%" G_GUINT64_FORMAT ".%06u:%" G_GINT64_FORMAT,
      g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
      g_file_info_get_attribute_uint32(info,
                                       G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
      (gint64)g_file_info_get_size(info));
  g_object_unref(info);
  return stamp;
}

/* Parse `cfg`'s theme and push colors+fonts everywhere. Deliberately takes the
 * config rather than reading w->config: the settings dialog previews a
 * candidate it still owns, and nothing about rendering it may put that
 * candidate anywhere the debounced save could later find it. `w` is only
 * written for the resolved accent hexes, which are not config state. */
static void render_config(PtWindow *w, const PtConfig *cfg) {
  char *tdir = pt_theme_dir();
  char *stamp = theme_file_stamp(tdir, cfg->theme);
  if (theme_cache_theme == NULL ||
      g_strcmp0(theme_cache_name, cfg->theme) != 0 ||
      g_strcmp0(theme_cache_stamp, stamp) != 0) {
    char *text = pt_theme_load_text(tdir, cfg->theme);
    if (text == NULL) {
      g_warning("pt: theme '%s' not found; using pt-dark", cfg->theme);
      text = g_strdup(pt_theme_builtin_pt_dark());
    }
    theme_cache_drop();
    theme_cache_theme = pt_theme_parse(text);
    theme_cache_name = g_strdup(cfg->theme);
    theme_cache_stamp = g_steal_pointer(&stamp);
    g_free(text);
  }
  g_free(stamp);
  /* Resolution is re-run every time — it is 33 color derivations, and it must
   * see the config's app_overrides, which change independently of the file. */
  PtResolvedTheme rt;
  pt_theme_resolve(theme_cache_theme, cfg->app_overrides, &rt);
  pt_style_apply(&rt, cfg);
  /* Push the theme into the terminals only when it moved: a font-size drag
   * re-renders per key-repeat with an identical theme, and set_theme repaints
   * every pane. memcmp is safe here because pt_theme_resolve zeroes the whole
   * struct before filling it; a stray padding byte could at worst force a
   * redundant push, never skip a real change. */
  static PtResolvedTheme last_pushed;
  static gboolean theme_pushed;
  if (!theme_pushed || memcmp(&last_pushed, &rt, sizeof rt) != 0) {
    pt_terminal_set_theme(&rt);
    last_pushed = rt;
    theme_pushed = TRUE;
  }
  pt_terminal_set_font(cfg->font_family, cfg->font_size);
  pt_terminal_set_mouse_reporting(cfg->mouse_reporting);
  pt_terminal_set_osc52(cfg->osc52);
  pt_terminal_set_padding(cfg->window_padding_x, cfg->window_padding_y);
  /* Spawn-time, so editing this reaches the next pane rather than the open
   * ones; ghostty's scrollback-limit works the same way. */
  pt_terminal_set_scrollback_limit((gsize)cfg->scrollback_limit);
  /* Spawn-time for the same reason: a running child read $TERM once, at exec,
   * and there is no telling it otherwise afterwards. */
  pt_term_core_set_term(cfg->term);
  /* Follows the file both ways: editing claude-usage back to false stops the
   * lookups and clears what they fetched. */
  pt_agent_monitor_set_claude_enabled(w->agents, cfg->claude_usage);
  /* Prompts read PT_ACCENT, so it has to track the theme the chrome uses.
   * Accents always resolve opaque, so the css form is "#rrggbb". */
  for (int i = 0; i < PT_ACCENT_COUNT; i++) {
    char *hex = pt_color_to_css(&rt.tokens[PT_TOK_ACCENT_0 + i]);
    g_strlcpy(w->accent_hex[i], hex, sizeof w->accent_hex[i]);
    g_free(hex);
  }
  g_free(tdir);
}

/* NULL config = disposed window, same guard convention as active_project(). */
static void apply_config(PtWindow *w) {
  if (w->config != NULL) render_config(w, w->config);
}

static gboolean config_save_now(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  w->config_save_source = 0;
  char *path = pt_config_default_path();
  GError *err = NULL;
  if (!pt_config_save(w->config, path, &err)) {
    g_warning("pt: config save failed: %s", err->message);
    g_clear_error(&err);
  }
  g_free(path);
  return G_SOURCE_REMOVE;
}

static void config_save_soon(PtWindow *w) {
  if (w->config_save_source != 0) g_source_remove(w->config_save_source);
  w->config_save_source = g_timeout_add(1000, config_save_now, w);
}

static gboolean config_reload_now(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  w->config_reload_source = 0;
  /* Never re-render underneath an open settings dialog: the window shows the
   * dialog's candidate, and re-applying w->config here would snap it back
   * mid-preview. An external edit made during those few seconds is simply not
   * adopted: the file keeps it, and the next config-file event (or the next
   * launch) picks it up. */
  if (w->settings != NULL && pt_settings_is_open(PT_SETTINGS(w->settings)))
    return G_SOURCE_REMOVE;
  /* A save of ours is still queued, so memory is newer than disk: reloading
   * here would resurrect the pre-edit file. Closes the 150ms hole where a
   * zoom at t=1100 gets reverted by the reload our own t=1000 write triggers
   * at t=1150. The save itself lands next, and its echo reloads a matching
   * file, so nothing is lost by skipping this one. */
  if (w->config_save_source != 0) return G_SOURCE_REMOVE;
  char *path = pt_config_default_path();
  PtConfig *fresh = pt_config_load(path);
  g_free(path);
  /* Self-writes and no-op saves land here too; identical config = no work.
   * This is also the guard that stops save->monitor->reload feedback. */
  if (pt_config_equal(fresh, w->config)) {
    pt_config_free(fresh);
    return G_SOURCE_REMOVE;
  }
  pt_config_free(w->config);
  w->config = fresh;
  apply_config(w);
  return G_SOURCE_REMOVE;
}

static void on_config_changed(GFileMonitor *m, GFile *f, GFile *other,
                              GFileMonitorEvent ev, gpointer user) {
  (void)m; (void)f; (void)other;
  if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      ev != G_FILE_MONITOR_EVENT_CHANGED &&
      ev != G_FILE_MONITOR_EVENT_CREATED)
    return;
  PtWindow *w = PT_WINDOW(user);
  if (w->config_reload_source != 0) g_source_remove(w->config_reload_source);
  w->config_reload_source = g_timeout_add(150, config_reload_now, w);
}

/* Theme-file edits reuse the same debounce but must force a re-apply even
 * though the config text itself is unchanged. */
static gboolean theme_reload_now(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  w->theme_reload_source = 0;
  /* Same as config_reload_now: the candidate on screen wins while the dialog
   * is open — but a theme-file edit must still reach the preview, so the
   * candidate itself is re-rendered rather than skipped (re-applying w->config
   * here would snap the preview back mid-edit). Notably the mkdir in
   * action_open_settings makes the themes dir fire a CREATED event on the
   * first-ever ⌃,; rendering the candidate again is a no-op for it. */
  if (w->settings != NULL && pt_settings_is_open(PT_SETTINGS(w->settings))) {
    const PtConfig *cand = pt_settings_config(PT_SETTINGS(w->settings));
    if (cand != NULL) render_config(w, cand);
    return G_SOURCE_REMOVE;
  }
  apply_config(w);
  return G_SOURCE_REMOVE;
}

static void on_theme_file_changed(GFileMonitor *m, GFile *f, GFile *other,
                                  GFileMonitorEvent ev, gpointer user) {
  (void)m; (void)f; (void)other;
  if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
      ev != G_FILE_MONITOR_EVENT_CHANGED &&
      ev != G_FILE_MONITOR_EVENT_CREATED)
    return;
  PtWindow *w = PT_WINDOW(user);
  /* Dropped immediately, not from the debounced reload: the reload bails while
   * the settings dialog is open, but the dialog's own previews keep calling
   * render_config and must see the edited file, not the cached parse. */
  theme_cache_drop();
  if (w->theme_reload_source != 0) g_source_remove(w->theme_reload_source);
  w->theme_reload_source = g_timeout_add(150, theme_reload_now, w);
}

static void watch_config(PtWindow *w) {
  char *cpath = pt_config_default_path();
  GFile *cf = g_file_new_for_path(cpath);
  w->config_monitor = g_file_monitor_file(cf, G_FILE_MONITOR_NONE, NULL, NULL);
  if (w->config_monitor != NULL)
    g_signal_connect(w->config_monitor, "changed",
                     G_CALLBACK(on_config_changed), w);
  g_object_unref(cf);
  g_free(cpath);
  /* Watch the whole themes dir: covers the active theme plus new files the
   * settings dialog should discover. */
  char *tdir = pt_theme_dir();
  GFile *tf = g_file_new_for_path(tdir);
  w->theme_monitor = g_file_monitor(tf, G_FILE_MONITOR_NONE, NULL, NULL);
  if (w->theme_monitor != NULL)
    g_signal_connect(w->theme_monitor, "changed",
                     G_CALLBACK(on_theme_file_changed), w);
  g_object_unref(tf);
  g_free(tdir);
}

/* Loads the config, applies it, and starts watching. `state` is the session
 * this window just restored (NULL when there is none) and only ever seeds the
 * font size, once, for users upgrading from state.json-only font handling. */
static void init_config(PtWindow *w, PtSessionState *state) {
  char *cfg_path = pt_config_default_path();
  char *cfg_text = NULL;
  gboolean had_file = g_file_get_contents(cfg_path, &cfg_text, NULL, NULL);
  w->config = had_file ? pt_config_parse(cfg_text) : pt_config_new();
  /* Migration: a state.json font_size seeds the default exactly once, when
   * the config file has no font-size line of its own. */
  gboolean has_font_line =
      had_file && g_regex_match_simple("^\\s*font-size\\s*=", cfg_text,
                                       G_REGEX_MULTILINE, 0);
  gboolean seeded = !has_font_line && state != NULL;
  if (seeded)
    w->config->font_size = state->font_size;
  g_free(cfg_text);
  g_free(cfg_path);
  pt_style_init(gtk_widget_get_display(GTK_WIDGET(w)));
  apply_config(w);
  watch_config(w);
  /* Write the seed out once. Without this the config file still has no
   * font-size line, so the first unrelated external edit reloads the plain
   * default and the user watches their font jump. */
  if (seeded) config_save_soon(w);
}

static void refresh_projectbar(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  pt_project_bar_update(PT_PROJECT_BAR(w->projectbar),
      p != NULL ? proj_name(p) : "pt",
      p != NULL ? proj_path(p) : "",
      (p != NULL && p->is_repo) ? &p->git : NULL,
      p != NULL ? proj_accent(p) : 0);
}

static void refresh_sidebar(PtWindow *w) {
  int n = (int)pt_workspace_project_count(w->ws);
  PtSidebarRow *rows = g_new0(PtSidebarRow, n);
  for (int i = 0; i < n; i++) {
    PtWsId id = pt_workspace_project_at(w->ws, (guint)i);
    PtProjectUI *p = pt_workspace_get_data(w->ws, id);
    rows[i].name = pt_workspace_project_name(w->ws, id);
    rows[i].path = pt_workspace_project_path(w->ws, id);
    rows[i].missing = p->missing;
    rows[i].is_repo = p->is_repo;
    rows[i].git = p->git;
    rows[i].accent = pt_workspace_project_accent(w->ws, id);
    guint tabs = pt_workspace_tab_count(w->ws, id);
    rows[i].shell_count = (int)tabs;
    int running = 0;
    for (guint j = 0; j < tabs; j++) {
      PtTabUI *t =
          pt_workspace_get_data(w->ws, pt_workspace_tab_at(w->ws, id, j));
      if (pt_pane_grid_any_running(PT_PANE_GRID(t->grid))) running++;
    }
    rows[i].running = running;
  }
  guint active_idx =
      pt_workspace_project_index(w->ws, pt_workspace_active_project(w->ws));
  pt_sidebar_set_projects(PT_SIDEBAR(w->sidebar), rows, n,
                          active_idx == PT_WS_INDEX_NONE ? -1
                                                         : (int)active_idx);
  g_free(rows);
  PtProjectUI *ap = active_project(w);
  char *top = ap != NULL ? g_strdup_printf("pt :: %s", proj_name(ap))
                         : g_strdup("pt");
  /* GTK4 does not dedupe this; each call is a Wayland round-trip, and this
   * runs on every foreground-command change. */
  if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), top) != 0)
    gtk_window_set_title(GTK_WINDOW(w), top);
  g_free(top);
  /* cheap enough to redo unconditionally, and never drifts out of sync */
  refresh_projectbar(w);
}

/* refresh_sidebar walks every pane of every project, so callers that can fire
 * in bursts (one "command-changed" per pane after a broadcast keystroke, git
 * monitors of several projects polling together) queue it instead: however
 * many requests land in one main-loop iteration, the walk happens once. The
 * pending source id doubles as the dirty flag; dispose removes it, so the
 * callback can never see a dead window. */
static gboolean sidebar_refresh_idle(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  w->sidebar_idle = 0;
  refresh_sidebar(w);
  return G_SOURCE_REMOVE;
}

static void queue_refresh_sidebar(PtWindow *w) {
  if (w->sidebar_idle == 0)
    w->sidebar_idle = g_idle_add(sidebar_refresh_idle, w);
}

static void refresh_tabstrip(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  int n = p != NULL ? (int)pt_workspace_tab_count(w->ws, p->id) : 0;
  /* infos[].title borrows each tab's live string; the strip copies it before
   * this returns, and nothing here can free a title in between. */
  PtTabInfo *infos = g_new0(PtTabInfo, n);
  for (int i = 0; i < n; i++) {
    PtWsId id = pt_workspace_tab_at(w->ws, p->id, (guint)i);
    PtTabUI *t = pt_workspace_get_data(w->ws, id);
    PtPaneGrid *grid = PT_PANE_GRID(t->grid);
    PtTerminal *foc = pt_pane_grid_focused_terminal(grid);
    infos[i].title = t->title;
    infos[i].id = id;
    infos[i].running = pt_pane_grid_any_running(grid);
    infos[i].last_exit = foc != NULL ? pt_terminal_last_exit(foc) : -1;
  }
  guint active_ti =
      p != NULL ? pt_workspace_tab_index(w->ws,
                                         pt_workspace_active_tab(w->ws, p->id))
                : PT_WS_INDEX_NONE;
  pt_tab_strip_set_tabs(PT_TAB_STRIP(w->tabstrip), infos, n,
                        active_ti == PT_WS_INDEX_NONE ? -1 : (int)active_ti,
                        p != NULL ? proj_accent(p) : 0);
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
  /* Only a running command can have progress, so the row read is skipped while
   * the pane sits at a prompt. Progress counters live on the last non-empty
   * row of the visible grid; anything above it is scrollback of an older
   * state. `running` implies the core exists (pt_terminal_running is FALSE
   * before spawn). */
  if (running) {
    task = pt_terminal_last_command(term);
    char row[1024];
    if (pt_term_core_last_nonempty_row(pt_terminal_core(term), row, sizeof row))
      has_prog = pt_progress_parse_line(row, &prog);
  }
  pt_statusline_update(PT_STATUSLINE(w->statusline), running, last_exit,
                       has_prog ? &prog : NULL, task,
                       p != NULL ? proj_accent(p) : 0);
}

/* ---------- info panel ---------- */
/* The directory the panel shows and its three buttons act on: the focused
 * pane's cwd, falling back to the project root. NULL when there is neither.
 * Caller frees. */
static char *panel_dir(PtWindow *w) {
  PtTerminal *term = focused_terminal(w);
  char *cwd = term != NULL ? pt_terminal_current_cwd(term) : NULL;
  if (cwd != NULL) return cwd;
  PtProjectUI *p = active_project(w);
  return p != NULL ? g_strdup(proj_path(p)) : NULL;
}

/* The shell's own name, not the foreground command: the pane's shell pid is a
 * direct child, so its comm stays "zsh" while a build runs under it. The core
 * derives it at spawn, so this costs nothing; $SHELL/"sh" cover a pane with no
 * live core (a tab restored into the background, a failed respawn). Borrowed,
 * not freed. */
static const char *shell_name_for(PtTerminal *term) {
  const char *name = term != NULL ? pt_terminal_shell_name(term) : NULL;
  if (name != NULL) return name;
  const char *sh = g_getenv("SHELL");
  if (sh == NULL || sh[0] == '\0') return "sh";
  const char *slash = strrchr(sh, '/');
  return slash != NULL ? slash + 1 : sh;
}

/* Draw the agent block from whatever the monitor currently holds. Split out
 * from refresh_infopanel because the monitor calls it back when a lookup
 * lands, and going the long way round would re-run detection from inside the
 * detector's own callback. */
static void redraw_agent_usage(PtWindow *w) {
  if (w->infopanel == NULL || w->agents == NULL) return;
  PtAgentView view;
  pt_agent_monitor_view(w->agents, &view);
  pt_info_panel_set_usage(PT_INFO_PANEL(w->infopanel), &view,
                          g_get_real_time() / G_USEC_PER_SEC);
}

/* One "limit reached" per episode: the latch re-arms only when usage drops
 * back under the threshold, so a reading pinned at the cap cannot re-notify
 * on every poll. Fires only while the info panel is closed — whoever has it
 * open is looking straight at the bars. */
#define PT_LIMIT_REARM_PCT 90.0

static void maybe_notify_limit(PtWindow *w) {
  if (w->agents == NULL || w->ws == NULL) return;
  PtAgentView view;
  pt_agent_monitor_view(w->agents, &view);
  double max_pct = 0.0;
  if (view.usage != NULL)
    for (int i = 0; i < view.usage->n_windows; i++)
      if (view.usage->windows[i].percent > max_pct)
        max_pct = view.usage->windows[i].percent;
  gboolean hit = view.usage != NULL && view.usage->limit_hit &&
                 max_pct >= PT_LIMIT_REARM_PCT;
  /* Pressure off: the next episode may notify again. */
  if (!hit) {
    w->limit_notified = FALSE;
    return;
  }
  if (w->limit_notified || gtk_widget_get_visible(w->infopanel)) return;
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w));
  if (app == NULL) return;
  w->limit_notified = TRUE;

  const char *label = pt_agent_label(view.kind);
  char *body = g_strdup_printf(
      "%s hit a plan limit — the info panel has the details.",
      label != NULL ? label : "the agent");
  GNotification *n = g_notification_new("Usage limit reached");
  g_notification_set_body(n, body);
  GIcon *icon = g_themed_icon_new("dev.hdprajwal.pt");
  g_notification_set_icon(n, icon);
  g_object_unref(icon);
  g_application_send_notification(G_APPLICATION(app), "agent-limit", n);
  g_object_unref(n);
  g_free(body);
}

static void on_agent_usage_changed(gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  redraw_agent_usage(w);
  maybe_notify_limit(w);
}

/* Hidden is the default state, and the 500ms tick calls this unconditionally —
 * so a hidden panel must cost nothing at all. */
static void refresh_infopanel(PtWindow *w) {
  if (w->infopanel == NULL || !gtk_widget_get_visible(w->infopanel)) return;
  PtProjectUI *p = active_project(w);
  PtTerminal *term = focused_terminal(w);
  /* A pane that has never been allocated (a tab restored into the background,
   * or the first frame of a fresh one) has no core yet, and there is no pid to
   * show until it does. */
  PtTermCore *core = term != NULL ? pt_terminal_core(term) : NULL;
  int pid = core != NULL ? (int)pt_term_core_shell_pid(core) : 0;
  const char *shell = shell_name_for(term);
  char *dir = panel_dir(w);
  pt_info_panel_set_info(PT_INFO_PANEL(w->infopanel), shell, pid,
                         dir != NULL ? dir : "",
                         p != NULL ? proj_accent(p) : 0);
  /* The pane's foreground command is already polled for the tab title, so
   * handing it over here means the usual case — an agent typed at the prompt —
   * is detected without touching /proc at all. The monitor decides for itself
   * whether anything is due; this call is cheap by design. */
  pt_agent_monitor_observe(w->agents, TRUE, pid,
                           term != NULL ? pt_terminal_last_command(term) : NULL,
                           dir);
  redraw_agent_usage(w);
  g_free(dir);
  PtGitStatus none = {0};
  pt_info_panel_set_git(PT_INFO_PANEL(w->infopanel),
                        p != NULL ? &p->git : &none,
                        p != NULL && p->is_repo,
                        p != NULL ? p->git_files : NULL);
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
  /* The panel has no signal for "the focused pane cd'd" or "focus moved", so
   * it rides this poll. No-op while hidden. */
  refresh_infopanel(w);
  return G_SOURCE_CONTINUE;
}

static void show_active_grid(PtWindow *w) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(w->content)) != NULL)
    gtk_box_remove(GTK_BOX(w->content), child);
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) {
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
 * whose dispose already dropped w->ws. NULL there means "gone" — bail before
 * touching the model (or re-arming a save through mark_dirty). */
static void prune_agent_latch(PtWindow *w);

static void on_grid_structure(PtPaneGrid *g, gpointer user) {
  (void)g;
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  prune_agent_latch(w);
  refresh_statusline(w);
  mark_dirty(w);
}

static void on_grid_focus(PtPaneGrid *g, gpointer user) {
  (void)g;
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  /* The user is looking at this pane again, so its last raised lifecycle
   * event is old news by the time the next one lands. Re-arming here is what
   * keeps turn N+1's turn-complete notifying after turn N's did: Claude's
   * Stop hook always reports the same name, and without a re-arm the latch
   * would swallow every turn after the first. */
  PtTerminal *foc = pt_pane_grid_focused_terminal(g);
  if (foc != NULL) {
    const char *tok = pt_term_core_pane_token(pt_terminal_core(foc));
    if (tok != NULL) pt_agent_latch_rearm(w->agent_notified, tok);
  }
  refresh_statusline(w);
}

static void tab_ui_free(gpointer data);   /* body below, with tab_ui_new */

/* The notification latch keys on pane tokens, and a closed pane's token dies
 * with it — its report file is swept separately — so its entry would
 * otherwise sit in the table forever. Every structural change walks the panes
 * that are still alive and drops entries for tokens nobody owns now. */
static void collect_live_tokens(PtSplitNode *n, GPtrArray *acc) {
  if (n == NULL) return;
  if (n->kind != PT_SPLIT_LEAF) {
    collect_live_tokens(n->a, acc);
    collect_live_tokens(n->b, acc);
    return;
  }
  if (n->user == NULL) return;
  const char *tok =
      pt_term_core_pane_token(pt_terminal_core(PT_TERMINAL(n->user)));
  if (tok != NULL) g_ptr_array_add(acc, (gpointer)tok);
}

static void prune_agent_latch(PtWindow *w) {
  if (w->agent_notified == NULL) return;
  GPtrArray *live = g_ptr_array_new();
  for (guint pi = 0; pi < pt_workspace_project_count(w->ws); pi++) {
    PtWsId proj = pt_workspace_project_at(w->ws, pi);
    guint tabs = pt_workspace_tab_count(w->ws, proj);
    for (guint ti = 0; ti < tabs; ti++) {
      PtTabUI *t = pt_workspace_get_data(
          w->ws, pt_workspace_tab_at(w->ws, proj, ti));
      collect_live_tokens(pt_pane_grid_tree(PT_PANE_GRID(t->grid)), live);
    }
  }
  pt_agent_latch_prune(w->agent_notified,
                       (const char *const *)live->pdata, live->len);
  g_ptr_array_free(live, TRUE);
}

/* Drop a tab and everything under it — freeing the UI struct unparents the
 * grid, which kills its panes and their PTYs. Every "the tab is going away"
 * path ends here: a clean shell exit, the last pane closing, the tab's ×
 * button. Active-tab succession is the workspace's job now (the tab that
 * slides into the slot, else the new last). */
static void remove_tab(PtWindow *w, PtTabUI *t) {
  gboolean was_visible = pt_workspace_tab_project(w->ws, t->id) ==
                         pt_workspace_active_project(w->ws);
  pt_workspace_remove_tab(w->ws, t->id);
  /* The tab's panes are gone with it: drop their latch entries now, while
   * the workspace no longer lists the tab (the prune walks what is left). */
  prune_agent_latch(w);
  tab_ui_free(t);
  if (was_visible) show_active_grid(w);
  refresh_sidebar(w);   /* shell count dropped; do not wait for the poll */
  mark_dirty(w);
}

/* Last pane in a grid closed via a clean shell exit → drop the owning tab.
 * The grid may belong to a background project/tab (a background shell can exit),
 * which is why the lookup goes through find_grid_tab rather than assuming the
 * active tab. The emitting grid survives this (the idle in pt-pane-grid holds
 * its own ref) even though tab removal unrefs it here. */
static void on_grid_emptied(PtPaneGrid *g, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  PtTabUI *t = find_grid_tab(w, g);
  if (t == NULL) return;
  remove_tab(w, t);
}

/* The name a tab wears: the focused pane's own title while a program is
 * running there, and the foreground command otherwise, which is what pt showed
 * everywhere before. Claude Code and Codex both publish a far better label
 * than their process name — "✳ Claude Code", then a summary of the session —
 * and a pane full of agents is otherwise a row of tabs all reading "claude".
 * Titles go through verbatim, spinner glyph included: that the glyph moves is
 * how the tab says the agent is still going.
 * Prompt-set titles never reach last_title, and it is dropped at every prompt
 * that carries the snippet's exit marker — so with share/prompt/pt-prompt.*
 * sourced (opt-in, per the README) this answers with neither the cwd nor a
 * name the previous program left behind. Without the snippet nothing clears
 * last_title, and a tab can keep wearing the title of a program that has
 * already finished; an accepted tradeoff, and one the snippet fixes.
 * NULL when the tab has no focused pane at all, and before the first comm poll
 * of a pane that set no title. */
static const char *tab_label_for(PtTabUI *t) {
  PtTerminal *foc = pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid));
  if (foc == NULL) return NULL;
  const char *title = pt_terminal_last_title(foc);
  if (pt_terminal_running(foc) && title != NULL && *title != '\0') return title;
  return pt_terminal_last_command(foc);
}

static void set_tab_label(PtTabUI *t) {
  const char *label = tab_label_for(t);
  if (label == NULL || g_strcmp0(label, t->title) == 0) return;
  g_free(t->title);
  t->title = g_strdup(label);
}

/* Focused pane's foreground program changed → relabel the owning tab live.
 * A comm change is exactly when run-state flips, so refresh the strip (dots)
 * and the sidebar (run counters) regardless of whether the title moved; both
 * are throttled by the 700ms comm poll upstream.
 * Deliberately does NOT mark_dirty: command churn must not spam saves; the tab
 * title is captured on the next structural save anyway. */
static void on_grid_command(PtPaneGrid *g, const char *comm, gpointer user) {
  (void)comm;   /* the label comes from the pane: its title first, then this */
  PtWindow *w = PT_WINDOW(user);
  PtTabUI *t = find_grid_tab(w, g);
  if (t == NULL) return;
  set_tab_label(t);
  if (pt_workspace_tab_project(w->ws, t->id) ==
      pt_workspace_active_project(w->ws)) {
    refresh_tabstrip(w);
    /* run-state just flipped for a pane of the visible project: this is the
     * edge the 500ms poll deliberately does not cover (it only runs while
     * something is running), so the "✓ / ✗ exit N" settle happens here. */
    refresh_statusline(w);
  }
  queue_refresh_sidebar(w);
}

/* A title change is both the pane's new name and, when the prompt set it, the
 * earliest moment the "✓ / ✗ exit N" marker can settle — a failing builtin
 * (`false`) never moves the foreground comm, so waiting for "command-changed"
 * would leave the bar stale until the next poll. Goes through find_grid_tab
 * rather than the active tab so a background tab relabels too: an agent left
 * running in another tab is exactly the one whose name is worth watching.
 * The statusline stops one step short of that, at the active tab. It speaks
 * for focused_terminal(w) — the active tab's focused pane — so a title from a
 * background tab of the same project recomputes an identical bar, and not for
 * free: the refresh scans the grid for its last non-empty row and parses it
 * for progress. An agent spins a braille frame through the title about once a
 * second, per tab. The renderers do dedupe their output, but that scan is
 * upstream of the dedupe. */
static void on_grid_title(PtPaneGrid *g, const char *title, gpointer user) {
  (void)title;
  PtWindow *w = PT_WINDOW(user);
  PtTabUI *t = find_grid_tab(w, g);
  if (t == NULL) return;
  set_tab_label(t);
  if (pt_workspace_tab_project(w->ws, t->id) !=
      pt_workspace_active_project(w->ws))
    return;
  refresh_tabstrip(w);
  if (t == active_tab(active_project(w))) refresh_statusline(w);
}

/* PT_BRANCH has to be right for a project's *first* shells, but the git
 * monitor's first poll is async and lands long after they spawn — without this
 * every restored shell (i.e. every shell at app start) got PT_BRANCH="".
 * Reading .git/HEAD is a single small file read, no subprocess; the monitor
 * overwrites p->git with the authoritative status shortly after.
 * Leaves the branch untouched when there is no .git/HEAD (non-repo, or a
 * worktree/submodule whose .git is a gitdir: pointer — the monitor covers it). */
static void seed_git_branch(PtProjectUI *p) {
  char *head = g_build_filename(proj_path(p), ".git", "HEAD", NULL);
  char *txt = NULL;
  if (g_file_get_contents(head, &txt, NULL, NULL)) {
    char *branch = pt_git_parse_head(txt);
    if (branch != NULL)
      g_strlcpy(p->git.branch, branch, sizeof p->git.branch);
    else if (*g_strstrip(txt) != '\0')
      /* HEAD said something, just not a branch: a detached sha. An empty file
       * says nothing, and leaves the branch for the monitor's first poll. */
      g_strlcpy(p->git.branch, "(detached)", sizeof p->git.branch);
    g_free(branch);
  }
  g_free(txt);
  g_free(head);
}

/* What a shell of project p is told about it: the project's name, its accent as
 * a hex the prompt can colour with, and its branch. Given to the grid, which is
 * what actually builds the panes — leaves become terminals inside it, with no
 * project context of their own. Rebuilt at every point that can add a pane, so
 * a split hours later still sees the current theme's accent and the branch as
 * it stands now. */
static void set_spawn_env_for(PtProjectUI *p, PtPaneGrid *g) {
  PtWindow *w = p->window;
  int a = ((proj_accent(p) % PT_ACCENT_COUNT) + PT_ACCENT_COUNT)
          % PT_ACCENT_COUNT;
  char *proj = g_strdup_printf("PT_PROJECT=%s", proj_name(p));
  char *acc  = g_strdup_printf("PT_ACCENT=%s", w->accent_hex[a]);
  char *br   = g_strdup_printf("PT_BRANCH=%s", p->git.branch);
  const char *pairs[] = { proj, acc, br, NULL };
  pt_pane_grid_set_env(g, pairs);
  g_free(proj); g_free(acc); g_free(br);
}

/* The two per-pane config values as they stand now — what a pane built from
 * here on starts out with. The terminal widget remembers neither between panes:
 * the grid that builds a pane is what carries them to it, so that arming a new
 * pane cannot drag a pane the user toggled by hand back into line with the file.
 * That is the config apply's job, and it re-arms every live pane itself.
 * The NULL config is a window mid-teardown; the compiled-in defaults are then
 * as good an answer as any. */
static gboolean pane_mouse_reporting(PtWindow *w) {
  return w->config != NULL ? w->config->mouse_reporting
                           : PT_CONFIG_MOUSE_REPORTING_DEFAULT;
}

static PtOsc52Mode pane_osc52(PtWindow *w) {
  return w->config != NULL ? w->config->osc52 : PT_CONFIG_OSC52_DEFAULT;
}

static PtTabUI *tab_ui_new(PtWindow *w, PtProjectUI *p, const char *title,
                           PtSplitNode *tree) {
  PtTabUI *t = g_new0(PtTabUI, 1);
  t->window = w;
  t->title = g_strdup(title);
  t->grid = pt_pane_grid_new(tree, pane_mouse_reporting(w), pane_osc52(w));
  g_object_ref_sink(t->grid);
  /* The grid built its first panes above, so this is a back-fill — safe, and
   * still ahead of every spawn: the grid is not parented yet, and a pane only
   * spawns when it allocates. */
  set_spawn_env_for(p, PT_PANE_GRID(t->grid));
  g_object_set_data(G_OBJECT(t->grid), PT_GRID_TAB_KEY, t);
  g_signal_connect(t->grid, "structure-changed",
                   G_CALLBACK(on_grid_structure), w);
  g_signal_connect(t->grid, "focus-changed", G_CALLBACK(on_grid_focus), w);
  g_signal_connect(t->grid, "command-changed", G_CALLBACK(on_grid_command), w);
  g_signal_connect(t->grid, "title-changed", G_CALLBACK(on_grid_title), w);
  g_signal_connect(t->grid, "emptied", G_CALLBACK(on_grid_emptied), w);
  g_signal_connect(t->grid, "notification",
                   G_CALLBACK(on_grid_notification), w);
  return t;
}

static void tab_ui_free(gpointer data) {
  PtTabUI *t = data;
  g_free(t->title);
  if (t->grid != NULL) {
    /* The six handlers above all carry the window, and the grid can outlive
     * this tab (pt-pane-grid's close idle holds its own ref). Drop them here
     * or a background shell exiting in the same frame as window close would
     * emit "emptied" into a finalized window. */
    g_signal_handlers_disconnect_by_data(t->grid, t->window);
    g_object_set_data(G_OBJECT(t->grid), PT_GRID_TAB_KEY, NULL);
    if (gtk_widget_get_parent(t->grid) != NULL)
      gtk_widget_unparent(t->grid);
    g_object_unref(t->grid);
  }
  g_free(t);
}

/* The one way a tab comes into being: the widget struct, its workspace id, and
 * the data slot binding them, in one place. Appends — the caller decides
 * whether the new tab is also selected. */
static PtTabUI *add_tab_ui(PtWindow *w, PtProjectUI *p, const char *title,
                           PtSplitNode *tree) {
  PtTabUI *t = tab_ui_new(w, p, title, tree);
  t->id = pt_workspace_add_tab(w->ws, p->id);
  pt_workspace_set_data(w->ws, t->id, t);
  return t;
}

static void on_git_update(const PtGitStatus *st, GPtrArray *files,
                          gboolean is_repo, gpointer user) {
  /* user is the PtProjectUI; find its window via stored back-pointer */
  PtProjectUI *p = user;
  p->git = *st;
  p->is_repo = is_repo;
  /* `files` transfers from the monitor: the project keeps the array itself,
   * and everything downstream (sidebar, project bar, palette, info panel)
   * reads from here — no copies anywhere on the path. */
  g_clear_pointer(&p->git_files, g_ptr_array_unref);
  p->git_files = files;
  /* No refresh_statusline here: the status bar stopped speaking for git in the
   * rebuild (that moved to the project bar), and scraping the terminal grid on
   * every git poll would be pure waste. */
  queue_refresh_sidebar(p->window);
  refresh_infopanel(p->window);
}

/* Everything a project is before it has tabs: its workspace entry (identity,
 * accent, order — `accent` < 0 takes the next colour in the cycle), the UI
 * struct in its data slot, and — for a project whose directory is actually
 * there — the branch seed its first shells' PT_BRANCH comes from. Tabs stay
 * the caller's business because that is the one place the two entry points
 * differ: a restored project brings its own from the session, a freshly added
 * one gets a single shell. */
static PtProjectUI *project_ui_alloc(PtWindow *w, const char *name,
                                     const char *path, int accent) {
  PtProjectUI *p = g_new0(PtProjectUI, 1);
  p->window = w;
  p->id = pt_workspace_add_project(w->ws, name, path, accent);
  pt_workspace_set_data(w->ws, p->id, p);
  p->missing = !g_file_test(path, G_FILE_TEST_IS_DIR);
  /* Before the caller builds any tab: PT_BRANCH is part of the env every one of
   * them hands its shells, and the git monitor's first poll lands far later. */
  if (!p->missing)
    seed_git_branch(p);   /* the monitor has not polled yet; read HEAD directly */
  return p;
}

/* A project the user just added (or one restored with no tabs saved): one
 * shell, git monitor running. `accent` < 0 = next in the cycle. */
static PtProjectUI *project_ui_new(PtWindow *w, const char *name,
                                   const char *path, int accent) {
  PtProjectUI *p = project_ui_alloc(w, name, path, accent);
  if (!p->missing) {
    add_tab_ui(w, p, "shell", pt_split_leaf_new(path));
    p->monitor = pt_git_monitor_new(path, on_git_update, p);
  }
  return p;
}

static void project_ui_free(gpointer data) {
  PtProjectUI *p = data;
  pt_git_monitor_free(p->monitor);
  g_clear_pointer(&p->git_files, g_ptr_array_unref);
  g_free(p);
}

/* Drop a project from the model, then free the UI hung off it — strictly in
 * that order: grid teardown can fire grid signals, and a handler running
 * mid-teardown must find dead ids (a defined no-op), never live ids whose data
 * slots point at freed memory. Successor selection is the removal's job in the
 * model; refreshes stay with the caller (dispose wants none). */
static void remove_project_ui(PtWorkspace *ws, PtWsId project) {
  guint tabs = pt_workspace_tab_count(ws, project);
  GPtrArray *dead = g_ptr_array_new();
  for (guint i = 0; i < tabs; i++)
    g_ptr_array_add(
        dead, pt_workspace_get_data(ws, pt_workspace_tab_at(ws, project, i)));
  PtProjectUI *p = pt_workspace_get_data(ws, project);
  pt_workspace_remove_project(ws, project);
  for (guint i = 0; i < dead->len; i++)
    tab_ui_free(g_ptr_array_index(dead, i));
  project_ui_free(p);
  g_ptr_array_free(dead, TRUE);
}

/* Poll cost follows attention: the active project's monitor polls at 5s, the
 * rest back off to 60s, and the numstat subprocess only runs for the project
 * on screen while the info panel is. Called whenever the active project or
 * the panel's visibility moves; the setters no-op when nothing changed. */
static void sync_git_monitors(PtWindow *w) {
  gboolean panel = w->infopanel != NULL &&
                   gtk_widget_get_visible(w->infopanel);
  PtWsId active = pt_workspace_active_project(w->ws);
  for (guint i = 0; i < pt_workspace_project_count(w->ws); i++) {
    PtWsId id = pt_workspace_project_at(w->ws, i);
    PtProjectUI *p = pt_workspace_get_data(w->ws, id);
    if (p->monitor == NULL) continue;
    gboolean is_active = id == active;
    /* One refresh even when both settings move together — the setters only
     * say a poll is due, they never spawn one themselves. */
    gboolean due =
        pt_git_monitor_set_want_line_counts(p->monitor, panel && is_active);
    if (pt_git_monitor_set_active(p->monitor, is_active)) due = TRUE;
    if (due) pt_git_monitor_refresh(p->monitor);
  }
}

/* ---------- actions ---------- */
/* Ids inside, indices only at the seams: the id-taking cores are what the
 * window acts through, and the index wrappers translate exactly where an index
 * is the caller's native language (shortcut table, sidebar and tab-strip
 * signals, the palette). A dead id no-ops. */
/* Zoom is per-grid view state, and walking away from a tab puts its grid
 * back: coming back later must not land on a half-forgotten zoomed view the
 * user has long stopped expecting. */
static void reset_zoom_before_switch(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  if (t != NULL) pt_pane_grid_unzoom(PT_PANE_GRID(t->grid));
}

static void switch_project_id(PtWindow *w, PtWsId project) {
  if (w->ws == NULL ||
      pt_workspace_project_index(w->ws, project) == PT_WS_INDEX_NONE)
    return;
  reset_zoom_before_switch(w);
  pt_workspace_set_active_project(w->ws, project);
  sync_git_monitors(w);
  refresh_sidebar(w);
  show_active_grid(w);
  refresh_infopanel(w);
  mark_dirty(w);
}

static void action_switch_project(PtWindow *w, int idx) {
  if (w->ws == NULL || idx < 0) return;
  PtWsId id = pt_workspace_project_at(w->ws, (guint)idx);
  if (id != PT_WS_ID_NONE) switch_project_id(w, id);
}

/* Cycle the active project. delta is ±1; wraps. Goes through switch_project_id
 * so a step lands on exactly the state a ⌃1…9 jump would. */
static void step_project(PtWindow *w, int delta) {
  if (w->ws == NULL) return;
  guint len = pt_workspace_project_count(w->ws);
  if (len <= 1) return;
  guint cur = pt_workspace_project_index(w->ws,
                                         pt_workspace_active_project(w->ws));
  if (cur == PT_WS_INDEX_NONE) return;
  guint next = (cur + (guint)((int)len + delta)) % len;
  switch_project_id(w, pt_workspace_project_at(w->ws, next));
}

static void action_next_project(PtWindow *w) { step_project(w, +1); }

static void action_prev_project(PtWindow *w) { step_project(w, -1); }

static void switch_tab_id(PtWindow *w, PtWsId tab) {
  if (w->ws == NULL ||
      pt_workspace_tab_project(w->ws, tab) == PT_WS_ID_NONE)
    return;
  reset_zoom_before_switch(w);
  pt_workspace_set_active_tab(w->ws, tab);
  show_active_grid(w);
  refresh_infopanel(w);
  mark_dirty(w);
}

static void action_switch_tab(PtWindow *w, int idx) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || idx < 0) return;
  PtWsId tab = pt_workspace_tab_at(w->ws, p->id, (guint)idx);
  if (tab != PT_WS_ID_NONE) switch_tab_id(w, tab);
}

/* Cycle within the active project. delta is ±1; the workspace guarantees the
 * active tab is live whenever the project has any, so the index math is safe.
 * Goes through switch_tab_id so a step lands on exactly the state an ⌥1…9 jump
 * would — the info panel included. */
static void step_tab(PtWindow *w, int delta) {
  PtProjectUI *p = active_project(w);
  if (p == NULL) return;
  guint len = pt_workspace_tab_count(w->ws, p->id);
  if (len <= 1) return;
  guint cur = pt_workspace_tab_index(
      w->ws, pt_workspace_active_tab(w->ws, p->id));
  guint next = (cur + (guint)((int)len + delta)) % len;
  switch_tab_id(w, pt_workspace_tab_at(w->ws, p->id, next));
}

static void action_next_tab(PtWindow *w) { step_tab(w, +1); }

static void action_prev_tab(PtWindow *w) { step_tab(w, -1); }

static void action_new_tab(PtWindow *w) {
  PtProjectUI *p = active_project(w);
  if (p == NULL || p->missing) return;
  PtTabUI *t = add_tab_ui(w, p, "shell", pt_split_leaf_new(proj_path(p)));
  pt_workspace_set_active_tab(w->ws, t->id);
  show_active_grid(w);
  /* the row's shell count just moved; without this it waits for the comm poll */
  refresh_sidebar(w);
  mark_dirty(w);
}

/* ---------- desktop notifications (OSC 9 / OSC 777) ----------
 *
 * A build finishes in a pane on another workspace and nothing tells you. The
 * core has already decided which sequences deserve a notification, dropped the
 * ones from a pane the user is looking at, capped the text and paid the rate
 * limit, so what is left here is the desktop half: raise it, and make clicking
 * it land on the pane that sent it. */

/* Bring the pane with this id to the front: its project, its tab, its pane,
 * and the window itself. FALSE when no pane has that id, which is what a
 * notification clicked after its pane was closed looks like. */
static gboolean activate_pane(PtWindow *w, guint64 pane_id) {
  if (w->ws == NULL) return FALSE;
  /* Pane ids are per-process, not per-grid, so the only way to the right grid
   * is to ask each one; the tab that answers is already the one to switch to —
   * no second lookup. */
  for (guint pi = 0; pi < pt_workspace_project_count(w->ws); pi++) {
    PtWsId proj = pt_workspace_project_at(w->ws, pi);
    guint tabs = pt_workspace_tab_count(w->ws, proj);
    for (guint ti = 0; ti < tabs; ti++) {
      PtTabUI *t = pt_workspace_get_data(
          w->ws, pt_workspace_tab_at(w->ws, proj, ti));
      PtPaneGrid *grid = PT_PANE_GRID(t->grid);
      if (pt_pane_grid_pane_by_id(grid, pane_id) == NULL) continue;
      /* Focus the pane inside its grid first: the switches below end in
       * show_active_grid, which re-focuses the grid's remembered pane — which
       * this call has just made the right one. */
      pt_pane_grid_focus_pane_by_id(grid, pane_id);
      switch_project_id(w, proj);
      switch_tab_id(w, t->id);
      gtk_window_present(GTK_WINDOW(w));
      return TRUE;
    }
  }
  return FALSE;
}

/* Pane ids are handed out by a counter that starts over at 1 every launch, and
 * a notification can outlive the process that sent it: notification daemons
 * keep what they are showing, and pt does not withdraw its own on the way out.
 * So a click on yesterday's notification could name a pane id that today's
 * process has since handed to something else, and land the user in an
 * unrelated shell — worse than doing nothing.
 *
 * The target carries this alongside the pane id, and a target from any other
 * process is dropped on sight. Random rather than a pid, which the kernel
 * reuses within a session. */
static guint64 session_nonce(void) {
  static guint64 nonce;
  if (nonce == 0)
    nonce = ((guint64)g_random_int() << 32) | g_random_int() | 1;
  return nonce;
}

/* The notification's default action, i.e. what clicking the body does. It
 * lives on the application rather than the window because that is where the
 * desktop can reach it: the shell activates `app.activate-pane` by name, and
 * the process it reaches need not be the one that sent the notification. The
 * window is found through the application for the same reason — nothing here
 * holds a window pointer that could outlive the window. */
static void on_activate_pane_action(GSimpleAction *action, GVariant *param,
                                    gpointer user) {
  (void)action;
  GtkApplication *app = user;
  if (param == NULL) return;
  guint64 nonce = 0, pane_id = 0;
  g_variant_get(param, "(tt)", &nonce, &pane_id);
  if (nonce != session_nonce()) return;    /* an earlier process's pane */
  for (GList *l = gtk_application_get_windows(app); l != NULL; l = l->next) {
    if (!PT_IS_WINDOW(l->data)) continue;
    if (activate_pane(PT_WINDOW(l->data), pane_id)) return;
  }
}

static void on_grid_notification(PtPaneGrid *g, guint64 pane_id,
                                 const char *title, const char *body,
                                 gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w));
  if (app == NULL) return;

  /* OSC 9 carries no title at all, so most notifications arrive nameless.
   * Ghostty falls back to the flat application name; pt can do better, because
   * the pane knows what is running in it — "cargo" over "build finished" says
   * which of four builds this was.
   *
   * Asked of the sending pane and not of its tab: a tab's title follows its
   * *focused* pane (on_grid_command only forwards for that one), and the pane
   * that raises a notification is by definition not the focused one, so the
   * tab title would name a sibling. Down to the app name only when the pane
   * has no command yet either. */
  const char *shown = title;
  if (shown == NULL || shown[0] == '\0') {
    PtTerminal *sender = pt_pane_grid_pane_by_id(g, pane_id);
    const char *cmd = sender != NULL ? pt_terminal_last_command(sender) : NULL;
    if (cmd != NULL && cmd[0] != '\0') shown = cmd;
  }
  if (shown == NULL || shown[0] == '\0') shown = "pt";

  GNotification *n = g_notification_new(shown);
  g_notification_set_body(n, body);
  GIcon *icon = g_themed_icon_new("dev.hdprajwal.pt");
  g_notification_set_icon(n, icon);
  g_object_unref(icon);
  g_notification_set_default_action_and_target(n, "app.activate-pane", "(tt)",
                                               session_nonce(), pane_id);
  /* Keyed per pane, so a pane that notifies twice replaces its own earlier
   * notification instead of stacking a second one the user has to dismiss.
   * Ghostty keys on the body text instead (apprt/gtk/class/surface.zig), which
   * collapses two panes that finished with the same message into one — the
   * opposite trade, and the wrong one here, because pt's notification is
   * addressed to a particular pane. */
  char *id = g_strdup_printf("pane-%" G_GUINT64_FORMAT, pane_id);
  g_application_send_notification(G_APPLICATION(app), id, n);
  g_free(id);
  g_object_unref(n);
}

/* ---------- agent lifecycle reports ----------
 *
 * The hooks drop their report files into one state directory; watching that
 * directory is how pt learns a turn finished or an agent is waiting on input
 * while the user is somewhere else. Same desktop pipeline as the OSC
 * notifications above: the click lands on the pane that owns the report, and
 * the per-pane key replaces instead of stacking. */

static void notify_agent_event(PtWindow *w, const char *token) {
  char *path = pt_agent_session_report_path(token);
  PtAgentSessionReport *r = pt_agent_session_report_load(path);
  g_free(path);
  /* No report, or one without a lifecycle event (the common write is the
   * SessionStart registration): nothing to say. */
  if (r == NULL || r->event == PT_AGENT_EVENT_NONE) {
    pt_agent_session_report_free(r);
    return;
  }
  const char *evname = pt_agent_session_event_name(r->event);

  /* Which pane owns this token. Reports outlive panes — the file is how a
   * restored window finds its session back — so "no taker" is the normal
   * answer for a pane that closed, not an error. */
  PtTerminal *owner = NULL;
  PtPaneGrid *owner_grid = NULL;
  if (w->ws != NULL) {
    for (guint pi = 0; pi < pt_workspace_project_count(w->ws); pi++) {
      PtWsId proj = pt_workspace_project_at(w->ws, pi);
      guint tabs = pt_workspace_tab_count(w->ws, proj);
      for (guint ti = 0; ti < tabs && owner == NULL; ti++) {
        PtTabUI *t = pt_workspace_get_data(
            w->ws, pt_workspace_tab_at(w->ws, proj, ti));
        PtPaneGrid *grid = PT_PANE_GRID(t->grid);
        PtTerminal *term = pt_pane_grid_pane_by_token(grid, token);
        if (term != NULL) { owner = term; owner_grid = grid; }
      }
      if (owner != NULL) break;
    }
  }
  if (owner == NULL || owner_grid == NULL) {
    pt_agent_session_report_free(r);
    return;
  }

  /* Never for a pane the user is looking at — a focused pane in an active
   * window is exactly where they would see the agent finish anyway. */
  gboolean focused = gtk_window_is_active(GTK_WINDOW(w)) &&
                     owner == pt_pane_grid_focused_terminal(owner_grid);
  if (focused || !pt_agent_latch_should_notify(w->agent_notified, token,
                                               evname)) {
    pt_agent_session_report_free(r);
    return;
  }

  /* The latch records what was DELIVERED, so it is written only after this
   * check: an event latched before a bail here could never notify again. */
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w));
  if (app == NULL) {
    pt_agent_session_report_free(r);
    return;
  }

  const char *label = pt_agent_label(r->agent);
  char *title = g_strdup_printf(
      "%s %s", label != NULL ? label : "agent",
      r->event == PT_AGENT_EVENT_NEEDS_INPUT ? "needs your input"
                                             : "finished");
  /* The body says where: the cwd basename names the project without
   * spending a whole line of notification on a path. */
  char *body;
  if (r->cwd != NULL && r->cwd[0] != '\0') {
    char *base = g_path_get_basename(r->cwd);
    body = g_strdup_printf("in %s", base);
    g_free(base);
  } else {
    body = g_strdup("in this pane");
  }

  GNotification *n = g_notification_new(title);
  g_notification_set_body(n, body);
  GIcon *icon = g_themed_icon_new("dev.hdprajwal.pt");
  g_notification_set_icon(n, icon);
  g_object_unref(icon);
  g_notification_set_default_action_and_target(n, "app.activate-pane",
                                               "(tt)", session_nonce(),
                                               pt_terminal_id(owner));
  char *id = g_strdup_printf("agent-%s", token);
  g_application_send_notification(G_APPLICATION(app), id, n);
  g_free(id);
  g_object_unref(n);
  pt_agent_latch_record(w->agent_notified, token, evname);
  g_free(title);
  g_free(body);
  pt_agent_session_report_free(r);
}

static void on_agent_report_changed(GFileMonitor *m, GFile *file,
                                    GFile *other, GFileMonitorEvent ev,
                                    gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  /* The reports are written temp+rename, so WATCH_MOVES is what makes the
   * arrival visible as RENAMED with other_file naming what landed — without
   * it the rename shows up as a CREATED of the temp file and the final name
   * never surfaces. A plain (non-atomic) writer arrives as CHANGES_DONE_HINT
   * instead. Both carry the final name; only names ending in .json are
   * reports, which is also what filters the temp file's own events, and the
   * sweep's DELETED events are noise by definition. */
  GFile *final = NULL;
  if (ev == G_FILE_MONITOR_EVENT_RENAMED ||
      ev == G_FILE_MONITOR_EVENT_MOVED_IN)
    final = other;
  else if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    final = file;
  if (final == NULL) return;
  char *name = g_file_get_basename(final);
  if (!g_str_has_suffix(name, ".json")) {
    g_free(name);
    return;
  }
  char *token = g_strndup(name, strlen(name) - strlen(".json"));
  g_free(name);
  notify_agent_event(w, token);
  g_free(token);
}

static void watch_agent_reports(PtWindow *w) {
  w->agent_notified = pt_agent_latch_new();
  /* pt_agent_session_dir creates the directory, so the monitor has something
   * to watch even on a first launch that has never run an agent. */
  char *dir = pt_agent_session_dir();
  GFile *f = g_file_new_for_path(dir);
  g_free(dir);
  w->agent_report_monitor = g_file_monitor_directory(
      f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  g_object_unref(f);
  if (w->agent_report_monitor != NULL)
    g_signal_connect(w->agent_report_monitor, "changed",
                     G_CALLBACK(on_agent_report_changed), w);
}

/* Close the focused pane of grid g (not of whatever happens to be active now).
 * No-op when g is no longer owned by any live tab. */
static void do_close_pane(PtWindow *w, PtPaneGrid *g) {
  PtTabUI *t = find_grid_tab(w, g);
  if (t == NULL) return;
  if (!pt_pane_grid_close_focused(g)) {
    /* last pane closed → close the owning tab, whichever one it is */
    remove_tab(w, t);
    return;
  }
  /* The running count on the sidebar row just moved, so refresh rather than
   * leaving it stale until the next comm poll. */
  refresh_sidebar(w);
  mark_dirty(w);
}

/* Close the whole tab that owns grid g — every pane, not just the focused one.
 * No-op when g is no longer owned by any live tab (see find_grid_tab). */
static void do_close_tab(PtWindow *w, PtPaneGrid *g) {
  PtTabUI *t = find_grid_tab(w, g);
  if (t == NULL) return;
  remove_tab(w, t);
}

/* Response-callback payload: keeps the target grid alive (the tab holding it
 * can be dropped while the dialog is up) and the window resolvable. Freed
 * through the closure's GDestroyNotify, so it survives a dialog that is torn
 * down without ever emitting a response. The grid, not a tab pointer, is what
 * crosses the async gap: on response the grid resolves to its tab's workspace
 * id, and a tab that died meanwhile resolves to nothing — a no-op, never
 * whatever slid into its slot. */
typedef struct {
  PtWindow *window;    /* owned ref */
  PtPaneGrid *grid;    /* owned ref */
  gboolean whole_tab;  /* the tab's ×, not ⌃⇧W: take every pane */
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
    if (c->whole_tab) do_close_tab(c->window, c->grid);
    else              do_close_pane(c->window, c->grid);
    return;
  }
  /* Cancelled (or dismissed): hand the keyboard back to the pane we asked
   * about, if that grid is still on screen. A background tab's grid is
   * unparented, so check for a root before grabbing focus into nowhere. */
  if (find_grid_tab(c->window, c->grid) != NULL &&
      gtk_widget_get_root(GTK_WIDGET(c->grid)) != NULL)
    pt_pane_grid_focus_terminal(c->grid);
}

/* One dialog for both close paths; the ⌃⇧W wording is the pane one. */
static void present_close_confirm(PtWindow *w, PtPaneGrid *grid,
                                  gboolean whole_tab) {
  AdwDialog *dlg = adw_alert_dialog_new(
      whole_tab ? "Close tab?" : "Close shell?",
      whole_tab ? "A process is still running in this tab."
                : "A process is still running in this shell.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                 "cancel", "Cancel", "close", "Close", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "close",
                                           ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
  PtCloseCtx *c = g_new0(PtCloseCtx, 1);
  c->window = g_object_ref(w);
  c->grid = g_object_ref(grid);
  c->whole_tab = whole_tab;
  w->close_confirm_open = TRUE;
  g_signal_connect_data(dlg, "response", G_CALLBACK(on_close_pane_response), c,
                        close_ctx_free, 0);
  adw_dialog_present(dlg, GTK_WIDGET(w));
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
  present_close_confirm(w, grid, FALSE);
}

/* The tab's × takes the whole tab, so ask when ANY of its panes is mid-command,
 * not just the focused one. Shares the ⌃⇧W dialog and its re-entrancy guard. */
static void action_close_tab(PtWindow *w, int idx) {
  if (w->close_confirm_open) return;   /* never stack confirm dialogs */
  PtProjectUI *p = active_project(w);
  if (p == NULL || idx < 0) return;
  PtTabUI *t = pt_workspace_get_data(
      w->ws, pt_workspace_tab_at(w->ws, p->id, (guint)idx));
  if (t == NULL) return;
  PtPaneGrid *grid = PT_PANE_GRID(t->grid);
  if (!pt_pane_grid_any_running(grid)) {
    do_close_tab(w, grid);
    return;
  }
  present_close_confirm(w, grid, TRUE);
}

static void action_split(PtWindow *w, PtSplitKind kind) {
  PtProjectUI *p = active_project(w);
  PtTabUI *t = active_tab(p);
  if (t == NULL) return;
  /* Re-set rather than relied on: the accent may have followed a theme change,
   * the branch may have moved and the config may have been edited since this
   * grid's last pane. Both reach the pane the split is about to build, and
   * nothing else — the panes already in the grid are left as they are. */
  set_spawn_env_for(p, PT_PANE_GRID(t->grid));
  pt_pane_grid_set_pane_defaults(PT_PANE_GRID(t->grid),
                                 pane_mouse_reporting(w), pane_osc52(w));
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

/* ghostty's toggle_mouse_reporting, one pane at a time: swap who owns the
 * pointer without editing the config or restarting. No accelerator — it lives
 * in the palette, where the row also says which way it is pointing. */
static void action_toggle_mouse_reporting(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  PtTerminal *term =
      t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid)) : NULL;
  if (term != NULL) pt_terminal_toggle_mouse_reporting(term);
}

/* ghostty's `reset`, which is surface-scoped there and pane-scoped here. No
 * accelerator, matching ghostty, which ships the action with no default
 * binding — and no confirmation either, though it does throw the scrollback
 * away. */
static void action_reset_terminal(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  PtTerminal *term =
      t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid)) : NULL;
  if (term != NULL) pt_terminal_reset(term);
}

static gboolean active_mouse_reporting(PtWindow *w) {
  PtTabUI *t = active_tab(active_project(w));
  PtTerminal *term =
      t != NULL ? pt_pane_grid_focused_terminal(PT_PANE_GRID(t->grid)) : NULL;
  return term != NULL && pt_terminal_mouse_reporting(term);
}

/* ---------- project add/remove ---------- */
static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src),
                                                     res, NULL);
  if (file == NULL) return;
  char *path = g_file_get_path(file);
  char *name = g_path_get_basename(path);
  /* -1: take the next accent in the cycle, by the project's new position. */
  PtProjectUI *p = project_ui_new(w, name, path, -1);
  pt_workspace_set_active_project(w->ws, p->id);
  sync_git_monitors(w);
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
  if (idx < 0) return;
  PtWsId id = pt_workspace_project_at(w->ws, (guint)idx);
  if (id == PT_WS_ID_NONE) return;
  remove_project_ui(w->ws, id);
  sync_git_monitors(w);   /* whoever was picked as active polls fast */
  refresh_sidebar(w);
  show_active_grid(w);
  mark_dirty(w);
}

/* The sidebar renders the workspace order and does not own it, so a reorder is
 * one model move here plus a save. Active project follows automatically — it
 * is an id, not a position, so there is no index to patch up. */
static void on_project_moved(PtSidebar *sb, int from, int to, gpointer user) {
  (void)sb;
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  int n = (int)pt_workspace_project_count(w->ws);
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  pt_workspace_move_project(w->ws,
                            pt_workspace_project_at(w->ws, (guint)from),
                            (guint)to);
  refresh_sidebar(w);
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

static void action_toggle_sidebar(PtWindow *w) {
  gtk_widget_set_visible(w->sidebar, !gtk_widget_get_visible(w->sidebar));
  /* Hiding while the sidebar search holds focus would strand the keyboard. */
  focus_active_terminal(w);
}

/* Shared by ⌃I and the tab strip's panel button. */
static void action_toggle_infopanel(PtWindow *w) {
  gboolean show = !gtk_widget_get_visible(w->infopanel);
  gtk_widget_set_visible(w->infopanel, show);
  /* Line counts are only worth a subprocess while the panel can show them;
   * opening flips the gate on, which also refreshes so they arrive now. */
  sync_git_monitors(w);
  /* refresh_infopanel is a no-op while hidden, so without this the panel would
   * appear holding whatever it showed when it was last closed. */
  if (show) refresh_infopanel(w);
  /* Closing is the only moment the agent monitor can learn it is off screen —
   * everything else runs through refresh_infopanel, which is a no-op by then.
   * Its poll stops here. */
  else pt_agent_monitor_observe(w->agents, FALSE, 0, NULL, NULL);
  focus_active_terminal(w);
}

static void on_tab_selected(PtTabStrip *s, int idx, gpointer user) {
  (void)s;
  action_switch_tab(PT_WINDOW(user), idx);
}

static void on_tab_new(PtTabStrip *s, gpointer user) {
  (void)s;
  action_new_tab(PT_WINDOW(user));
}

static void on_tab_close(PtTabStrip *s, int idx, gpointer user) {
  (void)s;
  action_close_tab(PT_WINDOW(user), idx);
}

/* Same shape as on_project_moved one level down: the strip renders the
 * workspace order and does not own it, so a drop is one model move plus a save.
 * The active tab and ⌥⇥ both follow along on their own — one is an id, the
 * other walks the workspace order.
 *
 * Ids and not indices, resolved in the workspace call: the strip's rows are
 * frozen for the length of a drag, and the workspace does not hold still under
 * them — a shell can exit, and the still-live ⌥⇥ capture controller can switch
 * the active project. An index taken from the frozen rows would then move the
 * wrong tab, or the wrong project's; the ids keep naming what the user saw
 * (the move lands in the dragged tab's own project even if it is no longer the
 * active one), and a tab closed mid-drag resolves to a no-op. */
static void on_tab_moved(PtTabStrip *s, guint tab, guint dest, gboolean after,
                         gpointer user) {
  (void)s;
  PtWindow *w = PT_WINDOW(user);
  if (w->ws == NULL) return;
  if (!pt_workspace_move_tab_beside(w->ws, tab, dest, after)) return;
  refresh_tabstrip(w);
  mark_dirty(w);
}

static void on_toggle_panel(PtTabStrip *s, gpointer user) {
  (void)s;
  action_toggle_infopanel(PT_WINDOW(user));
}

/* Fire-and-forget: zed detaches into its own process and pt never waits on it,
 * so nothing here reports back beyond a failed spawn. */
static void spawn_editor(const char *path) {
  if (path == NULL || path[0] == '\0') return;
  char *argv[] = {"zed", (char *)path, NULL};
  GError *err = NULL;
  if (!g_spawn_async(NULL, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                         G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, &err)) {
    g_warning("pt: failed to launch zed: %s", err->message);
    g_clear_error(&err);
  }
}

static void on_open_editor(PtTabStrip *s, gpointer user) {
  (void)s;
  PtProjectUI *p = active_project(PT_WINDOW(user));
  if (p != NULL) spawn_editor(proj_path(p));
}

/* ---------- info panel actions ---------- */
/* All three act on the directory the panel is showing, not the project root:
 * that is the path under the buttons. */
static void on_info_open_editor(PtInfoPanel *ip, gpointer user) {
  (void)ip;
  char *dir = panel_dir(PT_WINDOW(user));
  spawn_editor(dir);
  g_free(dir);
}

static void on_info_open_files(PtInfoPanel *ip, gpointer user) {
  (void)ip;
  char *dir = panel_dir(PT_WINDOW(user));
  if (dir == NULL) return;
  char *uri = g_filename_to_uri(dir, NULL, NULL);
  GError *err = NULL;
  if (uri != NULL &&
      !g_app_info_launch_default_for_uri(uri, NULL, &err)) {
    g_warning("pt: failed to open the file manager: %s", err->message);
    g_clear_error(&err);
  }
  g_free(uri);
  g_free(dir);
}

static void on_info_copy_path(PtInfoPanel *ip, gpointer user) {
  (void)ip;
  PtWindow *w = PT_WINDOW(user);
  char *dir = panel_dir(w);
  if (dir == NULL) return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(w)), dir);
  g_free(dir);
}

static void on_info_refresh(PtInfoPanel *ip, gpointer user) {
  (void)ip;
  PtWindow *w = PT_WINDOW(user);
  PtProjectUI *p = active_project(w);
  /* The git side lands asynchronously; everything else is up to date on the
   * spot. */
  if (p != NULL && p->monitor != NULL) pt_git_monitor_refresh(p->monitor);
  /* Same button for the usage block's Retry. It skips the poll interval but
   * not an active rate limit — that gate lives inside the monitor precisely
   * so a button cannot step around it. */
  pt_agent_monitor_refresh(w->agents);
  refresh_infopanel(w);
}

/* The user turning Claude usage on. Written to the config as well as pushed
 * into the monitor: permission to put their token on the wire is a standing
 * answer, not a per-session one, and the config file is where they can take
 * it back without hunting for a button.
 *
 * Written now, not on the usual debounce. That debounce exists for settings
 * that arrive in a stream — a font-size drag — and dispose drops it rather
 * than flushing it, so closing the window within the second would silently
 * discard this. Everything else it could discard would only be re-done; this
 * one is the user's answer to a question about their credentials. */
static void on_info_usage_enable(PtInfoPanel *ip, gpointer user) {
  (void)ip;
  PtWindow *w = PT_WINDOW(user);
  if (w->config == NULL || w->config->claude_usage) return;
  w->config->claude_usage = TRUE;
  pt_agent_monitor_set_claude_enabled(w->agents, TRUE);
  if (w->config_save_source != 0) {
    g_source_remove(w->config_save_source);
    w->config_save_source = 0;
  }
  config_save_now(w);
}

/* ---------- command palette ---------- */
/* Commands ride the same list as projects and shells, marked is_command with
 * `command` saying which one. */
enum { PT_CMD_TOGGLE_MOUSE_REPORTING, PT_CMD_RESET_TERMINAL };

/* Every project, each followed by its shells, then the commands. The palette
 * ranks this flat list and hands back the workspace ids the user picked —
 * ids, not positions, because the palette sits open across an async gap
 * (see PtCommandPaletteItem). */
static void action_open_palette(PtWindow *w) {
  GArray *arr = g_array_new(FALSE, TRUE, sizeof(PtCommandPaletteItem));
  for (guint i = 0; i < pt_workspace_project_count(w->ws); i++) {
    PtWsId id = pt_workspace_project_at(w->ws, i);
    PtProjectUI *p = pt_workspace_get_data(w->ws, id);
    const char *name = pt_workspace_project_name(w->ws, id);
    int accent = pt_workspace_project_accent(w->ws, id);
    /* Same spelling as the project bar: home-abbreviated path, plain branch
     * text. The ⑂ glyph is reserved for the terminal's own identity line. */
    char shown_path[512];
    pt_path_home_abbrev(pt_workspace_project_path(w->ws, id),
                        g_get_home_dir(), shown_path, sizeof shown_path);
    PtCommandPaletteItem it = {
      .name = g_strdup(name),
      .detail = p->is_repo
          ? g_strdup_printf("%s · %s", shown_path, p->git.branch)
          : g_strdup(shown_path),
      .shortcut = i < 9 ? g_strdup_printf("^%u", i + 1) : NULL,
      .accent = accent, .is_shell = FALSE,
      .project_id = id, .tab_id = PT_WS_ID_NONE, .command = -1,
    };
    g_array_append_val(arr, it);
    for (guint j = 0; j < pt_workspace_tab_count(w->ws, id); j++) {
      PtWsId tab = pt_workspace_tab_at(w->ws, id, j);
      PtTabUI *t = pt_workspace_get_data(w->ws, tab);
      PtCommandPaletteItem sh = {
        .name = g_strdup(t->title),
        .detail = g_strdup(name),
        .shortcut = NULL, .accent = accent, .is_shell = TRUE,
        .project_id = id, .tab_id = tab, .command = -1,
      };
      g_array_append_val(arr, sh);
    }
  }
  /* The row says which way it is currently pointing, so the toggle is not a
   * coin flip. */
  PtCommandPaletteItem mr = {
    .name = g_strdup("Toggle mouse reporting"),
    .detail = g_strdup(active_mouse_reporting(w)
        ? "on · apps own the mouse, shift+drag selects"
        : "off · click and drag selects"),
    .shortcut = NULL, .accent = 0, .is_shell = FALSE, .is_command = TRUE,
    .project_id = PT_WS_ID_NONE, .tab_id = PT_WS_ID_NONE,
    .command = PT_CMD_TOGGLE_MOUSE_REPORTING,
  };
  g_array_append_val(arr, mr);

  /* The detail spells out what is lost and what is not: the row is one Enter
   * away from discarding the scrollback, and nothing asks again afterwards. */
  PtCommandPaletteItem rst = {
    .name = g_strdup("Reset terminal"),
    .detail = g_strdup("clears the screen, scrollback and modes · "
                       "the shell keeps running"),
    .shortcut = NULL, .accent = 0, .is_shell = FALSE, .is_command = TRUE,
    .project_id = PT_WS_ID_NONE, .tab_id = PT_WS_ID_NONE,
    .command = PT_CMD_RESET_TERMINAL,
  };
  g_array_append_val(arr, rst);

  int n = (int)arr->len;
  pt_command_palette_open(PT_COMMAND_PALETTE(w->palette),
                  (PtCommandPaletteItem *)g_array_free(arr, FALSE), n);
}

/* Ids resolve at activation time: a project or tab that died while the
 * palette was open (a background shell exiting takes its tab with it) is a
 * dead id and a no-op — never the row that slid into its old position. A live
 * project whose picked tab died still switches to the project, landing on its
 * current active tab. */
static void on_palette_activated(PtCommandPalette *pal, guint project_id,
                                 guint tab_id, int command, gpointer user) {
  (void)pal;
  PtWindow *w = PT_WINDOW(user);
  if (command >= 0) {
    switch (command) {
    case PT_CMD_TOGGLE_MOUSE_REPORTING: action_toggle_mouse_reporting(w); break;
    case PT_CMD_RESET_TERMINAL: action_reset_terminal(w); break;
    default: break;
    }
    return;
  }
  switch_project_id(w, project_id);
  if (tab_id != PT_WS_ID_NONE) switch_tab_id(w, tab_id);
}

static void on_palette_closed(PtCommandPalette *pal, gpointer user) {
  (void)pal;
  focus_active_terminal(PT_WINDOW(user));
}

/* ---------- settings ---------- */
/* Live preview: render the dialog's candidate and nothing else. w->config is
 * never assigned here, so the debounced save (which only ever writes
 * w->config) cannot pick the candidate up, and a cancel costs one re-render
 * and no file write at all. The candidate stays owned by the dialog. */
static void on_settings_changed(PtSettings *s, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->config == NULL) return;   /* disposed window */
  const PtConfig *cand = pt_settings_config(s);
  if (cand != NULL) render_config(w, cand);
}

/* Enter: the candidate becomes the config, and only now does it hit disk. */
static void on_settings_committed(PtSettings *s, gpointer user) {
  PtWindow *w = PT_WINDOW(user);
  if (w->config == NULL) return;
  const PtConfig *cand = pt_settings_config(s);
  if (cand == NULL) return;
  pt_config_free(w->config);
  w->config = pt_config_copy(cand);
  apply_config(w);
  config_save_soon(w);
}

/* Esc/scrim: w->config was never touched during the preview, so re-applying it
 * is the whole undo. */
static void on_settings_reverted(PtSettings *s, gpointer user) {
  (void)s;
  PtWindow *w = PT_WINDOW(user);
  if (w->config == NULL) return;
  apply_config(w);
}

static void on_settings_closed(PtSettings *s, gpointer user) {
  (void)s;
  focus_active_terminal(PT_WINDOW(user));
}

static void action_open_settings(PtWindow *w) {
  char *tdir = pt_theme_dir();
  /* The themes dir is created lazily: without it the GFileMonitor watches
   * nothing and a user's first theme file goes unnoticed. */
  g_mkdir_with_parents(tdir, 0700);
  char **names = pt_theme_list_names(tdir);
  pt_settings_open(PT_SETTINGS(w->settings), w->config,
                   (const char *const *)names);
  g_strfreev(names);
  g_free(tdir);
}

/* Zoom owns font-size: it pushes the new value into the terminals, into the
 * session (mark_dirty) and into the config file. delta steps the size (+1/-1);
 * delta 0 resets to the default. Clamping lives in pt_terminal_set_font_size,
 * and w->config is read back from the terminal afterwards so all three stay in
 * step. NULL config = disposed window, same guard convention as
 * active_project(). */
static void action_zoom(PtWindow *w, int delta) {
  if (w->config == NULL) return;
  pt_terminal_set_font_size(delta == 0 ? PT_CONFIG_FONT_SIZE_DEFAULT
                                       : pt_terminal_font_size() + delta);
  mark_dirty(w);
  w->config->font_size = pt_terminal_font_size();
  config_save_soon(w);
}

/* ---------- shortcuts ---------- */

/* The shortcut controller below runs in the CAPTURE phase on the window, so it
 * sees every accelerator before an overlay's own key controller does. Letting
 * one through while an overlay is up would strand it: almost all of these
 * actions end in pt_pane_grid_focus_terminal, focus would land on a terminal in
 * a sibling subtree, the overlay would drop out of the key propagation path
 * entirely, and it would sit there visible with Escape dead. So while either
 * overlay is open every accelerator reports "not handled" and falls through to
 * it — each overlay's own toggle (⌃K, ⌃,) still acts. */
static gboolean overlay_open(PtWindow *w) {
  return (w->palette != NULL && pt_command_palette_is_open(PT_COMMAND_PALETTE(w->palette))) ||
         (w->settings != NULL && pt_settings_is_open(PT_SETTINGS(w->settings)));
}

/* Two accelerators stay live, each a toggle for its own overlay. Both test
 * their own widget first — otherwise overlay_open(), which covers the other
 * overlay too, would eat the toggle — and only then defer to whatever else is
 * up. */
static gboolean sc_palette(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  PtWindow *w = PT_WINDOW(u);
  if (w->palette != NULL && pt_command_palette_is_open(PT_COMMAND_PALETTE(w->palette))) {
    pt_command_palette_close(PT_COMMAND_PALETTE(w->palette));
    return TRUE;
  }
  if (overlay_open(w)) return FALSE;   /* settings dialog is up */
  action_open_palette(w);
  return TRUE;
}
static gboolean sc_settings(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  PtWindow *w = PT_WINDOW(u);
  if (w->settings != NULL && pt_settings_is_open(PT_SETTINGS(w->settings))) {
    pt_settings_close(PT_SETTINGS(w->settings));
    /* pt_settings_close() emits "closed" only, so the live preview would stay
     * on screen with nothing behind it. A ⌃, dismissal is a cancel, exactly
     * like Escape, so put the real config back by hand. */
    if (w->config != NULL) apply_config(w);
    return TRUE;
  }
  if (overlay_open(w)) return FALSE;   /* palette is up */
  action_open_settings(w);
  return TRUE;
}
/* Every other accelerator is one row in the table below and goes through one
 * callback: the blocking rule is the same for all of them, so it is written
 * once, and the row says which action to run and with what argument. */
typedef enum {
  PT_ACTION_SWITCH_PROJECT,
  PT_ACTION_SWITCH_TAB,
  PT_ACTION_NEW_TAB,
  PT_ACTION_ADD_PROJECT,
  PT_ACTION_TOGGLE_SIDEBAR,
  PT_ACTION_TOGGLE_INFOPANEL,
  PT_ACTION_NEXT_TAB,
  PT_ACTION_PREV_TAB,
  PT_ACTION_NEXT_PROJECT,
  PT_ACTION_PREV_PROJECT,
  PT_ACTION_SPLIT,
  PT_ACTION_CLOSE_PANE,
  PT_ACTION_FOCUS_NEXT,
  PT_ACTION_FOCUS_PREV,
  PT_ACTION_FOCUS_DIRECTION,
  PT_ACTION_PASTE,
  PT_ACTION_COPY,
  PT_ACTION_ZOOM,
} PtActionId;

/* accel spells the trigger; arg is the action's argument: project/tab index,
 * PtSplitKind, PtPaneDirection, zoom delta. The Tab chords are not here — see
 * on_tab_chord for why they cannot be. */
static const struct {
  const char *accel;
  PtActionId id;
  int arg;
} shortcuts[] = {
  { .accel = "<Control>1", .id = PT_ACTION_SWITCH_PROJECT, .arg = 0 },
  { .accel = "<Control>2", .id = PT_ACTION_SWITCH_PROJECT, .arg = 1 },
  { .accel = "<Control>3", .id = PT_ACTION_SWITCH_PROJECT, .arg = 2 },
  { .accel = "<Control>4", .id = PT_ACTION_SWITCH_PROJECT, .arg = 3 },
  { .accel = "<Control>5", .id = PT_ACTION_SWITCH_PROJECT, .arg = 4 },
  { .accel = "<Control>6", .id = PT_ACTION_SWITCH_PROJECT, .arg = 5 },
  { .accel = "<Control>7", .id = PT_ACTION_SWITCH_PROJECT, .arg = 6 },
  { .accel = "<Control>8", .id = PT_ACTION_SWITCH_PROJECT, .arg = 7 },
  { .accel = "<Control>9", .id = PT_ACTION_SWITCH_PROJECT, .arg = 8 },
  { .accel = "<Alt>1", .id = PT_ACTION_SWITCH_TAB, .arg = 0 },
  { .accel = "<Alt>2", .id = PT_ACTION_SWITCH_TAB, .arg = 1 },
  { .accel = "<Alt>3", .id = PT_ACTION_SWITCH_TAB, .arg = 2 },
  { .accel = "<Alt>4", .id = PT_ACTION_SWITCH_TAB, .arg = 3 },
  { .accel = "<Alt>5", .id = PT_ACTION_SWITCH_TAB, .arg = 4 },
  { .accel = "<Alt>6", .id = PT_ACTION_SWITCH_TAB, .arg = 5 },
  { .accel = "<Alt>7", .id = PT_ACTION_SWITCH_TAB, .arg = 6 },
  { .accel = "<Alt>8", .id = PT_ACTION_SWITCH_TAB, .arg = 7 },
  { .accel = "<Alt>9", .id = PT_ACTION_SWITCH_TAB, .arg = 8 },
  { .accel = "<Control>n", .id = PT_ACTION_ADD_PROJECT },
  { .accel = "<Control>b", .id = PT_ACTION_TOGGLE_SIDEBAR },
  /* ⌃I costs the terminal its Ctrl+I (which a shell reads as Tab): this
   * window-level controller runs in the CAPTURE phase and takes it first. That
   * is the requested binding. */
  { .accel = "<Control>i", .id = PT_ACTION_TOGGLE_INFOPANEL },
  { .accel = "<Control>t", .id = PT_ACTION_NEW_TAB },
  { .accel = "<Control><Shift>t", .id = PT_ACTION_NEW_TAB },
  /* ⌃PgDn / ⌃PgUp cycle tabs too: no compositor claims those, so shells stay
   * reachable as a "next" on desktops that own ⌥⇥. */
  { .accel = "<Control>Page_Down", .id = PT_ACTION_NEXT_TAB },
  { .accel = "<Control>Page_Up", .id = PT_ACTION_PREV_TAB },
  { .accel = "<Control><Shift>d", .id = PT_ACTION_SPLIT, .arg = PT_SPLIT_H },
  { .accel = "<Control><Shift>s", .id = PT_ACTION_SPLIT, .arg = PT_SPLIT_V },
  { .accel = "<Control><Shift>w", .id = PT_ACTION_CLOSE_PANE },
  { .accel = "<Control><Shift>o", .id = PT_ACTION_FOCUS_NEXT },
  /* Ghostty parity: Ctrl+Super+] / [ cycle next / previous pane. */
  { .accel = "<Control><Super>bracketright", .id = PT_ACTION_FOCUS_NEXT },
  { .accel = "<Control><Super>bracketleft", .id = PT_ACTION_FOCUS_PREV },
  { .accel = "<Control><Alt>Left", .id = PT_ACTION_FOCUS_DIRECTION,
    .arg = PT_PANE_DIR_LEFT },
  { .accel = "<Control><Alt>Right", .id = PT_ACTION_FOCUS_DIRECTION,
    .arg = PT_PANE_DIR_RIGHT },
  { .accel = "<Control><Alt>Up", .id = PT_ACTION_FOCUS_DIRECTION,
    .arg = PT_PANE_DIR_UP },
  { .accel = "<Control><Alt>Down", .id = PT_ACTION_FOCUS_DIRECTION,
    .arg = PT_PANE_DIR_DOWN },
  { .accel = "<Control><Shift>v", .id = PT_ACTION_PASTE },
  { .accel = "<Control><Shift>c", .id = PT_ACTION_COPY },
  /* Font zoom: cover =, shifted + (both plain and explicit-shift forms),
   * and the keypad. */
  { .accel = "<Control>equal", .id = PT_ACTION_ZOOM, .arg = +1 },
  { .accel = "<Control>plus", .id = PT_ACTION_ZOOM, .arg = +1 },
  { .accel = "<Control><Shift>plus", .id = PT_ACTION_ZOOM, .arg = +1 },
  { .accel = "<Control>KP_Add", .id = PT_ACTION_ZOOM, .arg = +1 },
  { .accel = "<Control>minus", .id = PT_ACTION_ZOOM, .arg = -1 },
  { .accel = "<Control>underscore", .id = PT_ACTION_ZOOM, .arg = -1 },
  { .accel = "<Control>KP_Subtract", .id = PT_ACTION_ZOOM, .arg = -1 },
  { .accel = "<Control>0", .id = PT_ACTION_ZOOM, .arg = 0 },
};

/* What a table row's callback carries: the window (the only per-instance part)
 * plus the row's action. One array of these per window, not one heap block per
 * accelerator. */
typedef struct { PtWindow *w; PtActionId id; int arg; } ShortcutCtx;

static gboolean shortcut_dispatch(GtkWidget *wg, GVariant *a, gpointer u) {
  (void)wg; (void)a;
  const ShortcutCtx *c = u;
  PtWindow *w = c->w;
  if (overlay_open(w)) return FALSE;
  switch (c->id) {
    case PT_ACTION_SWITCH_PROJECT:   action_switch_project(w, c->arg); break;
    case PT_ACTION_SWITCH_TAB:       action_switch_tab(w, c->arg); break;
    case PT_ACTION_NEW_TAB:          action_new_tab(w); break;
    case PT_ACTION_ADD_PROJECT:      action_add_project(w); break;
    case PT_ACTION_TOGGLE_SIDEBAR:   action_toggle_sidebar(w); break;
    case PT_ACTION_TOGGLE_INFOPANEL: action_toggle_infopanel(w); break;
    case PT_ACTION_NEXT_TAB:         action_next_tab(w); break;
    case PT_ACTION_PREV_TAB:         action_prev_tab(w); break;
    case PT_ACTION_NEXT_PROJECT:     action_next_project(w); break;
    case PT_ACTION_PREV_PROJECT:     action_prev_project(w); break;
    case PT_ACTION_SPLIT:            action_split(w, (PtSplitKind)c->arg); break;
    case PT_ACTION_CLOSE_PANE:       action_close_pane(w); break;
    case PT_ACTION_FOCUS_NEXT:       action_focus_next(w); break;
    case PT_ACTION_FOCUS_PREV:       action_focus_prev(w); break;
    case PT_ACTION_FOCUS_DIRECTION:
      action_focus_direction(w, (PtPaneDirection)c->arg);
      break;
    case PT_ACTION_PASTE:            action_paste(w); break;
    case PT_ACTION_COPY:             action_copy(w); break;
    case PT_ACTION_ZOOM:             action_zoom(w, c->arg); break;
  }
  return TRUE;
}

static void add_shortcut(GtkShortcutController *ctl, const char *accel,
                         GtkShortcutFunc fn, gpointer data,
                         GDestroyNotify destroy) {
  GtkShortcutTrigger *trig = gtk_shortcut_trigger_parse_string(accel);
  g_warn_if_fail(trig != NULL);   /* never ship a silent, unparseable binding */
  gtk_shortcut_controller_add_shortcut(ctl,
      gtk_shortcut_new(trig, gtk_callback_action_new(fn, data, destroy)));
}

/* ⌃⇥ / ⌃⇧⇥ cycle projects — the axis of ⌃1…9 — and ⌥⇥ / ⌥⇧⇥ cycle tabs within
 * the active project, the axis of ⌥1…9, when the compositor does not claim them
 * first. No grab is attempted.
 *
 * They are dispatched by hand because the table above cannot spell them. Tab and
 * ISO_Left_Tab are one physical key at two shift levels, and gdk_key_event_matches
 * — what a GtkKeyvalTrigger asks — accepts either keysym for that key's keycode,
 * so one plain ⌥⇥ press matches an <Alt>Tab trigger and an ISO_Left_Tab+ALT one
 * alike. GtkShortcutController answers repeated matches by round-robin (it
 * resumes its search after whichever shortcut it fired last), so six identical
 * ⌥⇥ presses ran next, prev, next, prev — a two-tab bounce — while ⌥⇧⇥, which
 * arrives as ISO_Left_Tab with SHIFT still in the state, matched neither trigger
 * and did nothing. A key handler can say "exactly this chord"; a trigger cannot.
 *
 * The CAPTURE phase and the overlay rule are the shortcut controller's, for the
 * same reasons: see the comment on overlay_open. */
static gboolean on_tab_chord(GtkEventControllerKey *ctl, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer user) {
  (void)ctl; (void)keycode;
  if (keyval != GDK_KEY_Tab && keyval != GDK_KEY_KP_Tab &&
      keyval != GDK_KEY_ISO_Left_Tab)
    return GDK_EVENT_PROPAGATE;
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
  /* Exactly Control or exactly Alt, Shift optional. Anything else — ⌃⌥⇥, ⌘⇥ —
   * is somebody else's chord and travels on. */
  GdkModifierType base = mods & ~GDK_SHIFT_MASK;
  if (base != GDK_CONTROL_MASK && base != GDK_ALT_MASK)
    return GDK_EVENT_PROPAGATE;
  PtWindow *w = PT_WINDOW(user);
  if (overlay_open(w)) return GDK_EVENT_PROPAGATE;
  /* Some setups deliver a shifted Tab as ISO_Left_Tab with SHIFT already spent,
   * so the keysym is the second half of the answer, not a detail. */
  gboolean backward = (mods & GDK_SHIFT_MASK) != 0 ||
                      keyval == GDK_KEY_ISO_Left_Tab;
  if (base == GDK_CONTROL_MASK) {
    if (backward) action_prev_project(w);
    else          action_next_project(w);
  } else {
    if (backward) action_prev_tab(w);
    else          action_next_tab(w);
  }
  return GDK_EVENT_STOP;
}

static void install_shortcuts(PtWindow *w) {
  GtkShortcutController *ctl =
      GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(ctl),
                                             GTK_PHASE_CAPTURE);
  add_shortcut(ctl, "<Control>k", sc_palette, w, NULL);
  add_shortcut(ctl, "<Control>comma", sc_settings, w, NULL);
  /* One allocation for the whole table, owned by the window: the rows are
   * static, so the only thing a callback needs on top of its row is w. */
  ShortcutCtx *ctxs = g_new(ShortcutCtx, G_N_ELEMENTS(shortcuts));
  g_object_set_data_full(G_OBJECT(w), "pt-shortcut-ctxs", ctxs, g_free);
  for (gsize i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
    ctxs[i].w = w;
    ctxs[i].id = shortcuts[i].id;
    ctxs[i].arg = shortcuts[i].arg;
    add_shortcut(ctl, shortcuts[i].accel, shortcut_dispatch, &ctxs[i], NULL);
  }
  gtk_widget_add_controller(GTK_WIDGET(w), GTK_EVENT_CONTROLLER(ctl));

  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_tab_chord), w);
  gtk_widget_add_controller(GTK_WIDGET(w), keys);
}

/* ---------- persistence ---------- */
/* The session file speaks positions (it names nothing between runs), so this
 * is one of the two places ids become indices on purpose. The file format is
 * unchanged. */
static PtSessionState *capture_state(PtWindow *w) {
  PtSessionState *s = pt_session_state_new();
  guint active_idx =
      pt_workspace_project_index(w->ws, pt_workspace_active_project(w->ws));
  s->active_project = active_idx == PT_WS_INDEX_NONE ? -1 : (int)active_idx;
  /* The committed config, not the live terminal size: a settings preview moves
   * the terminals while w->config deliberately stays put, and a save armed
   * mid-preview would otherwise persist a size the user never accepted (and
   * the next launch's migration would seed it into the config file). */
  s->font_size = w->config != NULL ? w->config->font_size : pt_terminal_font_size();
  for (guint i = 0; i < pt_workspace_project_count(w->ws); i++) {
    PtWsId id = pt_workspace_project_at(w->ws, i);
    PtProjectState *ps =
        pt_project_state_new(pt_workspace_project_name(w->ws, id),
                             pt_workspace_project_path(w->ws, id));
    guint at = pt_workspace_tab_index(w->ws,
                                      pt_workspace_active_tab(w->ws, id));
    /* -1 for a project with no tabs, byte-compatible with the old int
     * bookkeeping (whose clamp left -1 when the last tab closed); restore
     * clamps it back into range once tabs exist again. */
    ps->active_tab = at == PT_WS_INDEX_NONE ? -1 : (int)at;
    ps->accent = pt_workspace_project_accent(w->ws, id);
    for (guint j = 0; j < pt_workspace_tab_count(w->ws, id); j++) {
      PtTabUI *t = pt_workspace_get_data(
          w->ws, pt_workspace_tab_at(w->ws, id, j));
      PtPaneGrid *grid = PT_PANE_GRID(t->grid);
      pt_pane_grid_sync_cwds(grid);
      pt_pane_grid_sync_agents(grid);
      PtSplitNode *copy = pt_split_copy(pt_pane_grid_tree(grid));
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
  /* Config owns fonts and colors from here on; it must be live before the
   * first tab widget spawns a terminal. */
  init_config(w, s);
  if (s == NULL) return;
  for (guint i = 0; i < s->projects->len; i++) {
    PtProjectState *ps = g_ptr_array_index(s->projects, i);
    /* Same construction as a project the user adds — the saved accent instead
     * of the next one in the cycle, and the saved tabs instead of one fresh
     * shell. project_ui_alloc has already seeded the branch, which the tabs
     * below put in the env of every shell they spawn. */
    PtProjectUI *p = project_ui_alloc(w, ps->name, ps->path, ps->accent);
    if (!p->missing) {
      for (guint j = 0; j < ps->tabs->len; j++) {
        PtTabState *ts = g_ptr_array_index(ps->tabs, j);
        /* Before add_tab_ui takes the tree and builds panes from it: with
         * resume-agents off, a restored pane must be a plain shell. */
        if (w->config == NULL || !w->config->resume_agents)
          pt_split_strip_agents(ts->tree);
        /* steal the tree from the session copy */
        add_tab_ui(w, p, ts->title, ts->tree);
        ts->tree = NULL;
      }
      int tabs = (int)pt_workspace_tab_count(w->ws, p->id);
      if (tabs == 0) {
        add_tab_ui(w, p, "shell", pt_split_leaf_new(ps->path));
        tabs = 1;
      }
      pt_workspace_set_active_tab(
          w->ws, pt_workspace_tab_at(w->ws, p->id,
                                     (guint)CLAMP(ps->active_tab, 0,
                                                  tabs - 1)));
      p->monitor = pt_git_monitor_new(ps->path, on_git_update, p);
    }
  }
  guint count = pt_workspace_project_count(w->ws);
  if (count > 0)
    pt_workspace_set_active_project(
        w->ws, pt_workspace_project_at(
                   w->ws, (guint)CLAMP(s->active_project, 0,
                                       (int)count - 1)));
  /* Monitors start on the fast poll; back the restored background projects
   * off now that the active one is known. */
  sync_git_monitors(w);
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
  if (w->save_source != 0) {
    g_source_remove(w->save_source);
    w->save_source = 0;
    do_save(w);   /* a quit that skipped on_close_request still writes it out */
  }
  if (w->status_source != 0) {
    g_source_remove(w->status_source);
    w->status_source = 0;
  }
  if (w->sidebar_idle != 0) {
    g_source_remove(w->sidebar_idle);
    w->sidebar_idle = 0;
  }
  /* Before the config goes: render_config pushes into this, and a late reload
   * landing on a freed monitor would be the one path that outlives it. */
  g_clear_pointer(&w->agents, pt_agent_monitor_free);
  g_clear_object(&w->config_monitor);
  g_clear_object(&w->theme_monitor);
  g_clear_object(&w->agent_report_monitor);
  theme_cache_drop();   /* module-level, but nothing renders past dispose */
  if (w->config_reload_source != 0) {
    g_source_remove(w->config_reload_source);
    w->config_reload_source = 0;
  }
  if (w->theme_reload_source != 0) {
    g_source_remove(w->theme_reload_source);
    w->theme_reload_source = 0;
  }
  if (w->config_save_source != 0) {
    g_source_remove(w->config_save_source);
    w->config_save_source = 0;
    config_save_now(w);        /* flush the pending write before the free */
  }
  g_clear_pointer(&w->config, pt_config_free);
  /* w->ws goes NULL before any teardown work, so a grid signal fired by the
   * unparenting below (or a queued one landing later) sees a dead window and
   * bails — the same contract the projects array's clear-before-free gave. */
  if (w->ws != NULL) {
    PtWorkspace *ws = w->ws;
    w->ws = NULL;
    while (pt_workspace_project_count(ws) > 0)
      remove_project_ui(ws, pt_workspace_project_at(ws, 0));
    pt_workspace_free(ws);
  }
  /* After the workspace teardown, not with the monitors above: a grid signal
   * landing mid-dispose (ws still set) re-arms through the latch, so the
   * latch must outlive it. */
  g_clear_pointer(&w->agent_notified, pt_agent_latch_free);
  G_OBJECT_CLASS(pt_window_parent_class)->dispose(obj);
}

static void pt_window_class_init(PtWindowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = pt_window_dispose;
}

static void pt_window_init(PtWindow *w) {
  w->ws = pt_workspace_new();
  /* pt-dark's accents, until the first render resolves the real theme. */
  static const char *const accent_seed[PT_ACCENT_COUNT] = {
    "#6ee7a0", "#8ab4f8", "#f2b25c", "#c99bf0", "#5ed3c4", "#e0849b",
  };
  for (int i = 0; i < PT_ACCENT_COUNT; i++)
    g_strlcpy(w->accent_hex[i], accent_seed[i], sizeof w->accent_hex[i]);

  /* The project bar is the window's header: it spans the full width, owning the
   * window controls and the drag handle, and everything else — rail, shell
   * column, info panel — lives in the body beneath it. The status line is the
   * exception, scoped to the shell column, so the rail and the panel run all
   * the way down to the window's bottom edge. */
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  w->projectbar = pt_project_bar_new();
  gtk_box_append(GTK_BOX(root), w->projectbar);

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

  /* Right rail, mirror of the sidebar: full body height, hidden until ⌃I or
   * the tab strip's panel button, and never persisted — every launch starts
   * without it. */
  w->infopanel = pt_info_panel_new();
  gtk_widget_set_visible(w->infopanel, FALSE);
  /* Same gate as the tab strip's Zed button: a button that cannot do anything
   * is worse than no button. PATH does not move under a running window. */
  char *zed = g_find_program_in_path("zed");
  pt_info_panel_set_has_zed(PT_INFO_PANEL(w->infopanel), zed != NULL);
  g_free(zed);
  gtk_box_append(GTK_BOX(body), w->infopanel);
  /* Before restore_state, which loads the config and pushes claude-usage into
   * it. Idle until the panel is first shown. */
  w->agents = pt_agent_monitor_new(on_agent_usage_changed, w);

  gtk_box_append(GTK_BOX(root), body);

  /* The palette floats over everything, top bar and rail included, so it wraps
   * the whole root rather than the shell column. GtkOverlay leaves overlay
   * children out of its size request, so a hidden palette costs the layout
   * nothing. */
  GtkWidget *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), root);
  w->palette = pt_command_palette_new();
  gtk_widget_set_visible(w->palette, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->palette);
  /* Same treatment, same stack: the settings dialog has to be a sibling of the
   * palette or its grab_focus would pull focus out of the overlay entirely. */
  w->settings = pt_settings_new();
  gtk_widget_set_visible(w->settings, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), w->settings);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(w), overlay);

  g_signal_connect(w->palette, "activated",
                   G_CALLBACK(on_palette_activated), w);
  g_signal_connect(w->palette, "closed", G_CALLBACK(on_palette_closed), w);
  g_signal_connect(w->settings, "changed",
                   G_CALLBACK(on_settings_changed), w);
  g_signal_connect(w->settings, "committed",
                   G_CALLBACK(on_settings_committed), w);
  g_signal_connect(w->settings, "reverted",
                   G_CALLBACK(on_settings_reverted), w);
  g_signal_connect(w->settings, "closed", G_CALLBACK(on_settings_closed), w);
  g_signal_connect(w->sidebar, "project-selected",
                   G_CALLBACK(on_project_selected), w);
  g_signal_connect(w->sidebar, "project-add", G_CALLBACK(on_project_add), w);
  g_signal_connect(w->sidebar, "project-remove",
                   G_CALLBACK(on_project_remove), w);
  g_signal_connect(w->sidebar, "project-moved",
                   G_CALLBACK(on_project_moved), w);
  g_signal_connect(w->sidebar, "search-escape",
                   G_CALLBACK(on_search_escape), w);
  g_signal_connect(w->tabstrip, "tab-selected",
                   G_CALLBACK(on_tab_selected), w);
  g_signal_connect(w->tabstrip, "tab-new", G_CALLBACK(on_tab_new), w);
  g_signal_connect(w->tabstrip, "tab-close", G_CALLBACK(on_tab_close), w);
  g_signal_connect(w->tabstrip, "tab-moved", G_CALLBACK(on_tab_moved), w);
  g_signal_connect(w->tabstrip, "open-editor", G_CALLBACK(on_open_editor), w);
  g_signal_connect(w->tabstrip, "toggle-panel",
                   G_CALLBACK(on_toggle_panel), w);
  g_signal_connect(w->infopanel, "open-editor",
                   G_CALLBACK(on_info_open_editor), w);
  g_signal_connect(w->infopanel, "open-files",
                   G_CALLBACK(on_info_open_files), w);
  g_signal_connect(w->infopanel, "copy-path",
                   G_CALLBACK(on_info_copy_path), w);
  g_signal_connect(w->infopanel, "refresh", G_CALLBACK(on_info_refresh), w);
  g_signal_connect(w->infopanel, "usage-enable",
                   G_CALLBACK(on_info_usage_enable), w);

  install_shortcuts(w);
  g_signal_connect(w, "close-request", G_CALLBACK(on_close_request), NULL);
  /* Crash leftovers: reports whose panes died with a previous process. Live
   * panes rewrite theirs; a week is comfortably past any real session. */
  pt_agent_session_sweep(7);
  /* Lifecycle reports land as file drops into the same directory the sweep
   * just made sure exists. */
  watch_agent_reports(w);
  restore_state(w);
  refresh_sidebar(w);
  show_active_grid(w);

  /* Plain pointer, no ref: the source must not keep the window alive. Removed
   * in dispose, which is the only place the window can go away from. */
  w->status_source = g_timeout_add(500, tick_statusline, w);
}

void pt_window_install_app_actions(AdwApplication *app) {
  if (g_action_map_lookup_action(G_ACTION_MAP(app), "activate-pane") != NULL)
    return;
  GSimpleAction *act =
      g_simple_action_new("activate-pane", G_VARIANT_TYPE("(tt)"));
  g_signal_connect(act, "activate", G_CALLBACK(on_activate_pane_action), app);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
  g_object_unref(act);
}

GtkWidget *pt_window_new(AdwApplication *app) {
  return g_object_new(PT_TYPE_WINDOW, "application", app,
                      "title", "pt",
                      "default-width", 1100, "default-height", 700, NULL);
}
