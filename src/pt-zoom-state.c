#include "pt-zoom-state.h"

PtZoomToggleResult pt_zoom_state_toggle(PtZoomState *z, gboolean can_zoom) {
  if (!can_zoom || z->ratio_restores > 0) return PT_ZOOM_TOGGLE_IGNORED;
  if (z->zoomed) {
    pt_zoom_state_leave(z);
    return PT_ZOOM_TOGGLE_LEAVE;
  }
  z->zoomed = TRUE;
  return PT_ZOOM_TOGGLE_ENTER;
}

gboolean pt_zoom_state_leave(PtZoomState *z) {
  if (!z->zoomed) return FALSE;
  z->zoomed = FALSE;
  return TRUE;
}

void pt_zoom_state_restore_done(PtZoomState *z) {
  if (z->ratio_restores > 0) z->ratio_restores--;
}

gboolean pt_zoom_state_busy(const PtZoomState *z) {
  return z->ratio_restores > 0;
}

gboolean pt_zoom_state_active(const PtZoomState *z) { return z->zoomed; }

gboolean pt_zoom_state_sync_suspended(const PtZoomState *z) {
  return z->zoomed || z->ratio_restores > 0;
}
