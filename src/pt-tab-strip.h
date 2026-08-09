/* pt-tab-strip.h */
#pragma once
#include <gtk/gtk.h>

/* One tab's render state. `title` is borrowed for the duration of the
 * pt_tab_strip_set_tabs call only — the strip copies what it needs. */
typedef struct {
  const char *title;
  guint id;           /* the tab's PtWsId: stable, never 0, never reused */
  gboolean running;   /* any pane has a foreground process */
  int last_exit;      /* -1 unknown/ok-so-far, 0 ok, >0 error */
} PtTabInfo;

#define PT_TYPE_TAB_STRIP (pt_tab_strip_get_type())
G_DECLARE_FINAL_TYPE(PtTabStrip, pt_tab_strip, PT, TAB_STRIP, GtkWidget)
GtkWidget *pt_tab_strip_new(void);
/* `accent` is the owning project's 0..5 accent index; it colours the active
 * tab's top edge. */
void pt_tab_strip_set_tabs(PtTabStrip *s, const PtTabInfo *tabs, int n,
                           int active, int accent);
/* Signals: "tab-selected" (int index), "tab-new" (void),
 *          "tab-moved" (guint tab, guint dest, gboolean after) — the tab with
 *          id `tab` was dropped beside the one with id `dest`: before it, or
 *          after it when `after`. Ids, not positions: the strip freezes its
 *          rows for the length of a drag while the workspace keeps moving, so
 *          a drop can name tabs whose positions — or whose project's being the
 *          active one — are long out of date. The receiver resolves them
 *          (pt_workspace_move_tab_beside), and a dead id resolves to a no-op.
 *          "tab-close" (int index) — the × on a tab; closes the whole tab
 *          "open-editor" (void) — the Zed button; opens the active project.
 *          Only emitted when zed is on PATH (the button is otherwise absent).
 *          "toggle-panel" (void) — the info-panel button; always present. */
