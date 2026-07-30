#pragma once
#include <gtk/gtk.h>

/* The CSS classes this widget carries are still ".pt-palette-*" (and the
 * overlay host is still ".pt-palette"): style.css names the *look*, and
 * renaming the C symbols is no reason to churn a stylesheet that the settings
 * dialog also borrows from. The mismatch between the C name and the CSS name
 * is deliberate. */

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
 *          "closed" (void) — emitted on any dismissal. */
