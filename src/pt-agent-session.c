#include "pt-agent-session.h"
#include "pt-json-read.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>          /* G_IO_ERROR, for the write's refusal */
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* How far up the ppid chain the ancestor walk is willing to look. A hook is
 * spawned by a shell that the agent spawned, so the agent is two or three
 * links away; the rest of the budget is for wrappers. The cap is what keeps a
 * /proc that lies about its links — a cycle, a pid recycled mid-walk — from
 * turning this into an endless loop. */
#define PT_AGENT_ANCESTOR_MAX_HOPS 32

/* Indexed by PtAgentKind, so the enum and this table have to stay in step;
 * NULL sits at PT_AGENT_NONE, which has no name to spell. */
static const char *const kind_names[] = { NULL, "claude", "codex" };

const char *pt_agent_session_kind_name(PtAgentKind kind) {
  if (kind <= PT_AGENT_NONE || (gsize)kind >= G_N_ELEMENTS(kind_names))
    return NULL;
  return kind_names[kind];
}

PtAgentKind pt_agent_session_kind_from_name(const char *name) {
  for (gsize i = 1; i < G_N_ELEMENTS(kind_names); i++)
    if (g_strcmp0(name, kind_names[i]) == 0) return (PtAgentKind)i;
  return PT_AGENT_NONE;
}

char *pt_agent_session_dir(void) {
  char *dir = g_build_filename(g_get_user_state_dir(), "pt",
                               "agent-sessions", NULL);
  /* 0700: a report names a session id that resumes a conversation, so the
   * directory is the user's alone even on a shared machine. */
  g_mkdir_with_parents(dir, 0700);
  return dir;
}

char *pt_agent_session_report_path(const char *token) {
  char *dir = pt_agent_session_dir();
  char *name = g_strconcat(token, ".json", NULL);
  char *path = g_build_filename(dir, name, NULL);
  g_free(dir);
  g_free(name);
  return path;
}

char *pt_agent_session_token_new(void) {
  /* 64 bits of it: the token is handed to the agent through the environment
   * and names a file every integration can write, so it has to be long enough
   * that no other pane's token is worth guessing at. */
  GString *s = g_string_new(NULL);
  for (gsize i = 0; i < 8; i++)
    g_string_append_printf(s, "%02x", (guint)g_random_int_range(0, 256));
  return g_string_free(s, FALSE);
}

gboolean pt_agent_session_report_write(const char *path, PtAgentKind agent,
                                       const char *session_id, const char *cwd,
                                       int pid, GError **err) {
  const char *name = pt_agent_session_kind_name(agent);
  /* Refuse up front rather than write a file that report_load would then
   * reject: a report nobody can read back is a silent failure, and the caller
   * asking for one has a bug worth hearing about. */
  if (path == NULL || name == NULL || session_id == NULL ||
      session_id[0] == '\0' || pid <= 0) {
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "incomplete agent session report");
    return FALSE;
  }

  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, PT_AGENT_REPORT_VERSION);
  json_builder_set_member_name(b, "agent");
  json_builder_add_string_value(b, name);
  json_builder_set_member_name(b, "session_id");
  json_builder_add_string_value(b, session_id);
  json_builder_set_member_name(b, "cwd");
  json_builder_add_string_value(b, cwd);
  json_builder_set_member_name(b, "pid");
  json_builder_add_int_value(b, pid);
  /* Written for a human reading the directory, and for anything later that
   * wants to sort reports by age without trusting mtime. pt itself reads the
   * file's mtime instead, which is what the sweep below acts on. */
  GDateTime *now = g_date_time_new_now_utc();
  char *ts = g_date_time_format_iso8601(now);
  json_builder_set_member_name(b, "ts");
  json_builder_add_string_value(b, ts);
  g_free(ts);
  g_date_time_unref(now);
  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  char *text = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(b);

  /* g_file_set_contents writes to a temp file and renames, so a pt reading
   * this path never sees a half-written report. */
  gboolean ok = g_file_set_contents(path, text, -1, err);
  g_free(text);
  return ok;
}

/* A session id is only ever a UUID or a CLI-generated token, so it may hold
 * letters, digits, dot, underscore and dash and nothing else.
 *
 * The gate is here rather than left to the resume command's quoting because
 * the command is not handed to a shell — it is typed into the pane's pty,
 * where the line editor sees the bytes first. Quoting stops the shell parser
 * but not the terminal: a raw 0x15 inside quotes is still ^U to readline,
 * which kills the line typed so far and lets the rest of a crafted id stand
 * on its own as a command. Refusing the whole report is the same move the
 * other rules make — a report pt cannot trust is a report pt does not use. */
static gboolean session_id_is_clean(const char *id) {
  for (const char *p = id; *p != '\0'; p++)
    if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
      return FALSE;
  return TRUE;
}

PtAgentSessionReport *pt_agent_session_report_load(const char *path) {
  /* Ask before parsing: a pane that never ran an agent has no report at all,
   * which is the common case, and json_parser_load_from_file would log about
   * every one of them. */
  if (path == NULL || !g_file_test(path, G_FILE_TEST_EXISTS)) return NULL;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_file(parser, path, NULL)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NULL;
  }
  JsonObject *o = json_node_get_object(root);

  /* Every field is read through pt-json-read, because this file was written
   * by an integration living outside pt's release cycle: a wrong type has to
   * read as "not there" rather than abort. */
  gint64 version = pt_json_int(o, "version", PT_AGENT_REPORT_VERSION);
  PtAgentKind agent = pt_agent_session_kind_from_name(pt_json_string(o, "agent"));
  const char *session_id = pt_json_string(o, "session_id");
  int pid = (int)pt_json_int(o, "pid", 0);
  /* A report from a newer pt may mean anything by these members; refuse it
   * the way a malformed one is refused instead of guessing. */
  if (version > PT_AGENT_REPORT_VERSION || agent == PT_AGENT_NONE ||
      session_id == NULL || session_id[0] == '\0' ||
      !session_id_is_clean(session_id) || pid <= 0) {
    g_object_unref(parser);
    return NULL;
  }

  PtAgentSessionReport *r = g_new0(PtAgentSessionReport, 1);
  r->agent = agent;
  r->session_id = g_strdup(session_id);
  /* cwd is the one optional field: it tells a restored pane where to resume,
   * and a caller that has the pane's own cwd can do without it. */
  r->cwd = g_strdup(pt_json_string(o, "cwd"));
  r->pid = pid;
  g_object_unref(parser);
  return r;
}

void pt_agent_session_report_free(PtAgentSessionReport *r) {
  if (r == NULL) return;
  g_free(r->session_id);
  g_free(r->cwd);
  g_free(r);
}

gboolean pt_agent_session_report_matches(const PtAgentSessionReport *r,
                                         PtAgentKind detected_kind,
                                         int detected_pid) {
  return r != NULL && detected_kind == r->agent && detected_pid > 0 &&
         detected_pid == r->pid;
}

char *pt_agent_session_resume_command(PtAgentKind kind, const char *session_id) {
  if (session_id == NULL || session_id[0] == '\0') return NULL;
  char *quoted = g_shell_quote(session_id);
  char *cmd = NULL;
  switch (kind) {
    case PT_AGENT_CLAUDE:
      cmd = g_strdup_printf("claude --resume %s\n", quoted);
      break;
    case PT_AGENT_CODEX:
      cmd = g_strdup_printf("codex resume %s\n", quoted);
      break;
    case PT_AGENT_NONE:
      break;
  }
  g_free(quoted);
  return cmd;
}

void pt_agent_session_sweep(int days) {
  /* A zero or negative age would mean "delete every report", including the
   * ones panes are using right now. Nothing asks for that. */
  if (days <= 0) return;
  char *dir = pt_agent_session_dir();
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d == NULL) {
    g_free(dir);
    return;
  }
  gint64 cutoff = g_get_real_time() / G_USEC_PER_SEC - (gint64)days * 86400;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL) {
    /* Only files this module names: the directory is pt's, but deleting
     * something that got there another way is not this function's business. */
    if (!g_str_has_suffix(name, ".json")) continue;
    char *path = g_build_filename(dir, name, NULL);
    GStatBuf st;
    if (g_stat(path, &st) == 0 && (gint64)st.st_mtime < cutoff)
      g_remove(path);
    g_free(path);
  }
  g_dir_close(d);
  g_free(dir);
}

/* The parent of `pid` per /proc/<pid>/stat, or 0 when it cannot be read.
 *
 * The fields are read from after the last ')' rather than by counting from
 * the start, because the second field is the executable name in parentheses
 * and a program is free to have spaces and parentheses in its own name. */
static int read_ppid(int pid) {
  char path[64];
  g_snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  char *buf = NULL;
  if (!g_file_get_contents(path, &buf, NULL, NULL)) return 0;
  int ppid = 0;
  const char *close = strrchr(buf, ')');
  if (close == NULL || sscanf(close + 1, " %*c %d", &ppid) != 1) ppid = 0;
  g_free(buf);
  return ppid;
}

int pt_agent_session_find_agent_ancestor(PtAgentKind *out_kind) {
  if (out_kind != NULL) *out_kind = PT_AGENT_NONE;
  int pid = (int)getppid();
  for (int hop = 0; hop < PT_AGENT_ANCESTOR_MAX_HOPS && pid > 1; hop++) {
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    char *buf = NULL;
    gsize len = 0;
    if (g_file_get_contents(path, &buf, &len, NULL)) {
      PtAgentKind kind = pt_agent_kind_from_cmdline(buf, len);
      g_free(buf);
      if (kind != PT_AGENT_NONE) {
        if (out_kind != NULL) *out_kind = kind;
        return pid;
      }
    }
    pid = read_ppid(pid);
  }
  return 0;
}
