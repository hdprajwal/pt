#include "pt-git-monitor.h"
#include <gio/gio.h>
#include <string.h>

#define PT_GIT_POLL_ACTIVE_S   5
#define PT_GIT_POLL_INACTIVE_S 60

struct PtGitMonitor {
  char *path;
  PtGitMonitorCb cb;
  gpointer user;
  GFileMonitor *head_mon;
  GFileMonitor *index_mon;
  guint timer;
  gboolean active;           /* 5s poll vs 60s backoff */
  gboolean want_line_counts; /* gate the numstat subprocess */
  guint generation;          /* discard stale async results */
  gboolean freed;            /* free() while a subprocess is in flight */
  int refs;
};

static void monitor_unref(PtGitMonitor *m) {
  if (--m->refs > 0) return;
  g_free(m->path);
  g_free(m);
}

/* One poll: the status pass captures git's output, an optional numstat pass
 * adds its own, and delivery parses both in a single call. */
typedef struct {
  PtGitMonitor *m;
  guint generation;
  char *status_out;      /* owned; NULL when not a repository */
  gboolean is_repo;
} Inflight;

static gboolean inflight_current(Inflight *inf) {
  return !inf->m->freed && inf->generation == inf->m->generation &&
         inf->m->cb != NULL;
}

static void deliver(Inflight *inf, const char *numstat_out) {
  PtGitMonitor *m = inf->m;
  if (inflight_current(inf)) {
    /* The one parse per poll; stale results never pay for one at all. */
    PtGitStatus st;
    GPtrArray *files = NULL;
    pt_git_result_parse(inf->status_out, numstat_out, &st, &files);
    m->cb(&st, files, inf->is_repo, m->user);   /* files transfer to the cb */
  }
  g_free(inf->status_out);
  monitor_unref(m);
  g_free(inf);
}

/* Whether the porcelain output lists any entry at all — decides if a numstat
 * pass would have anything to count, without parsing. */
static gboolean has_entry_lines(const char *text) {
  for (const char *line = text; line != NULL && *line != '\0';) {
    if ((line[0] == '1' || line[0] == '2' || line[0] == 'u' ||
         line[0] == '?') && line[1] == ' ')
      return TRUE;
    const char *nl = strchr(line, '\n');
    line = nl != NULL ? nl + 1 : NULL;
  }
  return FALSE;
}

static void on_numstat_done(GObject *src, GAsyncResult *res, gpointer user) {
  Inflight *inf = user;
  char *out = NULL;
  GSubprocess *proc = G_SUBPROCESS(src);
  g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, NULL);
  /* A repo with no commits yet has no HEAD to diff against, so this pass just
   * fails — the file list still stands, only without counts. */
  gboolean ok = g_subprocess_get_successful(proc) && out != NULL;
  g_object_unref(proc);
  deliver(inf, ok ? out : NULL);
  g_free(out);
}

static void on_git_done(GObject *src, GAsyncResult *res, gpointer user) {
  Inflight *inf = user;
  PtGitMonitor *m = inf->m;
  char *out = NULL;
  GSubprocess *proc = G_SUBPROCESS(src);
  g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, NULL);
  inf->is_repo = g_subprocess_get_successful(proc) && out != NULL;
  if (inf->is_repo) inf->status_out = out;
  else              g_free(out);
  g_object_unref(proc);

  /* Porcelain carries no line counts, so a second pass fetches them and the
   * callback waits for it. Only run while someone is looking at counts, and
   * only when there is something to count. */
  if (inf->is_repo && m->want_line_counts &&
      has_entry_lines(inf->status_out) && inflight_current(inf)) {
    GSubprocess *diff = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        NULL, "git", "-C", m->path, "diff", "HEAD", "--numstat", NULL);
    if (diff != NULL) {
      g_subprocess_communicate_utf8_async(diff, NULL, NULL, on_numstat_done,
                                          inf);
      return;
    }
  }
  deliver(inf, NULL);
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
      PtGitStatus st;
      GPtrArray *files = NULL;
      pt_git_result_parse(NULL, NULL, &st, &files);
      m->cb(&st, files, FALSE, m->user);   /* files transfer to the cb */
    }
    return;
  }
  Inflight *inf = g_new0(Inflight, 1);
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

static void restart_timer(PtGitMonitor *m) {
  if (m->timer != 0) g_source_remove(m->timer);
  m->timer = g_timeout_add_seconds(
      m->active ? PT_GIT_POLL_ACTIVE_S : PT_GIT_POLL_INACTIVE_S, on_timer, m);
}

/* Both setters store and report, they do not poll: a caller that flips both on
 * the same monitor would otherwise spawn two `git status` runs and throw the
 * first one's answer away. The refresh is the caller's to make, once. */
gboolean pt_git_monitor_set_want_line_counts(PtGitMonitor *m, gboolean want) {
  if (m == NULL || m->want_line_counts == want) return FALSE;
  m->want_line_counts = want;
  /* The next delivery must carry counts, not the next poll's. */
  return want;
}

gboolean pt_git_monitor_set_active(PtGitMonitor *m, gboolean active) {
  if (m == NULL || m->active == active) return FALSE;
  m->active = active;
  restart_timer(m);
  return active;
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
  m->active = TRUE;   /* a new monitor polls fast until told otherwise */
  m->head_mon = watch(m, "HEAD");
  m->index_mon = watch(m, "index");
  restart_timer(m);
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
