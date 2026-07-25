/* pt-project-bar.h */
#pragma once
#include <gtk/gtk.h>
#define PT_TYPE_PROJECT_BAR (pt_project_bar_get_type())
G_DECLARE_FINAL_TYPE(PtProjectBar, pt_project_bar, PT, PROJECT_BAR, GtkWidget)
GtkWidget *pt_project_bar_new(void);
/* "/home/me/dev/foo" → "~/dev/foo". Caller frees. Shared so every surface that
 * shows a project path spells it the same way. */
char *pt_path_home_abbrev(const char *path);
/* branch NULL (or empty) → hide the chip. accent 0..5. */
void pt_project_bar_update(PtProjectBar *b, const char *name, const char *path,
                           const char *branch, int changed, int accent);
