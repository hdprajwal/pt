/* pt-agent-monitor.h — the one thing the panel talks to about agent usage.
 *
 * Owns everything the three readers have in common and nothing specific to
 * any of them: which agent the pane is running, when to go and look, what the
 * last good reading was, and what went wrong last time.
 *
 * Three of its rules are worth stating up front, because they are the ones
 * that make the difference between a card you can read and a card that
 * flickers:
 *
 * - a reading survives the agent going away. The process list is polled a tick
 *   behind and reads empty for a moment on a tab switch; clearing on every
 *   flap would blink the card constantly. The section hides, the numbers stay.
 * - a reading survives a failed refresh. A network blip does not make a
 *   two-minute-old number wrong. The old reading stays on screen with the
 *   error under it.
 * - the poll runs at PT_AGENT_POLL_S and nothing shortens it. Plan limits move
 *   slowly, and a tighter loop only re-reads unchanged files and burns
 *   requests. A question that genuinely changed — a different agent, a
 *   different directory for the one reader the directory means anything to —
 *   is asked ahead of the timer, but never more often than a few seconds
 *   apart, because the directory follows keyboard focus. */
#pragma once
#include <glib.h>
#include "pt-usage.h"

/* Two minutes. Limits move over hours. */
#define PT_AGENT_POLL_S 120

/* Fires when anything the panel draws has changed. */
typedef void (*PtAgentMonitorCb)(gpointer user);

typedef struct PtAgentMonitor PtAgentMonitor;

PtAgentMonitor *pt_agent_monitor_new(PtAgentMonitorCb cb, gpointer user);
void pt_agent_monitor_free(PtAgentMonitor *m);

/* What the panel is looking at right now. Cheap enough to call on every panel
 * refresh: the detection fast path is a string compare, the /proc walk behind
 * it is rate limited, and a lookup only starts when the answer could have
 * changed. `panel_visible` FALSE stops all of it — with the panel closed
 * nothing is polled at all. */
void pt_agent_monitor_observe(PtAgentMonitor *m, gboolean panel_visible,
                              int shell_pid, const char *fg_name,
                              const char *cwd);

/* Whether Claude Code usage may be looked up, which means putting the user's
 * stored token on the wire. Off until they say so. */
void pt_agent_monitor_set_claude_enabled(PtAgentMonitor *m, gboolean on);

/* The panel's refresh button. Skips the "not due yet" check but not an active
 * rate limit — hammering a 429 only makes it last longer. */
void pt_agent_monitor_refresh(PtAgentMonitor *m);

/* Everything the panel needs to draw, borrowed from the monitor and valid
 * until the next call into it. */
typedef struct {
  PtAgentKind kind;      /* NONE: no agent in this pane, hide the section */
  const PtUsage *usage;  /* NULL when this agent has never reported */
  const char *error;     /* NULL when the last lookup succeeded */
  gboolean needs_optin;  /* Claude, and usage lookups are still off */
  gboolean busy;         /* a lookup is in flight */
} PtAgentView;

void pt_agent_monitor_view(PtAgentMonitor *m, PtAgentView *out);
