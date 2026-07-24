/* pt-tab-strip.h */
#pragma once
#include <gtk/gtk.h>
#define PT_TYPE_TAB_STRIP (pt_tab_strip_get_type())
G_DECLARE_FINAL_TYPE(PtTabStrip, pt_tab_strip, PT, TAB_STRIP, GtkWidget)
GtkWidget *pt_tab_strip_new(void);
/* titles: const char* elements; rebuilds the buttons. */
void pt_tab_strip_set_tabs(PtTabStrip *s, GPtrArray *titles, int active);
void pt_tab_strip_set_activity(PtTabStrip *s, int index, gboolean on);
/* Signals: "tab-selected" (int index), "tab-new" (void) */
