/* pt-tab-strip.h */
#pragma once
#include <gtk/gtk.h>

/* One tab's render state. `title` is borrowed for the duration of the
 * pt_tab_strip_set_tabs call only — the strip copies what it needs. */
typedef struct {
  const char *title;
  gboolean running;   /* any pane has a foreground process */
  int last_exit;      /* -1 unknown/ok-so-far, 0 ok, >0 error */
  int unread;         /* 0 → no badge */
  int accent;         /* project accent for the badge */
} PtTabInfo;

#define PT_TYPE_TAB_STRIP (pt_tab_strip_get_type())
G_DECLARE_FINAL_TYPE(PtTabStrip, pt_tab_strip, PT, TAB_STRIP, GtkWidget)
GtkWidget *pt_tab_strip_new(void);
void pt_tab_strip_set_tabs(PtTabStrip *s, const PtTabInfo *tabs, int n,
                           int active);
/* Signals: "tab-selected" (int index), "tab-new" (void),
 *          "tab-close" (int index) — the × on a tab; closes the whole tab */
