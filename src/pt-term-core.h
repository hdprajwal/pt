#pragma once
#include <glib.h>
#include <ghostty/vt.h>
#include <sys/types.h>

typedef struct PtTermCore PtTermCore;

typedef struct {
  void (*draw)(PtTermCore *core, gpointer user);   /* state changed; redraw */
  void (*exited)(PtTermCore *core, int status, gpointer user);
  void (*title)(PtTermCore *core, const char *title, gpointer user);
  void (*command)(PtTermCore *core, const char *comm, gpointer user);
} PtTermCoreCallbacks;

/* argv NULL → spawn the user's shell ($SHELL → passwd → /bin/sh). */
PtTermCore *pt_term_core_new(const char *cwd, const char *const *argv,
                             guint16 cols, guint16 rows,
                             int cell_w, int cell_h, GError **error);
void pt_term_core_set_callbacks(PtTermCore *c, const PtTermCoreCallbacks *cbs,
                                gpointer user);
void pt_term_core_resize(PtTermCore *c, guint16 cols, guint16 rows,
                         int cell_w, int cell_h);
void pt_term_core_write(PtTermCore *c, const char *buf, gssize len);
/* Returns TRUE if the encoder produced bytes (event was consumed). */
gboolean pt_term_core_send_key(PtTermCore *c, GhosttyKey key,
                               GhosttyKeyAction action, GhosttyMods mods,
                               guint32 unshifted_cp,
                               const char *utf8, gsize utf8_len);
void pt_term_core_scroll_delta(PtTermCore *c, int rows);

/* ---- mouse selection (viewport-relative pixels; PT_PAD-padded) ---- */
void pt_term_core_selection_press(PtTermCore *c, double px, double py,
                                  guint64 time_ns);
void pt_term_core_selection_drag(PtTermCore *c, double px, double py);
void pt_term_core_selection_release(PtTermCore *c, double px, double py);
void pt_term_core_selection_clear(PtTermCore *c);
char *pt_term_core_selection_text(PtTermCore *c);  /* NULL when no selection; caller g_free */

gboolean pt_term_core_mouse_tracking(PtTermCore *c);
gboolean pt_term_core_bracketed_paste(PtTermCore *c);
void pt_term_core_sync(PtTermCore *c);              /* render_state_update */
GhosttyTerminal pt_term_core_terminal(PtTermCore *c);
GhosttyRenderState pt_term_core_render_state(PtTermCore *c);
GhosttyRenderStateRowIterator pt_term_core_row_iter(PtTermCore *c);
GhosttyRenderStateRowCells pt_term_core_row_cells(PtTermCore *c);
char *pt_term_core_grid_text(PtTermCore *c);        /* visible grid, caller frees */
gboolean pt_term_core_exited(PtTermCore *c, int *status);
pid_t pt_term_core_shell_pid(PtTermCore *c);
void pt_term_core_free(PtTermCore *c);
