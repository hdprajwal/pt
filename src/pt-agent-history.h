/* pt-agent-history.h — recent agent sessions, read back from the report
 * directory. Panes keep their reports current while an agent runs there and
 * swept after a week, so what this module answers is "which conversations ran
 * here lately", never a full history. Pure C: no widgets, headless-testable. */
#pragma once
#include <glib.h>
#include "pt-agent.h"

typedef struct {
  PtAgentKind agent;
  char *session_id;
  char *cwd;        /* the report's one optional field; NULL when absent */
  GDateTime *ts;    /* from the ISO8601 string; NULL when missing or
                       unparseable — such an entry sorts oldest */
} PtAgentHistoryEntry;

void pt_agent_history_entry_free(PtAgentHistoryEntry *e);

/* Every readable report in `dir`, newest first. Files that fail the report
 * gate (missing, malformed, unknown agent, hostile session id) are skipped
 * silently; a missing or unreadable directory yields an empty array. The
 * caller owns the array and everything in it. */
GPtrArray *pt_agent_history_load(const char *dir);

/* "now", "Nm ago", "Nh ago" or "Nd ago" for how long before `now` `ts` sits.
 * A ts in the future (clock skew) reads as "now"; either argument NULL gives
 * "". Caller frees. */
char *pt_agent_history_relative_time(GDateTime *ts, GDateTime *now);
