#pragma once
#include <glib.h>
#include "pt-git-parse.h"
typedef struct PtGitMonitor PtGitMonitor;
/* `files` holds PtGitFile* for the same status and is borrowed: the monitor
 * frees it as soon as the callback returns, so keep a copy, not the array. */
typedef void (*PtGitMonitorCb)(const PtGitStatus *status, GPtrArray *files,
                               gboolean is_repo, gpointer user);
PtGitMonitor *pt_git_monitor_new(const char *repo_path, PtGitMonitorCb cb,
                                 gpointer user);
void pt_git_monitor_refresh(PtGitMonitor *m);
void pt_git_monitor_free(PtGitMonitor *m);
