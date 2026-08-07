#include "pt-agent.h"
#include <string.h>

/* How far below the shell the walk looks, and how many processes it is willing
 * to read before giving up. A shell running an agent has it one level down;
 * the extra levels are for wrappers (a `mise exec`, a shell function that
 * `exec`s through something). The caps are what keep this from turning into a
 * full /proc scan when a pane happens to be running a build. */
#define PT_AGENT_MAX_DEPTH 4
#define PT_AGENT_MAX_VISIT 64

static const struct { const char *name; PtAgentKind kind; } agent_names[] = {
  { "claude", PT_AGENT_CLAUDE },
  { "codex",  PT_AGENT_CODEX  },
};

PtAgentKind pt_agent_kind_from_name(const char *name) {
  if (name == NULL || name[0] == '\0') return PT_AGENT_NONE;
  /* A path answers for its basename: the foreground poll reports a bare comm,
   * but /proc/<pid>/cmdline and a hand-typed command both carry paths. */
  const char *slash = strrchr(name, '/');
  if (slash != NULL) name = slash + 1;
  for (gsize i = 0; i < G_N_ELEMENTS(agent_names); i++)
    if (strcmp(name, agent_names[i].name) == 0) return agent_names[i].kind;
  return PT_AGENT_NONE;
}

const char *pt_agent_label(PtAgentKind kind) {
  switch (kind) {
    case PT_AGENT_CLAUDE: return "Claude Code";
    case PT_AGENT_CODEX:  return "Codex";
    case PT_AGENT_NONE:   break;
  }
  return "";
}

/* /proc/<pid>/comm, trimmed. NULL when the process is gone — which is the
 * normal outcome for anything read out of a walk over live pids. */
static char *read_comm(int pid) {
  char path[64];
  g_snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  char *comm = NULL;
  if (!g_file_get_contents(path, &comm, NULL, NULL)) return NULL;
  return g_strchomp(comm);
}

/* Interpreters that say nothing about what is actually running: for these the
 * script on the command line is the program, not the binary. */
static gboolean is_runtime(const char *name) {
  static const char *const runtimes[] = {
    "node", "bun", "deno", "python", "python3", "ruby", "sh", "bash", "zsh",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(runtimes); i++)
    if (strcmp(name, runtimes[i]) == 0) return TRUE;
  return FALSE;
}

PtAgentKind pt_agent_kind_from_cmdline(const char *cmdline, gsize len) {
  if (cmdline == NULL) return PT_AGENT_NONE;
  gsize at = 0;
  for (int arg = 0; arg < 2 && at < len; arg++) {
    /* strnlen, not strlen: the buffer came off /proc and the last argument
     * is not guaranteed to have its terminator inside `len`. */
    gsize n = strnlen(cmdline + at, len - at);
    if (n == 0) break;
    char *word = g_strndup(cmdline + at, n);
    PtAgentKind kind = pt_agent_kind_from_name(word);
    g_free(word);
    if (kind != PT_AGENT_NONE) return kind;
    at += n + 1;
  }
  return PT_AGENT_NONE;
}

/* Only reached when comm matched nothing and named a runtime, so the common
 * case never pays for the read. */
static PtAgentKind read_cmdline_kind(int pid) {
  char path[64];
  g_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  char *buf = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &buf, &len, NULL)) return PT_AGENT_NONE;
  PtAgentKind kind = pt_agent_kind_from_cmdline(buf, len);
  g_free(buf);
  return kind;
}

/* Append the child pids of every thread of `pid` to `out`.
 *
 * children is per-thread, not per-process, so a parent that spawned from a
 * worker thread keeps its child under that thread's entry. Reading the whole
 * task directory costs one readdir and covers that; the visit cap upstream is
 * what bounds the total work. A kernel without CONFIG_PROC_CHILDREN has no
 * children file at all, which reads here as "no children" — the foreground
 * fast path is then the only detection there is, and that is the case this
 * degrades to rather than failing. */
static void collect_children(int pid, GArray *out) {
  char taskdir[64];
  g_snprintf(taskdir, sizeof(taskdir), "/proc/%d/task", pid);
  GDir *dir = g_dir_open(taskdir, 0, NULL);
  if (dir == NULL) return;
  const char *tid;
  while ((tid = g_dir_read_name(dir)) != NULL) {
    char *path = g_build_filename(taskdir, tid, "children", NULL);
    char *text = NULL;
    gboolean ok = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!ok) continue;
    /* Space-separated pids, with a trailing space. */
    char **fields = g_strsplit_set(g_strstrip(text), " \t\n", -1);
    for (int i = 0; fields[i] != NULL; i++) {
      if (fields[i][0] == '\0') continue;
      int child = (int)g_ascii_strtoll(fields[i], NULL, 10);
      if (child > 0) g_array_append_val(out, child);
    }
    g_strfreev(fields);
    g_free(text);
  }
  g_dir_close(dir);
}

/* Breadth-first so the shallowest agent wins: a pane running `claude` that
 * itself shells out to `codex` should report Claude Code, which is the one the
 * user is talking to. */
static PtAgentKind walk_descendants(int shell_pid, int *out_pid) {
  GArray *level = g_array_new(FALSE, FALSE, sizeof(int));
  GArray *next = g_array_new(FALSE, FALSE, sizeof(int));
  PtAgentKind found = PT_AGENT_NONE;
  int found_pid = 0;
  int visited = 0;

  collect_children(shell_pid, level);
  for (int depth = 0; depth < PT_AGENT_MAX_DEPTH && level->len > 0 &&
                      found == PT_AGENT_NONE;
       depth++) {
    for (guint i = 0; i < level->len && found == PT_AGENT_NONE; i++) {
      if (visited++ >= PT_AGENT_MAX_VISIT) break;
      int pid = g_array_index(level, int, i);
      char *comm = read_comm(pid);
      PtAgentKind kind = pt_agent_kind_from_name(comm);
      /* An agent running under an interpreter shows up as the interpreter, so
       * the command line gets the second word. Only for names that could hide
       * one — a `cc` or a `git` under here is not an agent under a different
       * name, and reading its cmdline would be work for nothing. */
      if (kind == PT_AGENT_NONE && comm != NULL && is_runtime(comm))
        kind = read_cmdline_kind(pid);
      g_free(comm);
      if (kind != PT_AGENT_NONE) {
        found = kind;
        found_pid = pid;
        break;
      }
      collect_children(pid, next);
    }
    if (visited >= PT_AGENT_MAX_VISIT) break;
    GArray *swap = level;
    level = next;
    next = swap;
    g_array_set_size(next, 0);
  }
  g_array_free(level, TRUE);
  g_array_free(next, TRUE);
  if (out_pid != NULL) *out_pid = found_pid;
  return found;
}

PtAgentKind pt_agent_detect(int shell_pid, const char *fg_name, int *out_pid) {
  if (out_pid != NULL) *out_pid = 0;
  PtAgentKind fast = pt_agent_kind_from_name(fg_name);
  if (fast != PT_AGENT_NONE) return fast;
  if (shell_pid <= 0) return PT_AGENT_NONE;
  return walk_descendants(shell_pid, out_pid);
}
