#pragma once
#include <glib.h>

/* Pane zoom bookkeeping, factored out of pt-pane-grid.c's widget walks so the
 * state machine itself is unit-testable headless (tests/test-zoom-state.c).
 * Two fields tell the whole story:
 *
 *   zoomed          the focused pane fills the grid; other panes are hidden.
 *   ratio_restores  un-zoom restore ticks still pending. Counted up when a
 *                   leave arms one tick per paned, down one per tick as each
 *                   paned gets its saved divider back. Divider-position
 *                   syncing stays suspended while either field is live.
 *
 * `zoomed` clears the moment a leave starts — not when the last tick lands —
 * so re-entry inside the restore window (key repeat, another unzoom from a
 * focus move) finds a quiet state and cannot arm a second sweep against the
 * pending counter. */
typedef struct {
  gboolean zoomed;
  guint ratio_restores;
} PtZoomState;

typedef enum {
  PT_ZOOM_TOGGLE_IGNORED, /* nothing to zoom, or a restore already running */
  PT_ZOOM_TOGGLE_ENTER,   /* panes hidden; focused pane fills the grid */
  PT_ZOOM_TOGGLE_LEAVE,   /* restore started; caller shows widgets and arms */
} PtZoomToggleResult;

/* How many consecutive unallocated frames one restore tick tolerates before
 * it gives up and finishes best-effort instead of coming back next frame. A
 * window minimized for the whole restore never allocates its paneds; without
 * the bound the pending counter would never drain and syncing would stay
 * suspended forever. Roughly a second of frames. */
#define PT_ZOOM_RESTORE_MAX_STARVED_TICKS 60

/* Apply a zoom toggle to `z`. `can_zoom` is FALSE for an empty or single-pane
 * grid (nothing to fill). Mid-restore — ratio_restores > 0 — the toggle is
 * ignored either way: entering would hide panes while a sweep is running, and
 * leaving again would double-arm against the pending counter. On LEAVE the
 * caller must show every pane and arm the restore ticks; on ENTER, hide
 * toward the focused leaf. */
PtZoomToggleResult pt_zoom_state_toggle(PtZoomState *z, gboolean can_zoom);

/* Begin leaving zoom: `zoomed` clears immediately (the statusline chip follows
 * at once), then the caller shows every pane and arms one pending restore per
 * paned. Returns FALSE when zoom was not active — notably mid-restore
 * re-entry, which must not touch the pending counter. */
gboolean pt_zoom_state_leave(PtZoomState *z);

/* One restore tick landed (or gave up): the paned it served is done. */
void pt_zoom_state_restore_done(PtZoomState *z);

gboolean pt_zoom_state_busy(const PtZoomState *z);    /* restore in flight */
gboolean pt_zoom_state_active(const PtZoomState *z);  /* what the chip shows */
gboolean pt_zoom_state_sync_suspended(const PtZoomState *z);
