/* pt-info-panel.h */
#pragma once
#include <gtk/gtk.h>
#include "pt-agent-monitor.h"
#include "pt-git-parse.h"
#define PT_TYPE_INFO_PANEL (pt_info_panel_get_type())
G_DECLARE_FINAL_TYPE(PtInfoPanel, pt_info_panel, PT, INFO_PANEL, GtkWidget)
GtkWidget *pt_info_panel_new(void);
/* Identity block: shell name, its pid, the directory shown to the user, and the
 * project accent for the dot. Strings are copied. */
void pt_info_panel_set_info(PtInfoPanel *ip, const char *shell, int pid,
                            const char *dir, int accent);
/* Git block. `files` holds PtGitFile*; the panel keeps its own reference to
 * the array instead of copying it, so treat a handed-in array as frozen —
 * replace it wholesale, never mutate it in place. NULL means none. */
void pt_info_panel_set_git(PtInfoPanel *ip, const PtGitStatus *st,
                           gboolean is_repo, GPtrArray *files);
/* Hides the Zed button when zed is not on PATH. */
void pt_info_panel_set_has_zed(PtInfoPanel *ip, gboolean has_zed);

/* Agent usage block. The whole section hides when no agent is running in the
 * pane, which is the normal state — a terminal is a terminal.
 *
 * `now` comes from the caller rather than the clock so that every countdown
 * in one refresh is measured against the same instant, and so the panel stays
 * a pure function of what it is handed. Nothing here is copied: the view is
 * read and forgotten before this returns. */
void pt_info_panel_set_usage(PtInfoPanel *ip, const PtAgentView *view,
                             gint64 now);

/* Signals: "open-editor", "open-files", "copy-path", "refresh",
 * "usage-enable" — all void. The window owns what each one acts on; the panel
 * only reports the click. "usage-enable" is the opt-in for looking up Claude
 * Code's usage, which is a network call with the user's token and so is never
 * anything but a deliberate press. */
