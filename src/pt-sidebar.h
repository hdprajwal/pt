/* pt-sidebar.h */
#pragma once
#include <gtk/gtk.h>
#include "pt-git-parse.h"
typedef struct {
  const char *name;
  char branch[128];
  int changed;
  gboolean is_repo;
  gboolean missing;
} PtSidebarRow;
#define PT_TYPE_SIDEBAR (pt_sidebar_get_type())
G_DECLARE_FINAL_TYPE(PtSidebar, pt_sidebar, PT, SIDEBAR, GtkWidget)
GtkWidget *pt_sidebar_new(void);
void pt_sidebar_set_projects(PtSidebar *sb, const PtSidebarRow *rows,
                             int n_rows, int active);
/* Signals: "project-selected" (int), "project-add" (void), "project-remove" (int) */
