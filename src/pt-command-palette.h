#pragma once
#include <gtk/gtk.h>
#include "pt-agent.h"

/* The CSS classes this widget carries are still ".pt-palette-*" (and the
 * overlay host is still ".pt-palette"): style.css names the *look*, and
 * renaming the C symbols is no reason to churn a stylesheet that the settings
 * dialog also borrows from. The mismatch between the C name and the CSS name
 * is deliberate. */

/* A command value below every window command (which are >= 0; -1 already
 * means "not a command"): the palette answers this one itself, swapping its
 * rows to recent agent sessions, and the window never sees an activation
 * carrying it. */
#define PT_COMMAND_PALETTE_RECENT_SESSIONS (-100)

/* One row of the command palette. All strings are owned by the item and are
 * freed by the palette; `shortcut` may be NULL. */
typedef struct {
  char *name;       /* project or shell title */
  char *detail;     /* project: path · branch; shell: project name */
  char *shortcut;   /* "^3" for the first 9 projects, else NULL */
  int accent;
  gboolean is_shell;
  gboolean is_command;  /* a window command, not somewhere to switch to */
  /* Switch targets are workspace ids, not positions: the palette stays open
   * across an async gap (a background shell can exit and drop its tab while
   * the user types), and an index captured at open time would then name
   * whatever slid into the slot. A dead id activates as a no-op instead.
   * 0 = none: commands carry neither, project rows carry no tab. */
  guint project_id;
  guint tab_id;
  int command;          /* which command, when is_command; else -1 */
  /* A recent agent session row (is_history). Activating one asks — through
   * "history-activated" — to resume that session in a pane in `history_cwd`.
   * history_dead marks a row that only informs: a cwd that is gone on disk,
   * or the empty-list note. It renders but activation is a no-op. */
  gboolean is_history;
  gboolean history_dead;
  PtAgentKind history_agent;
  char *history_session_id;
  char *history_cwd;
} PtCommandPaletteItem;

#define PT_TYPE_COMMAND_PALETTE (pt_command_palette_get_type())
G_DECLARE_FINAL_TYPE(PtCommandPalette, pt_command_palette, PT, COMMAND_PALETTE,
                     GtkWidget)

GtkWidget *pt_command_palette_new(void);

/* Takes ownership of `items` (an array of n_items PtCommandPaletteItem
 * allocated with g_malloc; freed internally, strings included). Shows the
 * overlay, clears the query and focuses the input. */
void pt_command_palette_open(PtCommandPalette *p, PtCommandPaletteItem *items,
                             int n_items);
void pt_command_palette_close(PtCommandPalette *p);
gboolean pt_command_palette_is_open(PtCommandPalette *p);

/* Signals: "activated" (guint project_id, guint tab_id, int command),
 *          "history-activated" (int agent_kind, const char *session_id,
 *              const char *cwd) — a recent-session row was picked; the fields
 *              are copied before emission, so they survive the close that
 *              follows,
 *          "closed" (void) — emitted on any dismissal. */
