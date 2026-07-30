#pragma once
#include <gtk/gtk.h>

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
} PtPaletteItem;

#define PT_TYPE_PALETTE (pt_palette_get_type())
G_DECLARE_FINAL_TYPE(PtPalette, pt_palette, PT, PALETTE, GtkWidget)

GtkWidget *pt_palette_new(void);

/* Takes ownership of `items` (an array of n_items PtPaletteItem allocated with
 * g_malloc; freed internally, strings included). Shows the overlay, clears the
 * query and focuses the input. */
void pt_palette_open(PtPalette *p, PtPaletteItem *items, int n_items);
void pt_palette_close(PtPalette *p);
gboolean pt_palette_is_open(PtPalette *p);

/* Signals: "activated" (guint project_id, guint tab_id, int command),
 *          "closed" (void) — emitted on any dismissal. */
