#pragma once
#include <glib.h>
#include "pt-git-parse.h"
typedef struct PtGitMonitor PtGitMonitor;
/* `files` holds PtGitFile* for the same status and transfers to the callback:
 * the callback owner keeps the array and unrefs it when done. */
typedef void (*PtGitMonitorCb)(const PtGitStatus *status, GPtrArray *files,
                               gboolean is_repo, gpointer user);
PtGitMonitor *pt_git_monitor_new(const char *repo_path, PtGitMonitorCb cb,
                                 gpointer user);
void pt_git_monitor_refresh(PtGitMonitor *m);
/* Line counts cost a `git diff` subprocess per poll, so they are only fetched
 * while wanted (the info panel is on screen). Stores only, and returns TRUE
 * when the counts want a refresh to land before the next poll — the caller
 * makes it, so that flipping both settings at once costs one poll, not two. */
gboolean pt_git_monitor_set_want_line_counts(PtGitMonitor *m, gboolean want);
/* active: 5s poll; inactive: 60s. Stores only; returns TRUE on going active,
 * where a refresh keeps a project switched back to off minute-old state. */
gboolean pt_git_monitor_set_active(PtGitMonitor *m, gboolean active);
void pt_git_monitor_free(PtGitMonitor *m);
