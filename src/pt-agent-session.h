/* pt-agent-session.h — the report contract between agent-side integrations
 * and pt. An integration running inside an agent writes one JSON file named
 * by the pane's PT_PANE_TOKEN; pt reads it at save time. File-drop on
 * purpose: it survives pt crashing, needs no listener, and moves onto ptd's
 * socket later without the integrations changing shape. */
#pragma once
#include <glib.h>
#include "pt-agent.h"

#define PT_AGENT_REPORT_VERSION 1

/* Why the agent reported, beyond "it is running". A lifecycle report carries
 * one of these so pt can raise a desktop notification when it lands while the
 * pane is not being watched. NONE is both "no event" and "an event name this
 * build never saw" — an unknown name must read as absence, because a report
 * written by a newer integration still has to resume. */
typedef enum {
  PT_AGENT_EVENT_NONE = 0,
  PT_AGENT_EVENT_TURN_COMPLETE,   /* the agent finished its turn */
  PT_AGENT_EVENT_NEEDS_INPUT,     /* permission prompt / waiting for input */
} PtAgentEvent;

const char *pt_agent_session_event_name(PtAgentEvent ev);
PtAgentEvent pt_agent_session_event_from_name(const char *name);

/* Machine names ("claude", "codex") — what report files and state.json
 * spell. pt_agent_label() is for humans; this one is a wire format and
 * never changes spelling. PT_AGENT_NONE and unknown names map to NULL/NONE. */
const char *pt_agent_session_kind_name(PtAgentKind kind);
PtAgentKind pt_agent_session_kind_from_name(const char *name);

char *pt_agent_session_dir(void);   /* $XDG_STATE_HOME/pt/agent-sessions, created 0700; caller frees */
char *pt_agent_session_report_path(const char *token);      /* caller frees */
char *pt_agent_session_token_new(void);  /* 16 lowercase hex chars; caller frees */

typedef struct {
  PtAgentKind agent;
  char *session_id;
  char *cwd;
  int pid;          /* the agent process itself, not the hook that reported */
  PtAgentEvent event;   /* NONE when the report carried no lifecycle event */
} PtAgentSessionReport;

/* Atomic (temp + rename via g_file_set_contents). Creates the directory.
 * `event` != NONE adds the "event" member — a lifecycle report must still
 * carry every field, because it overwrites the resume-registration file. */
gboolean pt_agent_session_report_write(const char *path, PtAgentKind agent,
                                       const char *session_id, const char *cwd,
                                       int pid, PtAgentEvent event,
                                       GError **err);
/* NULL on missing file, malformed JSON, unknown agent name, empty session_id,
 * a session_id holding anything outside [A-Za-z0-9._-], a version newer than
 * PT_AGENT_REPORT_VERSION, or pid <= 0. */
PtAgentSessionReport *pt_agent_session_report_load(const char *path);
void pt_agent_session_report_free(PtAgentSessionReport *r);

/* The save-time gate: a report is only trusted when the agent it names is
 * still the agent alive in the pane — same kind, same pid. */
gboolean pt_agent_session_report_matches(const PtAgentSessionReport *r,
                                         PtAgentKind detected_kind,
                                         int detected_pid);

/* "claude --resume 'id'\n" / "codex resume 'id'\n" — newline included, so a
 * caller writes it to the pty verbatim. The id is g_shell_quote'd: it came
 * from a file on disk, and an unquoted crafted id typed into a shell would be
 * command injection. Quoting alone is not the whole defence — the line goes
 * through a pty line editor, which acts on control bytes before any shell
 * parses them — so it pairs with report_load's charset gate, which is what
 * keeps such bytes out of an id in the first place. NULL for PT_AGENT_NONE or
 * empty id. Caller frees. */
char *pt_agent_session_resume_command(PtAgentKind kind, const char *session_id);

/* Delete reports whose mtime is older than `days` — crash leftovers keyed by
 * dead tokens. Live panes rewrite theirs on every agent session change. */
void pt_agent_session_sweep(int days);

/* The nearest ancestor of this process whose cmdline names an agent, walking
 * ppid links through /proc. How `pt agent-report` finds the pid to record: a
 * hook is spawned by a shell that was spawned by the agent, so the agent is
 * up the chain. 0 when no ancestor matches. */
int pt_agent_session_find_agent_ancestor(PtAgentKind *out_kind);
