/* pt agent-report — the one command pt's agent integrations run.
 *   pt agent-report claude                    SessionStart hook JSON on stdin
 *   pt agent-report claude-event <name>       Stop/Notification hook JSON on stdin
 *   pt agent-report codex-notify              notify JSON as last argv (stdin fallback)
 * Writes the report file $PT_PANE_TOKEN names. Returns 0 on "not applicable":
 * a hook that fails would break the agent it reports on, and a missing
 * report only costs one resume. 2 is reserved for a write that was asked for
 * and failed, which is the only case worth debugging. */
#include "pt-agent-report.h"
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

/* Claude hook event name → what pt tells the user. Only the two lifecycle
 * events carry a meaning; anything else (SessionStart, PreToolUse, a name a
 * newer Claude Code invented) reports without an event, which is exactly what
 * the plain `claude` mode writes. */
static PtAgentEvent claude_event_from_name(const char *name) {
  if (g_strcmp0(name, "Stop") == 0)
    return PT_AGENT_EVENT_TURN_COMPLETE;
  if (g_strcmp0(name, "Notification") == 0)
    return PT_AGENT_EVENT_NEEDS_INPUT;
  return PT_AGENT_EVENT_NONE;
}

/* codex spells the reason in the payload's "type". The turn-complete shape is
 * documented; approval requests arrive as their own type whose name carries
 * "approval" ("approval-requested", "apply_patch_approval_request", …), and
 * substring matching keeps that working across whatever they call each one.
 * Anything else — or no type at all — is a plain report. */
static PtAgentEvent codex_event_from_payload(JsonObject *o) {
  const char *type = pt_json_string(o, "type");
  if (type == NULL) return PT_AGENT_EVENT_NONE;
  if (strstr(type, "turn-complete") != NULL)
    return PT_AGENT_EVENT_TURN_COMPLETE;
  if (strstr(type, "approval") != NULL) return PT_AGENT_EVENT_NEEDS_INPUT;
  return PT_AGENT_EVENT_NONE;
}

int pt_agent_report_cli(int argc, char *argv[]) {
  /* argv[1] is "agent-report", so the mode is argv[2] and any payload word
   * codex appends comes after it. claude-event names its hook event in
   * argv[3] and takes the payload on stdin like plain claude. */
  const char *mode = argc >= 3 ? argv[2] : NULL;
  const char *token = g_getenv("PT_PANE_TOKEN");
  if (mode == NULL || token == NULL || *token == '\0') return 0;

  PtAgentKind kind = PT_AGENT_NONE;
  PtAgentEvent event = PT_AGENT_EVENT_NONE;
  char *payload = NULL;
  if (g_strcmp0(mode, "claude") == 0) {
    kind = PT_AGENT_CLAUDE;
    payload = read_stdin();
  } else if (g_strcmp0(mode, "claude-event") == 0 && argc >= 4) {
    kind = PT_AGENT_CLAUDE;
    event = claude_event_from_name(argv[3]);
    payload = read_stdin();
  } else if (g_strcmp0(mode, "codex-notify") == 0) {
    kind = PT_AGENT_CODEX;
    payload = argc >= 4 ? g_strdup(argv[argc - 1]) : read_stdin();
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
  if (kind == PT_AGENT_CODEX) event = codex_event_from_payload(o);
  /* Before the write, not after: report_write refuses an empty id with an
   * error, and this case is a noop rather than a failure — a SessionStart for
   * something that carries no session, or a payload shaped another way. */
  if (session == NULL || *session == '\0') {
    g_object_unref(p); g_free(payload);
    return 0;
  }

  /* The pid pt validates against is the agent's own. The agent spawned the
   * shell that spawned this command, so it is up the ancestry; when the walk
   * finds nothing (sandboxing, future spawn changes), fall back to the
   * direct parent — validation will simply fail closed at save time. */
  PtAgentKind walked = PT_AGENT_NONE;
  int pid = pt_agent_session_find_agent_ancestor(&walked);
  if (pid <= 0 || walked != kind) pid = (int)getppid();

  char *path = pt_agent_session_report_path(token);
  GError *err = NULL;
  gboolean ok = pt_agent_session_report_write(path, kind, session,
                                              cwd != NULL ? cwd : "", pid,
                                              event, &err);
  if (!ok) fprintf(stderr, "pt agent-report: %s\n",
                   err != NULL ? err->message : "write failed");
  g_clear_error(&err);
  g_free(path);
  g_object_unref(p); g_free(payload);
  return ok ? 0 : 2;
}
