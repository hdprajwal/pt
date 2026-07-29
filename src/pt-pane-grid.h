#pragma once
#include <gtk/gtk.h>
#include "pt-split-tree.h"
#include "pt-terminal.h"

#define PT_TYPE_PANE_GRID (pt_pane_grid_get_type())
G_DECLARE_FINAL_TYPE(PtPaneGrid, pt_pane_grid, PT, PANE_GRID, GtkWidget)

/* Takes ownership of tree; creates a PtTerminal per leaf (cwd from leaf). */
GtkWidget *pt_pane_grid_new(PtSplitNode *tree);
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
int pt_pane_grid_pane_count(PtPaneGrid *g);
/* TRUE when any pane in the grid has a foreground process other than the shell. */
gboolean pt_pane_grid_any_running(PtPaneGrid *g);
int pt_pane_grid_focused_index(PtPaneGrid *g);
void pt_pane_grid_sync_cwds(PtPaneGrid *g); /* leaf->cwd ← live terminal cwd */
void pt_pane_grid_focus_terminal(PtPaneGrid *g); /* grab focus on focused pane */
/* Signals: "structure-changed" (void) — split/close happened (persist!);
 *          "focus-changed" (void) — focused pane changed;
 *          "command-changed" (const char*) — focused pane's foreground program
 *              changed (or a focus move landed on a pane with a known command);
 *          "title-changed" (const char*) — focused pane's title changed (the
 *              prompt reports the last exit code through it, so this is the
 *              instant edge for the status bar's exit marker);
 *          "emptied" (void) — last pane closed via clean shell exit (close tab);
 *          "notification" (guint64 pane_id, const char *title,
 *              const char *body) — a program in *any* pane of this grid asked
 *              for a desktop notification (OSC 9 / OSC 777). Unlike the two
 *              above this is not restricted to the focused pane: an unwatched
 *              pane is exactly where these come from. */
