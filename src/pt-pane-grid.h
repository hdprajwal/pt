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
PtTerminal *pt_pane_grid_focused_terminal(PtPaneGrid *g);
int pt_pane_grid_pane_count(PtPaneGrid *g);
int pt_pane_grid_focused_index(PtPaneGrid *g);
void pt_pane_grid_sync_cwds(PtPaneGrid *g); /* leaf->cwd ← live terminal cwd */
void pt_pane_grid_focus_terminal(PtPaneGrid *g); /* grab focus on focused pane */
/* Signals: "structure-changed" (void) — split/close happened (persist!);
 *          "activity" (void) — any child terminal produced output;
 *          "focus-changed" (void) — focused pane changed;
 *          "emptied" (void) — last pane closed via clean shell exit (close tab). */
