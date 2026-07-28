/* pt-sidebar.h */
#pragma once
#include <gtk/gtk.h>
typedef struct {
  const char *name;
  const char *path;
  char branch[128];
  int changed;
  gboolean is_repo;
  gboolean missing;
  int accent;        /* 0..5 */
  int shell_count;   /* total tabs */
  int running;       /* tabs with a foreground process */
} PtSidebarRow;
#define PT_TYPE_SIDEBAR (pt_sidebar_get_type())
G_DECLARE_FINAL_TYPE(PtSidebar, pt_sidebar, PT, SIDEBAR, GtkWidget)
GtkWidget *pt_sidebar_new(void);
/* Deep-copies rows; safe to free the array after the call. */
void pt_sidebar_set_projects(PtSidebar *sb, const PtSidebarRow *rows,
                             int n_rows, int active);
void pt_sidebar_focus_search(PtSidebar *sb);
/* Signals: "project-selected" (int), "project-add" (void),
 *          "project-remove" (int), "project-moved" (int from, int to),
 *          "search-escape" (void) —
 * indexes are ALWAYS original project indexes, filter-independent. */
