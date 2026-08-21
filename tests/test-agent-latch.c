/* Tests for the agent-notification dedupe latch. The common regression it
 * guards against: a latch keyed on the event name alone and never re-armed
 * suppresses every turn after the first, because Claude's Stop hook always
 * reports turn-complete. */
#include "pt-agent-latch.h"

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
      failures++;                                                          \
    }                                                                      \
  } while (0)

/* Turn 1 notifies; the user comes back (rearm); turn 2 is also
 * turn-complete and must notify again. Without the rearm in between the
 * second one must stay suppressed — that is what the latch is for. */
static void test_same_name_twice_with_focus_between(void) {
  PtAgentLatch *l = pt_agent_latch_new();
  const char *turn = "turn-complete";

  CHECK(pt_agent_latch_should_notify(l, "tok", turn));
  pt_agent_latch_record(l, "tok", turn);
  CHECK(!pt_agent_latch_should_notify(l, "tok", turn));

  /* focus returns to the pane → its history is forgotten */
  pt_agent_latch_rearm(l, "tok");
  CHECK(pt_agent_latch_should_notify(l, "tok", turn));
  pt_agent_latch_record(l, "tok", turn);

  /* and the latch still works after being re-armed */
  CHECK(!pt_agent_latch_should_notify(l, "tok", turn));
  pt_agent_latch_free(l);
}

/* Recording is what arms the suppression. A decision to notify that never
 * got delivered (no GtkApplication yet) records nothing and stays free to
 * fire later. */
static void test_record_only_on_delivery(void) {
  PtAgentLatch *l = pt_agent_latch_new();

  CHECK(pt_agent_latch_should_notify(l, "tok", "turn-complete"));
  /* delivery never happened → no record → still news */
  CHECK(pt_agent_latch_should_notify(l, "tok", "turn-complete"));

  pt_agent_latch_record(l, "tok", "turn-complete");
  CHECK(!pt_agent_latch_should_notify(l, "tok", "turn-complete"));
  pt_agent_latch_free(l);
}

/* Event names are data from report files; anything this build can spell is
 * latchable, and different names for one pane do not suppress each other. */
static void test_unknown_and_alternating_names(void) {
  PtAgentLatch *l = pt_agent_latch_new();

  CHECK(pt_agent_latch_should_notify(l, "tok", "some-future-event"));
  pt_agent_latch_record(l, "tok", "some-future-event");
  CHECK(!pt_agent_latch_should_notify(l, "tok", "some-future-event"));
  /* a different name is not the same news */
  CHECK(pt_agent_latch_should_notify(l, "tok", "needs-input"));
  pt_agent_latch_free(l);
}

/* Panes close and take their tokens with them; their entries go too. */
static void test_prune_on_close(void) {
  PtAgentLatch *l = pt_agent_latch_new();
  pt_agent_latch_record(l, "alive-1", "turn-complete");
  pt_agent_latch_record(l, "alive-2", "needs-input");
  pt_agent_latch_record(l, "closed", "turn-complete");
  CHECK(pt_agent_latch_count(l) == 3);

  const char *live[] = {"alive-1", "alive-2"};
  pt_agent_latch_prune(l, live, 2);
  CHECK(pt_agent_latch_count(l) == 2);
  /* survivors keep their state; the pruned token would be fresh */
  CHECK(!pt_agent_latch_should_notify(l, "alive-1", "turn-complete"));
  CHECK(pt_agent_latch_should_notify(l, "closed", "turn-complete"));

  /* pruning everything empties the table */
  pt_agent_latch_prune(l, NULL, 0);
  CHECK(pt_agent_latch_count(l) == 0);
  CHECK(pt_agent_latch_should_notify(l, "alive-1", "turn-complete"));
  pt_agent_latch_free(l);
}

int main(void) {
  test_same_name_twice_with_focus_between();
  test_record_only_on_delivery();
  test_unknown_and_alternating_names();
  test_prune_on_close();
  if (failures != 0) {
    g_printerr("%d failure(s)\n", failures);
    return 1;
  }
  g_print("agent-latch: ok\n");
  return 0;
}
