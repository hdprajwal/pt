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
  int project_idx;
  int tab_idx;      /* -1 for project items */
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

/* Signals: "activated" (int project_idx, int tab_idx),
 *          "closed" (void) — emitted on any dismissal. */
