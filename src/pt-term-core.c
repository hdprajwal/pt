#include "pt-term-core.h"
#include "pt-term-core-internal.h"
#include "pt-status-parse.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>       /* setenv / putenv in the forked child */
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
  GhosttyMouseEncoder mouse_encoder;
  GhosttyMouseEvent mouse_event;
  guint buttons_down;       /* bitmask of held buttons, by GhosttyMouseButton */

  int pty_fd;
  pid_t child;
  guint fd_source;
  guint child_source;
  guint cmd_timer;
  char last_comm[64];
  char **env_pairs;         /* extra "KEY=VALUE" set in the child, or NULL */
  guint16 cols, rows;
  int cell_w, cell_h;

  gboolean eof;
  gboolean child_exited;
  int exit_status;
  int last_exit;            /* from the "pt-exit:<n>;" title marker; -1 = none */

  /* mouse selection (see pt_term_core_selection_*) */
  gboolean sel_active;      /* a selection is installed on the terminal */
  gboolean sel_dragging;    /* between press and release */
  gboolean sel_moved;       /* pointer left the anchor cell during the drag */
  uint16_t sel_anchor_col, sel_anchor_row;
  int click_count;          /* 1=char, 2=word, 3=line (cycles) */
  guint64 last_press_ns;
  uint16_t last_press_col, last_press_row;

  /* focus reporting (mode 1004); see pt_term_core_focus_report */
  gboolean focused;         /* last state a caller reported, mode 1004 or not */
  gboolean was_focus_event; /* mode 1004 as of the last read, for the 0->1 edge */
  gboolean was_in_band_resize; /* mode 2048, same, for the enable-time report */

  PtOscScan osc;            /* OSC scanner state, carried across reads */
  PtOsc52Mode osc52;        /* what OSC 52 may do to the clipboard */

  PtTermCoreCallbacks cbs;
  gpointer cbs_user;
};

/* Inset around the grid; mirrors PT_PAD_X / PT_PAD_Y in pt-terminal.c. */
#define PT_CORE_PAD_X 20
#define PT_CORE_PAD_Y 18

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
  GhosttyString title = {0};
  if (ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_TITLE, &title) !=
      GHOSTTY_SUCCESS)
    return;
  char buf[256];
  size_t len = title.len < sizeof(buf) - 1 ? title.len : sizeof(buf) - 1;
  if (len > 0) memcpy(buf, title.ptr, len);
  buf[len] = '\0';
  /* The prompt snippet prefixes the title with the last command's status:
   * "pt-exit:<code>;<real title>". Record it and hand on the real title.
   * Done before the callback check so the state is tracked either way. */
  int code = 0;
  const char *rest = NULL;
  if (pt_exit_marker_parse(buf, &code, &rest)) {
    c->last_exit = code;
    memmove(buf, rest, strlen(rest) + 1);
  }
  if (c->cbs.title != NULL) c->cbs.title(c, buf, c->cbs_user);
}

/* ---- foreground-command watcher ----
 * Polls the pty's foreground process group (the app the user is actually
 * interacting with, e.g. nvim under the shell) and reports its /proc/<pid>/comm
 * whenever it changes, so tabs can label themselves live. */
static gboolean poll_foreground_command(gpointer ud) {
  PtTermCore *c = ud;
  if (c->child_exited) return G_SOURCE_CONTINUE;
  pid_t fg = tcgetpgrp(c->pty_fd);
  if (fg <= 0) fg = c->child;
  char path[64];
  g_snprintf(path, sizeof(path), "/proc/%d/comm", (int)fg);
  char *comm = NULL;
  if (!g_file_get_contents(path, &comm, NULL, NULL)) return G_SOURCE_CONTINUE;
  g_strchomp(comm);              /* strip the trailing newline */
  if (comm[0] != '\0' && strcmp(comm, c->last_comm) != 0) {
    g_strlcpy(c->last_comm, comm, sizeof(c->last_comm));
    if (c->cbs.command != NULL) c->cbs.command(c, c->last_comm, c->cbs_user);
  }
  g_free(comm);
  return G_SOURCE_CONTINUE;
}

/* ---- OSC scanner (see pt-term-core-internal.h for why pt needs its own) ---- */
#define PT_ESC 0x1B
#define PT_BEL 0x07

static void osc_buf_reset(PtOscScan *s) {
  if (s->buf == NULL) return;
  g_string_truncate(s->buf, 0);
  /* One OSC 52 can leave a megabyte parked on a pane for the rest of the
   * session. Hand it back rather than keeping the high-water mark. */
  if (s->buf->allocated_len > PT_OSC_MAX) {
    g_string_free(s->buf, TRUE);
    s->buf = NULL;
  }
}

static void osc_begin(PtOscScan *s) {
  if (s->buf == NULL) s->buf = g_string_sized_new(64);
  else g_string_truncate(s->buf, 0);
  s->state = PT_OSC_PAYLOAD;
}

/* The cap depends on the code, which is the digits before the first ';' — so
 * it can only be known once that much has been buffered. Checked only after
 * the general cap is already reached, so the common path stays one compare. */
static gsize osc_cap(const GString *b) {
  return (b->len >= 3 && memcmp(b->str, "52;", 3) == 0) ? PT_OSC_52_MAX
                                                        : PT_OSC_MAX;
}

static void osc_append(PtOscScan *s, guint8 b) {
  if (G_UNLIKELY(s->buf->len >= PT_OSC_MAX && s->buf->len >= osc_cap(s->buf))) {
    /* Over budget. Keep tracking the sequence so the scanner stays in sync
     * with the stream, but throw the payload away. */
    osc_buf_reset(s);
    s->state = PT_OSC_DROP;
    return;
  }
  g_string_append_c(s->buf, (char)b);
}

static void osc_dispatch(PtOscScan *s, PtOscScanFn fn, gpointer user) {
  const GString *b = s->buf;
  const char *sep = memchr(b->str, ';', b->len);
  gsize digits = sep != NULL ? (gsize)(sep - b->str) : b->len;
  /* No digits, non-digits, or a code no OSC uses: not ours, drop it. */
  if (digits > 0 && digits <= 5 && fn != NULL) {
    int code = 0;
    gsize i;
    for (i = 0; i < digits; i++) {
      char ch = b->str[i];
      if (ch < '0' || ch > '9') break;
      code = code * 10 + (ch - '0');
    }
    if (i == digits)
      fn(code, sep != NULL ? sep + 1 : b->str + b->len,
         sep != NULL ? b->len - digits - 1 : 0, user);
  }
  osc_buf_reset(s);
  s->state = PT_OSC_GROUND;
}

/* An ESC that is not ST ends the sequence with nothing to show for it. `b` is
 * the byte that followed the ESC, and may itself start something. */
static void osc_abandon(PtOscScan *s, guint8 b) {
  osc_buf_reset(s);
  if (b == ']') osc_begin(s);
  else if (b == PT_ESC) s->state = PT_OSC_ESC;
  else s->state = PT_OSC_GROUND;
}

void pt_osc_scan_feed(PtOscScan *s, const guint8 *data, gsize len,
                      PtOscScanFn fn, gpointer user) {
  gsize i = 0;
  while (i < len) {
    if (s->state == PT_OSC_GROUND) {
      /* The hot path, and almost all of every read: with nothing in progress
       * only ESC matters, so skip straight to the next one instead of running
       * the state machine per byte. */
      const guint8 *esc = memchr(data + i, PT_ESC, len - i);
      if (esc == NULL) return;
      i = (gsize)(esc - data) + 1;
      s->state = PT_OSC_ESC;
      continue;
    }
    guint8 b = data[i++];
    switch (s->state) {
      case PT_OSC_ESC:
        if (b == ']') osc_begin(s);
        else if (b != PT_ESC) s->state = PT_OSC_GROUND;
        break;                       /* ESC ESC: keep waiting for the ']' */
      /* BEL and ESC \ are the only terminators, deliberately. A bare 0x9C is
       * payload, not single-byte ST: libghostty's osc_string table maps
       * 0x20..0xFF to osc_put, and that write lands after the "anywhere"
       * 0x9C => ground rule in genTable(), so the parser appends it too.
       * It has to — inside an OSC ghostty consumes raw bytes rather than
       * decoded codepoints, and 0x9C is a UTF-8 continuation byte (U+011C
       * is C4 9C), so terminating on it would cut payloads mid-character. */
      case PT_OSC_PAYLOAD:
        if (b == PT_BEL) osc_dispatch(s, fn, user);
        else if (b == PT_ESC) s->state = PT_OSC_PAYLOAD_ESC;
        else osc_append(s, b);
        break;
      case PT_OSC_PAYLOAD_ESC:
        if (b == '\\') osc_dispatch(s, fn, user);          /* ST */
        else osc_abandon(s, b);
        break;
      case PT_OSC_DROP:
        if (b == PT_BEL) s->state = PT_OSC_GROUND;
        else if (b == PT_ESC) s->state = PT_OSC_DROP_ESC;
        break;
      case PT_OSC_DROP_ESC:
        if (b == '\\') s->state = PT_OSC_GROUND;
        else osc_abandon(s, b);
        break;
      case PT_OSC_GROUND:
        break;                       /* handled above */
    }
  }
}

void pt_osc_scan_clear(PtOscScan *s) {
  if (s->buf != NULL) g_string_free(s->buf, TRUE);
  s->buf = NULL;
  s->state = PT_OSC_GROUND;
}

/* ---- OSC 52 clipboard writes ----
 *
 * `ESC ] 52 ; <targets> ; <base64> BEL` is how anything on the far side of an
 * ssh session copies to the local clipboard. What arrives here is a payload a
 * remote program chose, so every part of it is checked before a byte of it can
 * reach the clipboard. */

/* g_base64_decode() has no failure mode: characters outside the alphabet are
 * skipped and it returns the bytes it managed to assemble, so "not base64"
 * comes back as plausible-looking garbage rather than an error. */
static gboolean b64_valid(const char *s, gsize len) {
  if (len == 0) return FALSE;
  gsize pad = 0;
  for (gsize i = 0; i < len; i++) {
    char ch = s[i];
    if (ch == '=') { pad++; continue; }
    if (pad > 0) return FALSE;          /* padding belongs at the end, only */
    if (!g_ascii_isalnum(ch) && ch != '+' && ch != '/') return FALSE;
  }
  /* A single leftover character encodes nothing, so a group of one is broken
   * whatever the padding says. Two or three are a short but unambiguous tail:
   * emitters that leave the padding off are common enough that rejecting them
   * would just look like a clipboard that sometimes does nothing. */
  return pad <= 2 && (len - pad) % 4 != 1;
}

char *pt_osc52_decode(const char *payload, gsize len, gboolean *primary,
                      gsize *out_len) {
  const char *sep = memchr(payload, ';', len);
  if (sep == NULL) return NULL;      /* no targets/data split: not for us */
  const char *data = sep + 1;
  gsize n = len - (gsize)(data - payload);

  /* The read form. Never answered — see pt_term_core_set_osc52(). Dropping it
   * here means no callback fires and nothing is written to the pty. */
  if (n == 1 && data[0] == '?') return NULL;
  if (!b64_valid(data, n)) return NULL;
  /* Judged on the encoded length, so an oversized clipboard is refused before
   * anything is allocated: four characters carry three bytes. */
  if (n / 4 * 3 > PT_OSC_52_TEXT_MAX) return NULL;

  /* Targets can name several selections at once ("pc"). pt has one clipboard
   * and one primary selection, so the clipboard wins when both are asked for,
   * and everything else — "s", a cut-buffer digit, or no target at all, which
   * is what an emitter that names nothing means — lands on the clipboard. */
  gboolean want_primary = FALSE;
  for (const char *p = payload; p < sep; p++) {
    if (*p == 'c') { want_primary = FALSE; break; }
    if (*p == 'p') want_primary = TRUE;
  }

  /* g_base64_decode() wants a NUL-terminated string, and only flushes whole
   * four-character groups, so a payload that left its padding off gets it
   * back here rather than losing its last byte or two. */
  gsize padded_len = n % 4 == 0 ? n : n + (4 - n % 4);
  char *padded = g_malloc(padded_len + 1);
  memcpy(padded, data, n);
  memset(padded + n, '=', padded_len - n);
  padded[padded_len] = '\0';
  gsize decoded_len = 0;
  guchar *raw = g_base64_decode(padded, &decoded_len);
  g_free(padded);
  if (raw == NULL || decoded_len == 0 || decoded_len > PT_OSC_52_TEXT_MAX) {
    g_free(raw);
    return NULL;
  }
  /* An embedded NUL would leave everything past it outside every length the
   * consumers work with while the clipboard itself, which takes a C string,
   * quietly keeps only the head. */
  if (memchr(raw, '\0', decoded_len) != NULL) {
    g_free(raw);
    return NULL;
  }
  /* And the bytes have to be text. A clipboard is offered to the rest of the
   * desktop as UTF-8, so arbitrary bytes from a remote program would be
   * advertised as text and come back mangled wherever they were pasted. This
   * is the last content check rather than the first because it is the only one
   * that has to walk the whole decoded string. */
  if (!g_utf8_validate((const char *)raw, (gssize)decoded_len, NULL)) {
    g_free(raw);
    return NULL;
  }
  raw = g_realloc(raw, decoded_len + 1);   /* the decoder does not terminate */
  raw[decoded_len] = '\0';
  if (primary != NULL) *primary = want_primary;
  if (out_len != NULL) *out_len = decoded_len;
  return (char *)raw;
}

static void core_osc_dispatch(int code, const char *payload, gsize len,
                              gpointer user) {
  PtTermCore *c = user;
  /* Rechecked per dispatch, not just once per read: a single read can carry
   * several sequences, and a consumer is allowed to unregister itself from
   * inside its own handler. Without this the next sequence in the same read
   * would call through a NULL pointer. */
  if (code == 52 && c->osc52 != PT_OSC52_OFF &&
      c->cbs.clipboard_write != NULL) {
    gboolean primary = FALSE;
    gsize text_len = 0;
    char *text = pt_osc52_decode(payload, len, &primary, &text_len);
    if (text != NULL) {
      c->cbs.clipboard_write(c, text, text_len, primary, c->cbs_user);
      g_free(text);
    }
  }
  if (c->cbs.osc != NULL) c->cbs.osc(c, code, payload, len, c->cbs_user);
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

/* ---- modes a program turning on has to be answered for ----
 *
 * Ghostty reports the current focus state the instant its parser sees
 * CSI ? 1004 h (stream_handler.zig:754-756), so an editor that starts up in an
 * already-focused pane learns it is focused without the user clicking away and
 * back. libghostty-vt's C API has no mode-change callback (terminal.h exposes
 * modes through ghostty_terminal_mode_get only), so pt watches the mode for a
 * 0->1 edge once per read batch instead and reports on it. Same observable
 * behaviour, one read of a bitfield per batch of pty bytes. Mode 2048 wants
 * the same answer for the same reason, so it shares the poll below. */

/* An unsolicited mode-2048 report for the size the pane has right now.
 *
 * The resize path never needs this: ghostty_terminal_resize() checks mode 2048
 * and writes the report through the write_pty effect itself
 * (terminal/c/terminal.zig:505-519), so pt gets it for free. This is only for
 * the enable-time report below, which has no resize to hang off. Calling
 * ghostty_terminal_resize() with the size the terminal already has would emit
 * one — the library does not dedupe — but it also clears synchronized output
 * (:502), and an app that has just turned on 2048 has not asked for a frame to
 * be torn in half. Encoding it here is the smaller lie. */
static void send_size_report(PtTermCore *c) {
  if (c->pty_fd < 0 || c->child_exited) return;
  GhosttySizeReportSize size = { .rows = c->rows, .columns = c->cols,
                                 .cell_width = (uint32_t)c->cell_w,
                                 .cell_height = (uint32_t)c->cell_h };
  char buf[64];   /* 50 bytes is the widest mode-2048 report there can be */
  size_t written = 0;
  if (ghostty_size_report_encode(GHOSTTY_SIZE_REPORT_MODE_2048, size, buf,
                                 sizeof(buf), &written) != GHOSTTY_SUCCESS ||
      written == 0)
    return;
  pty_write_raw(c->pty_fd, buf, written);
}

static void poll_mode_edges(PtTermCore *c) {
  bool focus_event = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_FOCUS_EVENT,
                            &focus_event);
  /* Forced: the state has not changed, which is the whole point here. */
  if (focus_event && !c->was_focus_event)
    pt_term_core_focus_report(c, c->focused, TRUE);
  c->was_focus_event = focus_event;

  /* Mode 2048 is the same story: ghostty answers CSI ? 2048 h with a size
   * report on the spot (stream_handler.zig:750), so an app that enables it
   * while starting up can lay itself out without waiting for a window drag.
   * libghostty-vt makes the mode change an explicit no-op instead
   * (stream_terminal.zig:507-511), so the edge is pt's to catch. */
  bool in_band_resize = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_IN_BAND_RESIZE,
                            &in_band_resize);
  if (in_band_resize && !c->was_in_band_resize) send_size_report(c);
  c->was_in_band_resize = in_band_resize;
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
        /* After the parser, so an OSC consumer sees terminal state that
         * already includes the bytes it is reacting to. Skipped entirely
         * when nobody is listening. */
        if (c->cbs.osc != NULL || c->cbs.clipboard_write != NULL)
          pt_osc_scan_feed(&c->osc, buf, (size_t)n, core_osc_dispatch, c);
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
  if (got_data) poll_mode_edges(c);
  if (got_data && c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
  if (c->eof) { c->fd_source = 0; return G_SOURCE_REMOVE; }
  return G_SOURCE_CONTINUE;
}

/* ---- spawn ---- */
static int spawn_pty(const char *cwd, const char *const *argv,
                     char *const *env_pairs,
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
    setenv("COLORTERM", "truecolor", 1);
    /* env_pairs was copied before the fork, so putenv'ing its strings keeps
     * them alive for the (immediately following) exec without allocating. */
    for (int i = 0; env_pairs != NULL && env_pairs[i] != NULL; i++)
      putenv(env_pairs[i]);
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
                             const char *const *env_pairs,
                             guint16 cols, guint16 rows,
                             int cell_w, int cell_h, GError **error) {
  PtTermCore *c = g_new0(PtTermCore, 1);
  c->cols = cols; c->rows = rows; c->cell_w = cell_w; c->cell_h = cell_h;
  c->pty_fd = -1;
  c->last_exit = -1;
  c->osc52 = PT_CONFIG_OSC52_DEFAULT;
  if (env_pairs != NULL) c->env_pairs = g_strdupv((char **)env_pairs);

  GhosttyTerminalOptions opts = { .cols = cols, .rows = rows,
                                  .max_scrollback = 10000 };
  if (ghostty_terminal_new(NULL, &c->terminal, opts) != GHOSTTY_SUCCESS ||
      ghostty_render_state_new(NULL, &c->render_state) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_iterator_new(NULL, &c->row_iter) != GHOSTTY_SUCCESS ||
      ghostty_render_state_row_cells_new(NULL, &c->row_cells) != GHOSTTY_SUCCESS ||
      ghostty_key_encoder_new(NULL, &c->key_encoder) != GHOSTTY_SUCCESS ||
      ghostty_key_event_new(NULL, &c->key_event) != GHOSTTY_SUCCESS ||
      ghostty_mouse_encoder_new(NULL, &c->mouse_encoder) != GHOSTTY_SUCCESS ||
      ghostty_mouse_event_new(NULL, &c->mouse_event) != GHOSTTY_SUCCESS) {
    g_set_error(error, g_quark_from_static_string("pt-term-core"), 1,
                "libghostty-vt object creation failed");
    pt_term_core_free(c);
    return NULL;
  }
  ghostty_terminal_resize(c->terminal, cols, rows,
                          (uint32_t)cell_w, (uint32_t)cell_h);

  c->pty_fd = spawn_pty(cwd, argv, c->env_pairs, cols, rows, cell_w, cell_h,
                        &c->child);
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
  c->cmd_timer = g_timeout_add(700, poll_foreground_command, c);
  poll_foreground_command(c);   /* fire once so tabs label at startup */
  return c;
}

void pt_term_core_set_callbacks(PtTermCore *c, const PtTermCoreCallbacks *cbs,
                                gpointer user) {
  c->cbs = *cbs;
  c->cbs_user = user;
}

void pt_term_core_set_osc52(PtTermCore *c, PtOsc52Mode mode) {
  c->osc52 = mode;
}

void pt_term_core_resize(PtTermCore *c, guint16 cols, guint16 rows,
                         int cell_w, int cell_h) {
  if (cols < 1 || rows < 1) return;
  /* Nothing to do when nothing moved. pt_terminal_size_allocate calls straight
   * through on every GTK allocation, and ghostty_terminal_resize() re-sends a
   * byte-identical mode-2048 report however little changed, so without this an
   * app watching in-band resizes would be woken by every layout pass. Ghostty
   * guards further up and on a different quantity — the window's pixel size
   * (Surface.zig:2475) — and so does send duplicate reports when the pixels
   * move but the grid does not; pt's call site makes the stricter guard the
   * cheaper one. */
  if (cols == c->cols && rows == c->rows &&
      cell_w == c->cell_w && cell_h == c->cell_h)
    return;
  c->cols = cols; c->rows = rows; c->cell_w = cell_w; c->cell_h = cell_h;

  /* TIOCSWINSZ first, then the terminal, because ghostty_terminal_resize()
   * writes the mode-2048 report as it goes. Ghostty sets the winsize before it
   * reports too (Termio.zig:472 ahead of :495): an app that reads the report
   * and immediately asks the kernel with TIOCGWINSZ has to get one answer, not
   * the new size from one path and the old size from the other. */
  struct winsize ws = {
    .ws_row = rows, .ws_col = cols,
    .ws_xpixel = (unsigned short)(cols * cell_w),
    .ws_ypixel = (unsigned short)(rows * cell_h),
  };
  if (c->pty_fd >= 0) ioctl(c->pty_fd, TIOCSWINSZ, &ws);
  ghostty_terminal_resize(c->terminal, cols, rows,
                          (uint32_t)cell_w, (uint32_t)cell_h);
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

void pt_term_core_scroll_bottom(PtTermCore *c) {
  GhosttyTerminalScrollViewport sv = { .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM };
  ghostty_terminal_scroll_viewport(c->terminal, sv);
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
}

/* ---- mouse selection ----
 *
 * The pinned libghostty-vt has no selection-gesture object (the brief named
 * ghostty_selection_gesture_*, which does not exist in this ABI). We build the
 * selection directly from the snapshot API: pixel -> viewport cell -> a fresh
 * GhosttyGridRef pair -> GhosttySelection, installed with OPT_SELECTION which
 * copies it into terminal-owned tracked state immediately. Grid refs are
 * untracked snapshots (valid only until the next terminal mutation), so we
 * always resolve refs fresh and install in the same call rather than caching
 * them across events. Anchor is stored as a viewport cell coordinate.
 * Click count (double = word, triple = line) is derived from press timing. */

static void sel_pixel_to_cell(PtTermCore *c, double px, double py,
                              uint16_t *col, uint16_t *row) {
  double cx = (px - PT_CORE_PAD_X) / (double)c->cell_w;
  double cy = (py - PT_CORE_PAD_Y) / (double)c->cell_h;
  int ic = cx < 0 ? 0 : (int)cx;
  int ir = cy < 0 ? 0 : (int)cy;
  if (ic > c->cols - 1) ic = c->cols - 1;
  if (ir > c->rows - 1) ir = c->rows - 1;
  *col = (uint16_t)ic;
  *row = (uint16_t)ir;
}

static gboolean sel_ref_at(PtTermCore *c, uint16_t col, uint16_t row,
                           GhosttyGridRef *out) {
  GhosttyPoint pt = { .tag = GHOSTTY_POINT_TAG_VIEWPORT,
                      .value = { .coordinate = { .x = col, .y = row } } };
  *out = GHOSTTY_INIT_SIZED(GhosttyGridRef);
  return ghostty_terminal_grid_ref(c->terminal, pt, out) == GHOSTTY_SUCCESS;
}

static void sel_install(PtTermCore *c, const GhosttySelection *sel) {
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, sel);
  c->sel_active = TRUE;
}

static void sel_install_linear(PtTermCore *c,
                               uint16_t c0, uint16_t r0,
                               uint16_t c1, uint16_t r1) {
  GhosttyGridRef start, end;
  if (!sel_ref_at(c, c0, r0, &start) || !sel_ref_at(c, c1, r1, &end)) return;
  GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
  sel.start = start;
  sel.end = end;
  sel.rectangle = false;
  sel_install(c, &sel);
}

static void sel_install_word(PtTermCore *c, uint16_t col, uint16_t row) {
  GhosttyGridRef ref;
  if (!sel_ref_at(c, col, row, &ref)) return;
  GhosttyTerminalSelectWordOptions o =
      GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
  o.ref = ref;
  GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
  if (ghostty_terminal_select_word(c->terminal, &o, &sel) == GHOSTTY_SUCCESS)
    sel_install(c, &sel);
}

static void sel_install_line(PtTermCore *c, uint16_t col, uint16_t row) {
  GhosttyGridRef ref;
  if (!sel_ref_at(c, col, row, &ref)) return;
  GhosttyTerminalSelectLineOptions o =
      GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
  o.ref = ref;
  GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
  if (ghostty_terminal_select_line(c->terminal, &o, &sel) == GHOSTTY_SUCCESS)
    sel_install(c, &sel);
}

void pt_term_core_selection_clear(PtTermCore *c) {
  if (!c->sel_active) return;
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, NULL);
  c->sel_active = FALSE;
}

void pt_term_core_selection_press(PtTermCore *c, double px, double py,
                                  guint64 time_ns) {
  uint16_t col, row;
  sel_pixel_to_cell(c, px, py, &col, &row);

  /* Multi-click: same cell within 500ms cycles char -> word -> line. */
  if (c->last_press_ns != 0 && time_ns - c->last_press_ns < 500000000ULL &&
      col == c->last_press_col && row == c->last_press_row)
    c->click_count = c->click_count % 3 + 1;
  else
    c->click_count = 1;
  c->last_press_ns = time_ns;
  c->last_press_col = col;
  c->last_press_row = row;

  c->sel_anchor_col = col;
  c->sel_anchor_row = row;
  c->sel_moved = FALSE;
  c->sel_dragging = TRUE;

  pt_term_core_selection_clear(c);       /* a fresh press drops any prior sel */
  if (c->click_count == 2) sel_install_word(c, col, row);
  else if (c->click_count == 3) sel_install_line(c, col, row);
  /* single click installs nothing until the pointer actually drags */
}

void pt_term_core_selection_drag(PtTermCore *c, double px, double py) {
  if (!c->sel_dragging) return;
  uint16_t col, row;
  sel_pixel_to_cell(c, px, py, &col, &row);
  if (col != c->sel_anchor_col || row != c->sel_anchor_row) c->sel_moved = TRUE;
  /* Word/line selections stay put until the pointer leaves the anchor cell. */
  if (c->click_count >= 2 && !c->sel_moved) return;
  sel_install_linear(c, c->sel_anchor_col, c->sel_anchor_row, col, row);
}

void pt_term_core_selection_release(PtTermCore *c, double px, double py) {
  if (!c->sel_dragging) return;
  c->sel_dragging = FALSE;
  uint16_t col, row;
  sel_pixel_to_cell(c, px, py, &col, &row);
  if (c->sel_moved)
    sel_install_linear(c, c->sel_anchor_col, c->sel_anchor_row, col, row);
  /* No drag: a single click leaves nothing; a double/triple click keeps the
     word/line selection installed at press time. */
}

char *pt_term_core_selection_text(PtTermCore *c) {
  if (!c->sel_active) return NULL;
  GhosttyTerminalSelectionFormatOptions opt =
      GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
  opt.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
  opt.unwrap = true;                 /* clipboard semantics per header docs */
  opt.trim = true;
  opt.selection = NULL;              /* use the terminal's active selection */
  uint8_t *ptr = NULL;
  size_t len = 0;
  if (ghostty_terminal_selection_format_alloc(c->terminal, NULL, opt,
                                              &ptr, &len) != GHOSTTY_SUCCESS ||
      ptr == NULL || len == 0)
    return NULL;
  char *out = g_strndup((const char *)ptr, len);  /* result is not NUL-term'd */
  ghostty_free(NULL, ptr, len);                    /* free with same allocator */
  return out;
}

/* ---- OSC 8 hyperlinks ----
 *
 * libghostty already parses OSC 8 and hangs the URI off every cell the program
 * wrote between the start and end sequences, so this is a lookup: pixel ->
 * viewport cell -> a fresh grid ref -> the URI stored against it. Nothing here
 * detects links in plain text; a program has to say a run of cells is one. */

gboolean pt_term_core_hyperlink_is_safe(const char *uri) {
  if (uri == NULL) return FALSE;
  /* Whitespace and control bytes never appear in a URI that was written
   * properly (a space belongs percent-encoded), and a scheme test alone would
   * happily pass "http://x\nrm -rf ~" to whatever opens it. */
  for (const char *p = uri; *p != '\0'; p++)
    if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7F) return FALSE;
  const char *colon = strchr(uri, ':');
  if (colon == NULL) return FALSE;          /* no scheme: nothing to open it */
  gsize len = (gsize)(colon - uri);
  static const char *const allowed[] = { "http", "https", "file", "mailto" };
  for (gsize i = 0; i < G_N_ELEMENTS(allowed); i++)
    if (strlen(allowed[i]) == len &&
        g_ascii_strncasecmp(uri, allowed[i], len) == 0)
      return TRUE;
  return FALSE;
}

char *pt_term_core_hyperlink_at(PtTermCore *c, double px, double py) {
  /* Not clamped to the grid the way a selection drag is: the padding around it
   * is not part of any cell, and clamping there would make the edge column's
   * link openable from outside it. */
  double cx = (px - PT_CORE_PAD_X) / (double)c->cell_w;
  double cy = (py - PT_CORE_PAD_Y) / (double)c->cell_h;
  if (cx < 0 || cy < 0 || cx >= c->cols || cy >= c->rows) return NULL;

  GhosttyGridRef ref;
  if (!sel_ref_at(c, (uint16_t)cx, (uint16_t)cy, &ref)) return NULL;
  /* A NULL buffer only asks for the length; no link at all reports zero. */
  size_t need = 0;
  if (ghostty_grid_ref_hyperlink_uri(&ref, NULL, 0, &need) !=
          GHOSTTY_OUT_OF_SPACE || need == 0)
    return NULL;
  char *uri = g_malloc(need + 1);
  size_t written = 0;
  if (ghostty_grid_ref_hyperlink_uri(&ref, (uint8_t *)uri, need,
                                     &written) != GHOSTTY_SUCCESS) {
    g_free(uri);
    return NULL;
  }
  uri[written] = '\0';                       /* the URI is not NUL-terminated */
  /* An embedded NUL would leave the tail of the URI outside every check below
   * while some consumers still read past it. */
  if (strlen(uri) != written || !pt_term_core_hyperlink_is_safe(uri)) {
    g_free(uri);
    return NULL;
  }
  return uri;
}

gboolean pt_term_core_mouse_tracking(PtTermCore *c) {
  bool tracking = false;
  ghostty_terminal_get(c->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                       &tracking);
  return tracking;
}

/* ---- mouse reporting ----
 *
 * The encoder needs both the terminal's protocol state (which tracking mode,
 * which output format) and the renderer geometry, so it can map surface pixels
 * to cells and tell when the pointer left the viewport. Terminal state is
 * synced per event because the app can flip modes at any time; the geometry is
 * rebuilt from the same cell metrics and padding the widget draws with. */
static void mouse_encoder_sync(PtTermCore *c, gboolean any_button_pressed) {
  ghostty_mouse_encoder_setopt_from_terminal(c->mouse_encoder, c->terminal);

  GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
  size.screen_width = (uint32_t)(c->cols * c->cell_w + 2 * PT_CORE_PAD_X);
  size.screen_height = (uint32_t)(c->rows * c->cell_h + 2 * PT_CORE_PAD_Y);
  size.cell_width = (uint32_t)c->cell_w;
  size.cell_height = (uint32_t)c->cell_h;
  size.padding_left = size.padding_right = PT_CORE_PAD_X;
  size.padding_top = size.padding_bottom = PT_CORE_PAD_Y;
  ghostty_mouse_encoder_setopt(c->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE,
                               &size);

  bool pressed = any_button_pressed;
  ghostty_mouse_encoder_setopt(
      c->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &pressed);
  /* Dedupe motion by cell: without this a TUI gets a report per pixel. */
  bool track_last_cell = true;
  ghostty_mouse_encoder_setopt(
      c->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL,
      &track_last_cell);
}

gboolean pt_term_core_mouse_report(PtTermCore *c, GhosttyMouseAction action,
                                   GhosttyMouseButton button, GhosttyMods mods,
                                   double px, double py) {
  if (c->child_exited || c->pty_fd < 0) return FALSE;

  /* The encoder wants a button mask that already includes the current event,
   * so a press registers before encoding and a release clears before it. */
  if (button != GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
    if (action == GHOSTTY_MOUSE_ACTION_PRESS)
      c->buttons_down |= 1u << button;
    else if (action == GHOSTTY_MOUSE_ACTION_RELEASE)
      c->buttons_down &= ~(1u << button);
  }
  mouse_encoder_sync(c, c->buttons_down != 0);

  ghostty_mouse_event_set_action(c->mouse_event, action);
  if (button != GHOSTTY_MOUSE_BUTTON_UNKNOWN)
    ghostty_mouse_event_set_button(c->mouse_event, button);
  else
    ghostty_mouse_event_clear_button(c->mouse_event);
  ghostty_mouse_event_set_mods(c->mouse_event, mods);
  ghostty_mouse_event_set_position(c->mouse_event,
                                   (GhosttyMousePosition){ .x = (float)px,
                                                           .y = (float)py });

  char buf[128];
  size_t written = 0;
  if (ghostty_mouse_encoder_encode(c->mouse_encoder, c->mouse_event, buf,
                                   sizeof(buf), &written) != GHOSTTY_SUCCESS ||
      written == 0)
    return FALSE;
  pty_write_raw(c->pty_fd, buf, written);
  return TRUE;
}

gboolean pt_term_core_focus_report(PtTermCore *c, gboolean focused,
                                   gboolean force) {
  /* The dedupe sits above the mode check, where ghostty puts it
   * (Surface.zig:3309 dedupes, Termio.focusGained checks 1004 below it): the
   * state has to be recorded even while nobody is listening, or the resend on
   * enable has nothing to report. Ghostty keeps that state on the terminal, so
   * RIS resets it to "focused" whatever the truth is; keeping it here instead
   * means pt still knows. */
  if (!force && c->focused == focused) return FALSE;
  c->focused = focused;
  if (c->child_exited || c->pty_fd < 0) return FALSE;

  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_FOCUS_EVENT, &on);
  if (!on) return FALSE;

  char buf[8];   /* three bytes today; the header asks callers not to bet on it */
  size_t written = 0;
  if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST,
                           buf, sizeof(buf), &written) != GHOSTTY_SUCCESS ||
      written == 0)
    return FALSE;
  pty_write_raw(c->pty_fd, buf, written);
  return TRUE;
}

gboolean pt_term_core_alt_screen(PtTermCore *c) {
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  ghostty_terminal_get(c->terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                       &screen);
  return screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
}

gboolean pt_term_core_alt_scroll(PtTermCore *c) {
  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_ALT_SCROLL, &on);
  return on;
}

void pt_term_core_send_arrows(PtTermCore *c, gboolean up, int count) {
  if (c->child_exited || c->pty_fd < 0 || count <= 0) return;
  bool app_cursor = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_DECCKM, &app_cursor);
  const char *seq = app_cursor ? (up ? "\x1bOA" : "\x1bOB")
                               : (up ? "\x1b[A" : "\x1b[B");
  for (int i = 0; i < count; i++) pty_write_raw(c->pty_fd, seq, 3);
}

gboolean pt_term_core_bracketed_paste(PtTermCore *c) {
  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_BRACKETED_PASTE, &on);
  return on;
}

void pt_term_core_paste(PtTermCore *c, const char *text, gssize len) {
  if (c->pty_fd < 0 || c->child_exited || text == NULL) return;
  size_t n = len < 0 ? strlen(text) : (size_t)len;
  if (n == 0) return;

  /* ghostty_paste_encode rewrites the unsafe bytes in place, so it gets a
   * copy — the caller's string comes from the clipboard and is not ours. */
  char *data = g_memdup2(text, n);
  bool bracketed = pt_term_core_bracketed_paste(c);
  /* A NULL buffer only asks for the size (markers included). The in-place
   * pass is idempotent, so encoding twice over the same copy is safe. */
  size_t need = 0;
  ghostty_paste_encode(data, n, bracketed, NULL, 0, &need);
  char *out = g_malloc(need);
  size_t written = 0;
  if (ghostty_paste_encode(data, n, bracketed, out, need,
                           &written) == GHOSTTY_SUCCESS)
    pty_write_raw(c->pty_fd, out, written);
  g_free(out);
  g_free(data);
}

gboolean pt_term_core_paste_is_safe(const char *text, gssize len) {
  if (text == NULL) return TRUE;
  size_t n = len < 0 ? strlen(text) : (size_t)len;
  /* ghostty looks for "\n" and the end sequence, and its encoder replaces
   * control bytes with spaces — but a bare CR is in neither set, so it reaches
   * the pty untouched and the line discipline's ICRNL turns it back into a
   * newline. That submits the line, which is the whole thing this check exists
   * to catch. Old-Mac line endings and crafted web pages both produce one.
   * Unsafe regardless of bracketed paste mode, exactly as "\n" already is. */
  if (memchr(text, '\r', n) != NULL) return FALSE;
  return ghostty_paste_is_safe(text, n);
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

static char *grid_text(GhosttyRenderState rs, GhosttyRenderStateRowIterator it,
                       GhosttyRenderStateRowCells rc) {
  GString *out = g_string_new(NULL);
  GhosttyRenderStateRowIterator iter = it;
  if (ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                               &iter) != GHOSTTY_SUCCESS)
    return g_string_free(out, FALSE);
  while (ghostty_render_state_row_iterator_next(iter)) {
    GhosttyRenderStateRowCells cells = rc;
    if (ghostty_render_state_row_get(iter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &cells) != GHOSTTY_SUCCESS)
      continue;
    gsize row_start = out->len;
    while (ghostty_render_state_row_cells_next(cells)) {
      uint32_t glen = 0;
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
      if (glen == 0) { g_string_append_c(out, ' '); continue; }
      /* GRAPHEMES_BUF writes ALL glen codepoints; the buffer must hold glen.
         Use a stack buffer for the common case, heap for long clusters. */
      uint32_t cps_stack[16];
      uint32_t *cps = glen <= 16 ? cps_stack : g_new(uint32_t, glen);
      ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      for (uint32_t i = 0; i < glen; i++) {
        char u8[4];
        g_string_append_len(out, u8, utf8_encode_cp(cps[i], u8));
      }
      if (cps != cps_stack) g_free(cps);
    }
    /* trim trailing spaces on the row */
    while (out->len > row_start && out->str[out->len - 1] == ' ')
      g_string_truncate(out, out->len - 1);
    g_string_append_c(out, '\n');
  }
  return g_string_free(out, FALSE);
}

char *pt_term_core_grid_text(PtTermCore *c) {
  return grid_text(c->render_state, c->row_iter, c->row_cells);
}

char *pt_term_grid_text_raw(GhosttyTerminal t) {
  GhosttyRenderState rs = NULL;
  GhosttyRenderStateRowIterator iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  char *out = NULL;
  if (ghostty_render_state_new(NULL, &rs) == GHOSTTY_SUCCESS &&
      ghostty_render_state_row_iterator_new(NULL, &iter) == GHOSTTY_SUCCESS &&
      ghostty_render_state_row_cells_new(NULL, &cells) == GHOSTTY_SUCCESS) {
    ghostty_render_state_update(rs, t);
    out = grid_text(rs, iter, cells);
  }
  if (cells != NULL) ghostty_render_state_row_cells_free(cells);
  if (iter != NULL) ghostty_render_state_row_iterator_free(iter);
  if (rs != NULL) ghostty_render_state_free(rs);
  return out;
}

gboolean pt_term_core_exited(PtTermCore *c, int *status) {
  if (status != NULL) *status = c->exit_status;
  return c->child_exited;
}

pid_t pt_term_core_shell_pid(PtTermCore *c) { return c->child; }

gboolean pt_term_core_running(PtTermCore *c) {
  if (c->pty_fd < 0 || c->child_exited) return FALSE;
  pid_t fg = tcgetpgrp(c->pty_fd);
  return fg > 0 && fg != c->child;
}

int pt_term_core_last_exit(PtTermCore *c) { return c->last_exit; }

void pt_term_core_free(PtTermCore *c) {
  if (c == NULL) return;
  if (c->fd_source != 0) g_source_remove(c->fd_source);
  if (c->child_source != 0) g_source_remove(c->child_source);
  if (c->cmd_timer != 0) g_source_remove(c->cmd_timer);
  pt_osc_scan_clear(&c->osc);
  if (c->pty_fd >= 0) close(c->pty_fd);
  if (c->child > 0 && !c->child_exited) {
    kill(c->child, SIGHUP);
    waitpid(c->child, NULL, 0);
  }
  if (c->key_event != NULL) ghostty_key_event_free(c->key_event);
  if (c->key_encoder != NULL) ghostty_key_encoder_free(c->key_encoder);
  if (c->mouse_event != NULL) ghostty_mouse_event_free(c->mouse_event);
  if (c->mouse_encoder != NULL) ghostty_mouse_encoder_free(c->mouse_encoder);
  if (c->row_cells != NULL) ghostty_render_state_row_cells_free(c->row_cells);
  if (c->row_iter != NULL) ghostty_render_state_row_iterator_free(c->row_iter);
  if (c->render_state != NULL) ghostty_render_state_free(c->render_state);
  if (c->terminal != NULL) ghostty_terminal_free(c->terminal);
  g_strfreev(c->env_pairs);
  g_free(c);
}
