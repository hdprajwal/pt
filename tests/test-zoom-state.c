#include "pt-zoom-state.h"

/* The pane zoom state machine lives in pt-pane-grid.c, but its decisions are
 * pure flag bookkeeping (pt-zoom-state.c) — these tests pin the transitions
 * that keep un-zoom re-entry from arming a second restore sweep against the
 * pending counter. The widget side of each step is noted per test: toggle
 * ENTER hides toward the focused leaf, LEAVE/leave shows everything and arms
 * one tick per paned (arm_ratio_restore), and each frame runs the armed
 * ticks (restore_ratio_tick). */

static void test_toggle_enter_leave_roundtrip(void) {
  PtZoomState z = {0};
  g_assert_cmpint(pt_zoom_state_toggle(&z, FALSE), ==, PT_ZOOM_TOGGLE_IGNORED);
  g_assert_false(pt_zoom_state_active(&z));

  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_ENTER);
  g_assert_true(pt_zoom_state_active(&z));
  g_assert_true(pt_zoom_state_sync_suspended(&z)); /* hidden: no sync */

  /* Leave ends zoom at once — this is what makes the statusline chip drop
   * immediately instead of waiting out the restore ticks. */
  g_assert_true(pt_zoom_state_leave(&z));
  g_assert_false(pt_zoom_state_active(&z));
}

static void test_double_toggle_inside_restore_window(void) {
  PtZoomState z = {0};
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_ENTER);

  /* First un-zoom (key press): zoom ends, two paneds arm a tick each
   * (arm_ratio_restore bumps the counter once per paned). */
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_LEAVE);
  g_assert_false(pt_zoom_state_active(&z));
  z.ratio_restores += 2;
  pt_zoom_state_restore_done(&z); /* first paned's tick landed */
  g_assert_true(pt_zoom_state_busy(&z));
  g_assert_true(pt_zoom_state_sync_suspended(&z));

  /* Key repeat lands before the next frame runs the remaining tick: ignored,
   * so no second sweep can be armed against the pending counter. */
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_IGNORED);
  g_assert_cmpuint(z.ratio_restores, ==, 1);

  /* Last tick drains the counter and sync resumes. */
  pt_zoom_state_restore_done(&z);
  g_assert_false(pt_zoom_state_busy(&z));
  g_assert_false(pt_zoom_state_sync_suspended(&z));
  /* And only now can zoom start again. */
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_ENTER);
}

static void test_unzoom_api_during_restore_is_noop(void) {
  PtZoomState z = {0};
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_ENTER);
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_LEAVE);
  z.ratio_restores = 2;

  /* pt_pane_grid_unzoom from a focus/tab switch mid-restore: leave() refuses
   * when zoom is already off, so the caller never re-arms. */
  g_assert_false(pt_zoom_state_leave(&z));
  g_assert_cmpuint(z.ratio_restores, ==, 2);
  g_assert_false(pt_zoom_state_active(&z));
}

static void test_null_root_arms_nothing(void) {
  PtZoomState z = {0};
  g_assert_cmpint(pt_zoom_state_toggle(&z, TRUE), ==, PT_ZOOM_TOGGLE_ENTER);
  /* NULL root (or a lone terminal — anything arm_ratio_restore skips):
   * leave still ends zoom, nothing pends afterwards, state is clean. */
  g_assert_true(pt_zoom_state_leave(&z));
  g_assert_false(pt_zoom_state_active(&z));
  g_assert_cmpuint(z.ratio_restores, ==, 0);
  g_assert_false(pt_zoom_state_busy(&z));
  g_assert_false(pt_zoom_state_sync_suspended(&z));
}

static void test_starved_tick_bails_out(void) {
  /* A paned that never gets an allocation (window minimized through the
   * whole restore) must stop waiting after a bounded run of frames instead
   * of continuing forever — forever would wedge the pending counter up and
   * divider syncing off for good. */
  for (guint i = 1; i < PT_ZOOM_RESTORE_MAX_STARVED_TICKS; i++)
    g_assert_false(pt_zoom_state_tick_starved(i));
  g_assert_true(
      pt_zoom_state_tick_starved(PT_ZOOM_RESTORE_MAX_STARVED_TICKS));
  /* And the bail-out counts as done, same as a successful write. */
  PtZoomState z = {.ratio_restores = 1};
  pt_zoom_state_restore_done(&z);
  g_assert_false(pt_zoom_state_busy(&z));
}

static void test_torn_down_tick_still_drains(void) {
  /* A paned torn down mid-restore has no node to write, but its tick must
   * still count as done or the counter wedges. */
  PtZoomState z = {.ratio_restores = 3};
  pt_zoom_state_restore_done(&z);
  pt_zoom_state_restore_done(&z);
  pt_zoom_state_restore_done(&z);
  pt_zoom_state_restore_done(&z); /* over-drain is a no-op */
  g_assert_cmpuint(z.ratio_restores, ==, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/zoom-state/toggle-roundtrip",
                  test_toggle_enter_leave_roundtrip);
  g_test_add_func("/zoom-state/double-toggle-in-restore-window",
                  test_double_toggle_inside_restore_window);
  g_test_add_func("/zoom-state/unzoom-during-restore-noop",
                  test_unzoom_api_during_restore_is_noop);
  g_test_add_func("/zoom-state/null-root-arms-nothing",
                  test_null_root_arms_nothing);
  g_test_add_func("/zoom-state/starved-tick-bails-out",
                  test_starved_tick_bails_out);
  g_test_add_func("/zoom-state/torn-down-tick-still-drains",
                  test_torn_down_tick_still_drains);
  return g_test_run();
}
