#pragma once
#include <gtk/gtk.h>
#include "pt-split-tree.h"
#include "pt-terminal.h"

#define PT_TYPE_PANE_GRID (pt_pane_grid_get_type())
G_DECLARE_FINAL_TYPE(PtPaneGrid, pt_pane_grid, PT, PANE_GRID, GtkWidget)

/* Takes ownership of tree; creates a PtTerminal per leaf (cwd from leaf). The
 * two config values are taken here rather than set afterwards because these
 * first panes are built inside this call — see pt_pane_grid_set_pane_defaults,
 * which is the same thing for the panes built later. */
GtkWidget *pt_pane_grid_new(PtSplitNode *tree, gboolean mouse_reporting,
                            PtOsc52Mode osc52);
/* The env every pane of this grid hands its shell: NULL-terminated
 * "KEY=VALUE" strings, copied (NULL clears). Panes are built deep inside the
 * grid, from split-tree leaves with no project context of their own, so the
 * window sets it here once per grid and the grid passes it to each pane.
 * Applies to future spawns — the grid's existing panes are updated too, but a
 * shell already running keeps the env it started with. */
void pt_pane_grid_set_env(PtPaneGrid *g, const char *const *envv);
/* What a pane this grid builds from here on starts out with: the
 * `mouse-reporting` and `osc52` config values. Only ever applied to panes the
 * grid builds *after* this call, so creating a tab or a split leaves a pane
 * somebody toggled by hand alone — following the file is the config apply's
 * job, and it re-arms every live pane itself. */
void pt_pane_grid_set_pane_defaults(PtPaneGrid *g, gboolean mouse_reporting,
                                    PtOsc52Mode osc52);
PtSplitNode *pt_pane_grid_tree(PtPaneGrid *g);
void pt_pane_grid_split(PtPaneGrid *g, PtSplitKind kind);
/* Close focused pane. Returns FALSE when the grid is now empty (close the tab). */
gboolean pt_pane_grid_close_focused(PtPaneGrid *g);
void pt_pane_grid_focus_next(PtPaneGrid *g);
void pt_pane_grid_focus_prev(PtPaneGrid *g);
typedef enum {
  PT_PANE_DIR_LEFT,
  PT_PANE_DIR_RIGHT,
  PT_PANE_DIR_UP,
  PT_PANE_DIR_DOWN,
} PtPaneDirection;
/* Move focus to the nearest pane in the given direction (spatial, based on
 * on-screen geometry). No-op when no pane lies that way. */
void pt_pane_grid_focus_direction(PtPaneGrid *g, PtPaneDirection dir);
/* The pane with this pt_terminal_id(), or NULL when the grid does not hold it.
 * Callers are expected to ask every grid in turn; a pane closed since is found
 * by nobody. */
PtTerminal *pt_pane_grid_pane_by_id(PtPaneGrid *g, guint64 id);
/* Same lookup, but focus what it finds. */
gboolean pt_pane_grid_focus_pane_by_id(PtPaneGrid *g, guint64 id);
PtTerminal *pt_pane_grid_focused_terminal(PtPaneGrid *g);
/* TRUE when any pane in the grid has a foreground process other than the shell. */
gboolean pt_pane_grid_any_running(PtPaneGrid *g);
void pt_pane_grid_sync_cwds(PtPaneGrid *g); /* leaf->cwd ← live terminal cwd */
/* leaf->agent/agent_session ← the pane's validated agent report. Called on
 * the save path, like sync_cwds: a report is only kept when the agent it
 * names is still alive in the pane (same kind, same pid), so a pane whose
 * agent exited saves as a plain shell. A pane that never spawned a shell
 * keeps whatever it was restored with — nothing ran there to invalidate it. */
void pt_pane_grid_sync_agents(PtPaneGrid *g);
void pt_pane_grid_focus_terminal(PtPaneGrid *g); /* grab focus on focused pane */
/* Signals: "structure-changed" (void) — split/close happened (persist!);
 *          "focus-changed" (void) — focused pane changed;
 *          "command-changed" (const char*) — focused pane's foreground program
 *              changed (or a focus move landed on a pane with a known command);
 *          "title-changed" (const char*) — focused pane's title changed. It
 *              is what the tab wears while a program runs there, so this is
 *              the edge that relabels the tab; secondarily the prompt reports
 *              the last exit code through the title, making it the instant
 *              edge for the status bar's exit marker too;
 *          "emptied" (void) — last pane closed via clean shell exit (close tab);
 *          "notification" (guint64 pane_id, const char *title,
 *              const char *body) — a program in *any* pane of this grid asked
 *              for a desktop notification (OSC 9 / OSC 777). Unlike the two
 *              above this is not restricted to the focused pane: an unwatched
 *              pane is exactly where these come from. */
