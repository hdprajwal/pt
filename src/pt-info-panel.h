/* pt-info-panel.h */
#pragma once
#include <gtk/gtk.h>
#include "pt-git-parse.h"
#define PT_TYPE_INFO_PANEL (pt_info_panel_get_type())
G_DECLARE_FINAL_TYPE(PtInfoPanel, pt_info_panel, PT, INFO_PANEL, GtkWidget)
GtkWidget *pt_info_panel_new(void);
/* Identity block: shell name, its pid, the directory shown to the user, and the
 * project accent for the dot. Strings are copied. */
void pt_info_panel_set_info(PtInfoPanel *ip, const char *shell, int pid,
                            const char *dir, int accent);
/* Git block. `files` holds PtGitFile* and is deep-copied; NULL means none. */
void pt_info_panel_set_git(PtInfoPanel *ip, const PtGitStatus *st,
                           gboolean is_repo, GPtrArray *files);
/* Hides the Zed button when zed is not on PATH. */
void pt_info_panel_set_has_zed(PtInfoPanel *ip, gboolean has_zed);
/* Signals: "open-editor", "open-files", "copy-path", "refresh" — all void.
 * The window owns what each one acts on; the panel only reports the click. */
