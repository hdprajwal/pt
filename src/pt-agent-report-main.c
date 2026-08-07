/* pt-agent-report — the one program pt's agent integrations run.
 *   pt-agent-report claude         SessionStart hook JSON on stdin
 *   pt-agent-report codex-notify   notify JSON as last argv (stdin fallback)
 * Writes the report file $PT_PANE_TOKEN names. Exits 0 on "not applicable":
 * a hook that fails would break the agent it reports on, and a missing
 * report only costs one resume. Exit 2 is reserved for a write that was
 * asked for and failed, which is the only case worth debugging. */
#include "pt-agent-session.h"
#include "pt-json-read.h"
#include <json-glib/json-glib.h>
#include <unistd.h>
#include <stdio.h>

static char *read_stdin(void) {
  GString *s = g_string_new(NULL);
  char buf[4096];
  gssize n;
  while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0)
    g_string_append_len(s, buf, n);
  return g_string_free(s, FALSE);
}

/* The conversation reference to hand `claude --resume` / `codex resume`.
 *
 * codex's notify payload spells it "thread-id" (verified: `codex resume
 * <thread-id>` reopens the conversation); "session_id" is the fallback in
 * case a later codex renames it to match claude's hook. Both are read through
 * pt-json-read, which answers NULL for a member that is absent or has turned
 * into some other type in a version of the agent this build never saw. */
static const char *session_ref(JsonObject *o, PtAgentKind kind) {
  const char *s = NULL;
  if (kind == PT_AGENT_CODEX) s = pt_json_string(o, "thread-id");
  if (s == NULL || *s == '\0') s = pt_json_string(o, "session_id");
  return s;
}

int main(int argc, char *argv[]) {
  const char *mode = argc >= 2 ? argv[1] : NULL;
  const char *token = g_getenv("PT_PANE_TOKEN");
  if (mode == NULL || token == NULL || *token == '\0') return 0;

  PtAgentKind kind = PT_AGENT_NONE;
  char *payload = NULL;
  if (g_strcmp0(mode, "claude") == 0) {
    kind = PT_AGENT_CLAUDE;
    payload = read_stdin();
  } else if (g_strcmp0(mode, "codex-notify") == 0) {
    kind = PT_AGENT_CODEX;
    payload = argc >= 3 ? g_strdup(argv[argc - 1]) : read_stdin();
  } else {
    return 0;
  }

  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, payload, -1, NULL) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    g_object_unref(p); g_free(payload); return 0;
  }
  JsonObject *o = json_node_get_object(json_parser_get_root(p));
  /* Borrowed from the parser, which outlives both uses below. */
  const char *session = session_ref(o, kind);
  const char *cwd = pt_json_string(o, "cwd");
  /* Before the write, not after: report_write refuses an empty id with an
   * error, and this case is a noop rather than a failure — a SessionStart for
   * something that carries no session, or a payload shaped another way. */
  if (session == NULL || *session == '\0') {
    g_object_unref(p); g_free(payload);
    return 0;
  }

  /* The pid pt validates against is the agent's own. The agent spawned the
   * shell that spawned this helper, so it is up the ancestry; when the walk
   * finds nothing (sandboxing, future spawn changes), fall back to the
   * direct parent — validation will simply fail closed at save time. */
  PtAgentKind walked = PT_AGENT_NONE;
  int pid = pt_agent_session_find_agent_ancestor(&walked);
  if (pid <= 0 || walked != kind) pid = (int)getppid();

  char *path = pt_agent_session_report_path(token);
  GError *err = NULL;
  gboolean ok = pt_agent_session_report_write(path, kind, session,
                                              cwd != NULL ? cwd : "", pid,
                                              &err);
  if (!ok) fprintf(stderr, "pt-agent-report: %s\n",
                   err != NULL ? err->message : "write failed");
  g_clear_error(&err);
  g_free(path);
  g_object_unref(p); g_free(payload);
  return ok ? 0 : 2;
}
