#include "pt-git-monitor.h"
#include <gio/gio.h>
#include <string.h>

struct PtGitMonitor {
  char *path;
  PtGitMonitorCb cb;
  gpointer user;
  GFileMonitor *head_mon;
  GFileMonitor *index_mon;
  guint timer;
  guint generation;      /* discard stale async results */
  gboolean freed;        /* free() while a subprocess is in flight */
  int refs;
};

static void monitor_unref(PtGitMonitor *m) {
  if (--m->refs > 0) return;
  g_free(m->path);
  g_free(m);
}

typedef struct { PtGitMonitor *m; guint generation; } Inflight;

static void on_git_done(GObject *src, GAsyncResult *res, gpointer user) {
  Inflight *inf = user;
  PtGitMonitor *m = inf->m;
  char *out = NULL;
  GSubprocess *proc = G_SUBPROCESS(src);
  g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, NULL);
  if (!m->freed && inf->generation == m->generation && m->cb != NULL) {
    PtGitStatus st;
    gboolean is_repo =
        g_subprocess_get_successful(proc) &&
        out != NULL && pt_git_parse_porcelain_v2(out, &st);
    if (!is_repo) memset(&st, 0, sizeof(st));
    m->cb(&st, is_repo, m->user);
  }
  g_free(out);
  g_object_unref(proc);
  monitor_unref(m);
  g_free(inf);
}

void pt_git_monitor_refresh(PtGitMonitor *m) {
  m->generation++;
  GError *err = NULL;
  GSubprocess *proc = g_subprocess_new(
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      &err, "git", "-C", m->path, "status", "--porcelain=v2", "--branch",
      NULL);
  if (proc == NULL) {   /* git not installed */
    g_clear_error(&err);
    if (m->cb != NULL) {
      PtGitStatus st = {0};
      m->cb(&st, FALSE, m->user);
    }
    return;
  }
  Inflight *inf = g_new(Inflight, 1);
  inf->m = m;
  inf->generation = m->generation;
  m->refs++;
  g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_git_done, inf);
}

static void on_git_file_changed(GFileMonitor *fm, GFile *a, GFile *b,
                                GFileMonitorEvent ev, gpointer user) {
  (void)fm; (void)a; (void)b; (void)ev;
  pt_git_monitor_refresh((PtGitMonitor *)user);
}

static gboolean on_timer(gpointer user) {
  pt_git_monitor_refresh((PtGitMonitor *)user);
  return G_SOURCE_CONTINUE;
}

static GFileMonitor *watch(PtGitMonitor *m, const char *rel) {
  char *p = g_build_filename(m->path, ".git", rel, NULL);
  GFile *f = g_file_new_for_path(p);
  GFileMonitor *fm = g_file_monitor_file(f, G_FILE_MONITOR_NONE, NULL, NULL);
  if (fm != NULL)
    g_signal_connect(fm, "changed", G_CALLBACK(on_git_file_changed), m);
  g_object_unref(f);
  g_free(p);
  return fm;
}

PtGitMonitor *pt_git_monitor_new(const char *repo_path, PtGitMonitorCb cb,
                                 gpointer user) {
  PtGitMonitor *m = g_new0(PtGitMonitor, 1);
  m->path = g_strdup(repo_path);
  m->cb = cb;
  m->user = user;
  m->refs = 1;
  m->head_mon = watch(m, "HEAD");
  m->index_mon = watch(m, "index");
  m->timer = g_timeout_add_seconds(5, on_timer, m);
  pt_git_monitor_refresh(m);
  return m;
}

void pt_git_monitor_free(PtGitMonitor *m) {
  if (m == NULL) return;
  m->freed = TRUE;
  m->cb = NULL;
  if (m->timer != 0) g_source_remove(m->timer);
  g_clear_object(&m->head_mon);
  g_clear_object(&m->index_mon);
  monitor_unref(m);
}
