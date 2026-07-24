/* pt-statusline.h */
#pragma once
#include <gtk/gtk.h>
#define PT_TYPE_STATUSLINE (pt_statusline_get_type())
G_DECLARE_FINAL_TYPE(PtStatusline, pt_statusline, PT, STATUSLINE, GtkWidget)
GtkWidget *pt_statusline_new(void);
void pt_statusline_update(PtStatusline *sl, const char *project,
                          const char *branch, int changed,
                          int tab_idx, int tab_count,
                          int pane_idx, int pane_count);
