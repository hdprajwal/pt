#include "pt-term-core.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

struct PtTermCore {
  GhosttyTerminal terminal;
  GhosttyRenderState render_state;
  GhosttyRenderStateRowIterator row_iter;
  GhosttyRenderStateRowCells row_cells;
  GhosttyKeyEncoder key_encoder;
  GhosttyKeyEvent key_event;

  int pty_fd;
  pid_t child;
  guint fd_source;
  guint child_source;
  guint16 cols, rows;
  int cell_w, cell_h;

  gboolean eof;
  gboolean child_exited;
  int exit_status;

  PtTermCoreCallbacks cbs;
  gpointer cbs_user;
};

/* ---- pty write (non-blocking best effort, as in ghostling) ---- */
static void pty_write_raw(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, buf, len);
    if (n > 0) { buf += n; len -= (size_t)n; }
    else if (n < 0) { if (errno == EINTR) continue; break; }
  }
}

/* ---- effects: VT queries that need responses (see ghostling) ---- */
static void effect_write_pty(GhosttyTerminal t, void *ud,
                             const uint8_t *data, size_t len) {
  (void)t;
  PtTermCore *c = ud;
  pty_write_raw(c->pty_fd, (const char *)data, len);
}

static bool effect_size(GhosttyTerminal t, void *ud,
                        GhosttySizeReportSize *out) {
  (void)t;
  PtTermCore *c = ud;
  out->rows = c->rows;
  out->columns = c->cols;
  out->cell_width = (uint32_t)c->cell_w;
  out->cell_height = (uint32_t)c->cell_h;
  return true;
}

static bool effect_device_attributes(GhosttyTerminal t, void *ud,
                                     GhosttyDeviceAttributes *out) {
  (void)t; (void)ud;
  out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
  out->primary.features[0] = GHOSTTY_DA_FEATURE_COLUMNS_132;
  out->primary.features[1] = GHOSTTY_DA_FEATURE_SELECTIVE_ERASE;
  out->primary.features[2] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
  out->primary.num_features = 3;
  out->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
  out->secondary.firmware_version = 1;
  out->secondary.rom_cartridge = 0;
  out->tertiary.unit_id = 0;
  return true;
}

static GhosttyString effect_xtversion(GhosttyTerminal t, void *ud) {
  (void)t; (void)ud;
  return (GhosttyString){ .ptr = (const uint8_t *)"pt", .len = 2 };
}

static void effect_title_changed(GhosttyTerminal t, void *ud) {
  PtTermCore *c = ud;
  if (c->cbs.title == NULL) return;
  GhosttyString title = {0};
  if (ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_TITLE, &title) !=
      GHOSTTY_SUCCESS)
    return;
  char buf[256];
  size_t len = title.len < sizeof(buf) - 1 ? title.len : sizeof(buf) - 1;
  memcpy(buf, title.ptr, len);
  buf[len] = '\0';
  c->cbs.title(c, buf, c->cbs_user);
}

/* ---- child + fd sources ---- */
static void on_child_exited(GPid pid, gint wait_status, gpointer ud) {
  PtTermCore *c = ud;
  (void)pid;
  c->child_exited = TRUE;
  c->child_source = 0;
  if (WIFEXITED(wait_status)) c->exit_status = WEXITSTATUS(wait_status);
  else if (WIFSIGNALED(wait_status)) c->exit_status = 128 + WTERMSIG(wait_status);
  else c->exit_status = -1;
  if (c->cbs.exited != NULL) c->cbs.exited(c, c->exit_status, c->cbs_user);
}

static gboolean on_pty_readable(gint fd, GIOCondition cond, gpointer ud) {
  PtTermCore *c = ud;
  gboolean got_data = FALSE;
  if (cond & (G_IO_IN | G_IO_HUP)) {
    uint8_t buf[4096];
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n > 0) {
        ghostty_terminal_vt_write(c->terminal, buf, (size_t)n);
        got_data = TRUE;
      } else if (n == 0) { c->eof = TRUE; break; }
      else {
        if (errno == EAGAIN) break;
        if (errno == EINTR) continue;
        c->eof = TRUE;      /* EIO on Linux when the slave side closes */
        break;
      }
    }
  }
  if (got_data && c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
  if (c->eof) { c->fd_source = 0; return G_SOURCE_REMOVE; }
  return G_SOURCE_CONTINUE;
}

/* ---- spawn ---- */
static int spawn_pty(const char *cwd, const char *const *argv,
                     guint16 cols, guint16 rows, int cell_w, int cell_h,
                     pid_t *child_out) {
  struct winsize ws = {
    .ws_row = rows, .ws_col = cols,
    .ws_xpixel = (unsigned short)(cols * cell_w),
    .ws_ypixel = (unsigned short)(rows * cell_h),
  };
  int fd;
  pid_t child = forkpty(&fd, NULL, NULL, &ws);
  if (child < 0) return -1;
  if (child == 0) {
    if (cwd != NULL) { if (chdir(cwd) != 0) { /* fall through to $HOME */ chdir(g_get_home_dir()); } }
    setenv("TERM", "xterm-256color", 1);
    if (argv != NULL) {
      execvp(argv[0], (char *const *)argv);
    } else {
      const char *shell = getenv("SHELL");
      if (shell == NULL || shell[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        shell = (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0] != '\0')
                    ? pw->pw_shell : "/bin/sh";
      }
      const char *name = strrchr(shell, '/');
      name = name != NULL ? name + 1 : shell;
      execl(shell, name, NULL);
    }
    _exit(127);
  }
  int flags = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  *child_out = child;
  return fd;
}

PtTermCore *pt_term_core_new(const char *cwd, const char *const *argv,
                             guint16 cols, guint16 rows,
                             int cell_w, int cell_h, GError **error) {
  PtTermCore *c = g_new0(PtTermCore, 1);
  c->cols = cols; c->rows = rows; c->cell_w = cell_w; c->cell_h = cell_h;
  c->pty_fd = -1;

  GhosttyTerminalOptions opts = { .cols = cols, .rows = rows,
                                  .max_scrollback = 10000 };
  if (ghostty_terminal_new(NULL, &c->terminal, opts) != GHOSTTY_SUCCESS ||
      ghostty_render_state_new(NULL, &c->render_state) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_iterator_new(NULL, &c->row_iter) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_cells_new(NULL, &c->row_cells) != GHOSTTY_SUCCESS ||
      ghostty_key_encoder_new(NULL, &c->key_encoder) != GHOSTTY_SUCCESS ||
      ghostty_key_event_new(NULL, &c->key_event) != GHOSTTY_SUCCESS) {
    g_set_error(error, g_quark_from_static_string("pt-term-core"), 1,
                "libghostty-vt object creation failed");
    pt_term_core_free(c);
    return NULL;
  }
  ghostty_terminal_resize(c->terminal, cols, rows,
                          (uint32_t)cell_w, (uint32_t)cell_h);

  c->pty_fd = spawn_pty(cwd, argv, cols, rows, cell_w, cell_h, &c->child);
  if (c->pty_fd < 0) {
    g_set_error(error, g_quark_from_static_string("pt-term-core"), 2,
                "forkpty failed: %s", g_strerror(errno));
    pt_term_core_free(c);
    return NULL;
  }

  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_USERDATA, c);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                       (const void *)effect_write_pty);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_SIZE,
                       (const void *)effect_size);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                       (const void *)effect_device_attributes);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_XTVERSION,
                       (const void *)effect_xtversion);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                       (const void *)effect_title_changed);

  c->fd_source = g_unix_fd_add(c->pty_fd, G_IO_IN | G_IO_HUP,
                               on_pty_readable, c);
  c->child_source = g_child_watch_add(c->child, on_child_exited, c);
  return c;
}

void pt_term_core_set_callbacks(PtTermCore *c, const PtTermCoreCallbacks *cbs,
                                gpointer user) {
  c->cbs = *cbs;
  c->cbs_user = user;
}

void pt_term_core_resize(PtTermCore *c, guint16 cols, guint16 rows,
                         int cell_w, int cell_h) {
  if (cols < 1 || rows < 1) return;
  c->cols = cols; c->rows = rows; c->cell_w = cell_w; c->cell_h = cell_h;
  ghostty_terminal_resize(c->terminal, cols, rows,
                          (uint32_t)cell_w, (uint32_t)cell_h);
  struct winsize ws = {
    .ws_row = rows, .ws_col = cols,
    .ws_xpixel = (unsigned short)(cols * cell_w),
    .ws_ypixel = (unsigned short)(rows * cell_h),
  };
  if (c->pty_fd >= 0) ioctl(c->pty_fd, TIOCSWINSZ, &ws);
}

void pt_term_core_write(PtTermCore *c, const char *buf, gssize len) {
  if (c->pty_fd < 0 || c->child_exited) return;
  pty_write_raw(c->pty_fd, buf, len < 0 ? strlen(buf) : (size_t)len);
}

gboolean pt_term_core_send_key(PtTermCore *c, GhosttyKey key,
                               GhosttyKeyAction action, GhosttyMods mods,
                               guint32 unshifted_cp,
                               const char *utf8, gsize utf8_len) {
  if (c->child_exited) return FALSE;
  ghostty_key_encoder_setopt_from_terminal(c->key_encoder, c->terminal);
  ghostty_key_event_set_key(c->key_event, key);
  ghostty_key_event_set_action(c->key_event, action);
  ghostty_key_event_set_mods(c->key_event, mods);
  ghostty_key_event_set_unshifted_codepoint(c->key_event, unshifted_cp);
  GhosttyMods consumed = 0;
  if (unshifted_cp != 0 && (mods & GHOSTTY_MODS_SHIFT))
    consumed |= GHOSTTY_MODS_SHIFT;
  ghostty_key_event_set_consumed_mods(c->key_event, consumed);
  ghostty_key_event_set_utf8(c->key_event,
                             utf8_len > 0 ? utf8 : NULL, utf8_len);
  char buf[128];
  size_t written = 0;
  GhosttyResult res = ghostty_key_encoder_encode(c->key_encoder, c->key_event,
                                                 buf, sizeof(buf), &written);
  if (res == GHOSTTY_SUCCESS && written > 0) {
    pty_write_raw(c->pty_fd, buf, written);
    return TRUE;
  }
  /* Fallback: raw text (e.g. IM-composed input with no matching key). */
  if (utf8_len > 0 && action != GHOSTTY_KEY_ACTION_RELEASE) {
    pty_write_raw(c->pty_fd, utf8, utf8_len);
    return TRUE;
  }
  return FALSE;
}

void pt_term_core_scroll_delta(PtTermCore *c, int rows) {
  GhosttyTerminalScrollViewport sv = {
    .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
    .value = { .delta = rows },
  };
  ghostty_terminal_scroll_viewport(c->terminal, sv);
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
}

gboolean pt_term_core_mouse_tracking(PtTermCore *c) {
  bool tracking = false;
  ghostty_terminal_get(c->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                       &tracking);
  return tracking;
}

gboolean pt_term_core_bracketed_paste(PtTermCore *c) {
  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_BRACKETED_PASTE, &on);
  return on;
}

void pt_term_core_sync(PtTermCore *c) {
  ghostty_render_state_update(c->render_state, c->terminal);
}

GhosttyTerminal pt_term_core_terminal(PtTermCore *c) { return c->terminal; }
GhosttyRenderState pt_term_core_render_state(PtTermCore *c) { return c->render_state; }
GhosttyRenderStateRowIterator pt_term_core_row_iter(PtTermCore *c) { return c->row_iter; }
GhosttyRenderStateRowCells pt_term_core_row_cells(PtTermCore *c) { return c->row_cells; }

static int utf8_encode_cp(guint32 cp, char out[4]) {
  if (cp > 0x10FFFF) cp = 0xFFFD;
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

char *pt_term_core_grid_text(PtTermCore *c) {
  GString *out = g_string_new(NULL);
  GhosttyRenderStateRowIterator iter = c->row_iter;
  if (ghostty_render_state_get(c->render_state,
          GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &iter) != GHOSTTY_SUCCESS)
    return g_string_free(out, FALSE);
  while (ghostty_render_state_row_iterator_next(iter)) {
    GhosttyRenderStateRowCells cells = c->row_cells;
    if (ghostty_render_state_row_get(iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells) != GHOSTTY_SUCCESS)
      continue;
    gsize row_start = out->len;
    while (ghostty_render_state_row_cells_next(cells)) {
      uint32_t glen = 0;
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
      if (glen == 0) { g_string_append_c(out, ' '); continue; }
      uint32_t cps[16];
      uint32_t n = glen < 16 ? glen : 16;
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      for (uint32_t i = 0; i < n; i++) {
        char u8[4];
        g_string_append_len(out, u8, utf8_encode_cp(cps[i], u8));
      }
    }
    /* trim trailing spaces on the row */
    while (out->len > row_start && out->str[out->len - 1] == ' ')
      g_string_truncate(out, out->len - 1);
    g_string_append_c(out, '\n');
  }
  return g_string_free(out, FALSE);
}

gboolean pt_term_core_exited(PtTermCore *c, int *status) {
  if (status != NULL) *status = c->exit_status;
  return c->child_exited;
}

pid_t pt_term_core_shell_pid(PtTermCore *c) { return c->child; }

void pt_term_core_free(PtTermCore *c) {
  if (c == NULL) return;
  if (c->fd_source != 0) g_source_remove(c->fd_source);
  if (c->child_source != 0) g_source_remove(c->child_source);
  if (c->pty_fd >= 0) close(c->pty_fd);
  if (c->child > 0 && !c->child_exited) {
    kill(c->child, SIGHUP);
    waitpid(c->child, NULL, 0);
  }
  if (c->key_event != NULL) ghostty_key_event_free(c->key_event);
  if (c->key_encoder != NULL) ghostty_key_encoder_free(c->key_encoder);
  if (c->row_cells != NULL) ghostty_render_state_row_cells_free(c->row_cells);
  if (c->row_iter != NULL) ghostty_render_state_row_iterator_free(c->row_iter);
  if (c->render_state != NULL) ghostty_render_state_free(c->render_state);
  if (c->terminal != NULL) ghostty_terminal_free(c->terminal);
  g_free(c);
}
