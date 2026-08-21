/* pt-agent-latch.h — the dedupe latch behind agent lifecycle notifications.
 *
 * A pane's report file can be rewritten many times between two interesting
 * moments, and a turn-complete for a turn the user was already told about
 * must not raise a second notification. The latch remembers, per pane token,
 * the name of the last event a notification was actually raised for: the
 * same name again is suppressed, anything else goes through.
 *
 * Two things keep the latch honest. Recording happens only after delivery —
 * an event that could not be shown (no application yet) stays unrecorded, so
 * it can still notify later. And focus re-arms: when the user looks at a pane
 * again its entry is dropped, so the NEXT turn-complete is news again rather
 * than a repeat of one they already saw. */
#pragma once
#include <glib.h>

typedef struct _PtAgentLatch PtAgentLatch;

PtAgentLatch *pt_agent_latch_new(void);
void pt_agent_latch_free(PtAgentLatch *l);

/* TRUE when an event named `event` for `token` should raise a notification:
 * it differs from whatever this token last had delivered. */
gboolean pt_agent_latch_should_notify(PtAgentLatch *l, const char *token,
                                      const char *event);
/* Record that a notification for (token, event) was delivered. Only call
 * this once the notification actually went out. */
void pt_agent_latch_record(PtAgentLatch *l, const char *token,
                           const char *event);
/* The user is looking at `token`'s pane again; forget its history so the next
 * event of any name notifies. */
void pt_agent_latch_rearm(PtAgentLatch *l, const char *token);
/* Drop every entry whose token does not appear in `live` (n_live entries) —
 * the cleanup when panes close and their tokens die with them. */
void pt_agent_latch_prune(PtAgentLatch *l, const char *const *live,
                          gsize n_live);
guint pt_agent_latch_count(PtAgentLatch *l);
