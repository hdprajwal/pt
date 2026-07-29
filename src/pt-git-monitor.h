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
 * while wanted (the info panel is on screen). Turning them on refreshes at
 * once so the counts do not wait for the next poll. */
void pt_git_monitor_set_want_line_counts(PtGitMonitor *m, gboolean want);
/* active: 5s poll; inactive: 60s. Going active refreshes immediately, so a
 * project switched back to never shows minute-old state. */
void pt_git_monitor_set_active(PtGitMonitor *m, gboolean active);
void pt_git_monitor_free(PtGitMonitor *m);
