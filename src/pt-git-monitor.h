#pragma once
#include <glib.h>
#include "pt-git-parse.h"
typedef struct PtGitMonitor PtGitMonitor;
typedef void (*PtGitMonitorCb)(const PtGitStatus *status, gboolean is_repo,
                               gpointer user);
PtGitMonitor *pt_git_monitor_new(const char *repo_path, PtGitMonitorCb cb,
                                 gpointer user);
void pt_git_monitor_refresh(PtGitMonitor *m);
void pt_git_monitor_free(PtGitMonitor *m);
