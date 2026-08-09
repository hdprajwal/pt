#include "pt-term-core.h"
#include "pt-term-core-internal.h"
#include "pt-status-parse.h"
#include "pt-agent-session.h"
#include <glib-unix.h>
#include <glib/gstdio.h>
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
  GhosttyKeyEncoder key_encoder;
  GhosttyKeyEvent key_event;
  GhosttyMouseEncoder mouse_encoder;
  GhosttyMouseEvent mouse_event;
  guint buttons_down;       /* bitmask of held buttons, by GhosttyMouseButton */

  int pty_fd;
  pid_t child;
  char *shell_name;         /* basename of what spawn exec'd; set once at spawn */
  guint fd_source;
  guint child_source;
  guint cmd_timer;
  char last_comm[64];
  char *last_title;         /* last title handed to cbs.title, for the dedupe */
  char **env_pairs;         /* extra "KEY=VALUE" set in the child, or NULL */
  char *pane_token;         /* $PT_PANE_TOKEN; see pt_term_core_pane_token */
  guint16 cols, rows;
  int cell_w, cell_h;
  int pad_x, pad_y;         /* grid inset; see pt_term_core_set_padding */

  gboolean eof;
  gboolean child_exited;
  gboolean fg_running;      /* cached fg-pgrp-vs-child; see pt_term_core_running */
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

  /* color scheme (CSI ? 996 n, mode 2031); see pt_term_core_set_color_scheme */
  gboolean dark;

  /* scrollbar cache; see pt_term_core_scrollbar */
  GhosttyTerminalScrollbar sb;   /* the last answer the library gave */
  gboolean sb_valid;             /* sb has been filled at least once */
  gboolean sb_dirty;             /* something moved since it was filled */
  guint64 sb_reads;              /* library reads, counted for the tests */

  /* what a frame would draw changed; see pt_term_core_take_render_dirty.
   * content_serial is bumped by every change, taken_serial latches it on
   * take, so "dirty" is the two disagreeing and readers move neither. */
  guint content_serial;
  guint taken_serial;

  PtTermCoreCallbacks cbs;
  gpointer cbs_user;
};

/* Bytes of history a fresh core is built with; see
 * pt_term_core_set_scrollback_limit. */
static gsize default_scrollback = PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT;

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

/* CSI ? 996 n. Answered whether or not mode 2031 is on — a direct question
 * always gets an answer, only the unsolicited notification is mode-gated
 * (Termio.zig:711-720, where ghostty's `force` flag splits the two). */
static bool effect_color_scheme(GhosttyTerminal t, void *ud,
                                GhosttyColorScheme *out) {
  (void)t;
  PtTermCore *c = ud;
  *out = c->dark ? GHOSTTY_COLOR_SCHEME_DARK : GHOSTTY_COLOR_SCHEME_LIGHT;
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
  /* Titles longer than the buffer are cut back to a character boundary, never
   * mid-codepoint: this string is the pane's name now, so it reaches a
   * GtkLabel in the tab strip and is written into state.json. Invalid UTF-8
   * there makes json-glib emit bytes it cannot read back, and an unparseable
   * state file is moved aside at the next launch — the user loses the whole
   * workspace layout. Ghostty's OSC buffer is 2048 bytes and it validates the
   * title as UTF-8 before this runs, so oversized-but-valid is the real case:
   * backing off continuation bytes lands on the lead byte of the sequence the
   * cut fell inside. Not notify_copy, whose answer to a bad string is to
   * refuse the whole thing: the exit marker below has to be recorded even from
   * a title whose tail is unusable. The validate after it is the belt. */
  char buf[256];
  size_t len = title.len;
  if (len > sizeof(buf) - 1) {
    const uint8_t *p = title.ptr + sizeof(buf) - 1;
    while (p > title.ptr && (*p & 0xC0) == 0x80) p--;
    len = (size_t)(p - title.ptr);
  }
  if (len > 0) memcpy(buf, title.ptr, len);
  buf[len] = '\0';
  /* The prompt snippet prefixes the title with the last command's status:
   * "pt-exit:<code>;<real title>". Record it and hand on the real title.
   * Done before the callback check so the state is tracked either way. */
  int code = 0;
  const char *rest = NULL;
  gboolean from_prompt = FALSE;
  if (pt_exit_marker_parse(buf, &code, &rest)) {
    c->last_exit = code;
    memmove(buf, rest, strlen(rest) + 1);
    from_prompt = TRUE;
  }
  /* Nothing invalid may go on to a GtkLabel or the session file, so a title
   * that does not validate is dropped whole — including from last_title, which
   * would otherwise dedupe away the next good one. The exit code is kept
   * either way: it rides in the ASCII marker at the front, which a bad tail
   * cannot reach. */
  if (!g_utf8_validate(buf, -1, NULL)) return;
  /* Shells re-emit the same title every prompt; only a change is worth a
   * callback. Compared after the marker strip, so a prompt whose exit code
   * moved but whose title did not still updates last_exit above in silence. */
  if (g_strcmp0(buf, c->last_title) == 0) return;
  g_free(c->last_title);
  c->last_title = g_strdup(buf);
  if (c->cbs.title != NULL) c->cbs.title(c, buf, from_prompt, c->cbs_user);
}

/* ---- foreground-command watcher ----
 * Polls the pty's foreground process group (the app the user is actually
 * interacting with, e.g. nvim under the shell) and reports its /proc/<pid>/comm
 * whenever it changes, so tabs can label themselves live. */
static gboolean poll_foreground_command(gpointer ud) {
  PtTermCore *c = ud;
  if (c->child_exited) return G_SOURCE_CONTINUE;
  pid_t fg = tcgetpgrp(c->pty_fd);
  /* Recorded here, before the fixup below, so pt_term_core_running() is a
   * field read instead of a tcgetpgrp per call — this poll already pays for
   * the syscall every 700ms. */
  c->fg_running = fg > 0 && fg != c->child;
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

/* ---- desktop notifications (OSC 9, OSC 777) ----
 *
 * See pt-term-core-internal.h for why the ConEmu tree is walked in full rather
 * than just checking for the `4;` progress prefix: ghostty routes eight other
 * subcodes away from notifications too, and a pane that popped a desktop
 * notification saying "9;/home/me" every time a shell reported its cwd would
 * be worse than one that popped none. */

/* Is this OSC 9 payload a notification, a progress report, or one of the
 * ConEmu extensions pt does not implement? Mirrors the switch in ghostty's
 * terminal/osc/parsers/osc9.zig, including every length check: each `break
 * :conemu` there — an extension that is cut short, or carries a subcode
 * ghostty does not know — falls through to a notification, and so does each
 * SHOW here. */
static PtOscNotifyKind osc9_kind(const char *p, gsize len) {
  if (len == 0) return PT_OSC_NOTIFY_SHOW;
  switch (p[0]) {
    case '1':
      if (len < 2) return PT_OSC_NOTIFY_SHOW;
      switch (p[1]) {
        case ';': return PT_OSC_NOTIFY_NONE;        /* 9;1 sleep */
        case '0':                                   /* 9;10 xterm emulation */
          /* Bare `9;10` means both on; with an argument only 0..3 are ConEmu. */
          if (len == 2) return PT_OSC_NOTIFY_NONE;
          if (len < 4 || p[2] != ';') return PT_OSC_NOTIFY_SHOW;
          return (p[3] >= '0' && p[3] <= '3') ? PT_OSC_NOTIFY_NONE
                                              : PT_OSC_NOTIFY_SHOW;
        case '1':                                   /* 9;11 comment */
          if (len < 3 || p[2] != ';') return PT_OSC_NOTIFY_SHOW;
          return PT_OSC_NOTIFY_NONE;
        /* 9;12 marks a prompt start and takes no argument, so ghostty accepts
         * it whatever follows — `9;12;anything` is still the mark. */
        case '2': return PT_OSC_NOTIFY_NONE;
        default:  return PT_OSC_NOTIFY_SHOW;
      }
    /* 9;2 message box, 9;3 tab title, 9;6 guimacro, 9;7 run process,
     * 9;8 environment variable, 9;9 report cwd: all `<digit>;<text>`, and all
     * things pt drops on the floor for now. */
    case '2': case '3': case '6': case '7': case '8': case '9':
      if (len < 2 || p[1] != ';') return PT_OSC_NOTIFY_SHOW;
      return PT_OSC_NOTIFY_NONE;
    case '4':                                       /* 9;4 progress report */
      if (len < 3 || p[1] != ';') return PT_OSC_NOTIFY_SHOW;
      return (p[2] >= '0' && p[2] <= '4') ? PT_OSC_NOTIFY_PROGRESS
                                          : PT_OSC_NOTIFY_SHOW;
    /* 9;5 waits for input and takes no argument either. */
    case '5': return PT_OSC_NOTIFY_NONE;
    default:  return PT_OSC_NOTIFY_SHOW;
  }
}

PtOscNotifyKind pt_osc_notification(int code, const char *payload, gsize len,
                                    PtOscNotification *out) {
  if (code == 9) {
    PtOscNotifyKind kind = osc9_kind(payload, len);
    if (kind == PT_OSC_NOTIFY_SHOW && out != NULL) {
      out->title = "";        /* OSC 9 has no title; the consumer supplies one */
      out->title_len = 0;
      out->body = payload;
      out->body_len = len;
    }
    return kind;
  }
  if (code != 777) return PT_OSC_NOTIFY_NONE;

  /* OSC 777 is rxvt's extension slot and `notify` is the only extension in it
   * pt (or ghostty) implements, so the name has to match exactly. Both
   * separators have to be there: with no second ';' there is no title, which
   * ghostty treats as malformed rather than as a body-only notification
   * (terminal/osc/parsers/rxvt_extension.zig). The body is the rest, extra
   * semicolons and all. */
  static const char ext[] = "notify";
  const char *k = memchr(payload, ';', len);
  if (k == NULL) return PT_OSC_NOTIFY_NONE;
  gsize ext_len = (gsize)(k - payload);
  if (ext_len != sizeof(ext) - 1 || memcmp(payload, ext, ext_len) != 0)
    return PT_OSC_NOTIFY_NONE;
  const char *rest = k + 1;
  const char *t = memchr(rest, ';', len - (gsize)(rest - payload));
  if (t == NULL) return PT_OSC_NOTIFY_NONE;
  if (out != NULL) {
    out->title = rest;
    out->title_len = (gsize)(t - rest);
    out->body = t + 1;
    out->body_len = len - (gsize)(t + 1 - payload);
  }
  return PT_OSC_NOTIFY_SHOW;
}

/* Copy at most `cap` bytes of `src` into `dst` (which holds cap+1), and say
 * whether the text may be shown at all.
 *
 * Two things happen here that ghostty does not do, both because the far end is
 * the session bus rather than a Zig string: text that is not valid UTF-8 is
 * refused outright (the payload came from whatever was writing to the pty, and
 * a GNotification body is offered to the desktop as UTF-8 — this is the same
 * rule pt already applies to OSC 52 clipboard writes), and the truncation
 * backs up to a character boundary instead of cutting mid-codepoint. */
static gboolean notify_copy(char *dst, gsize cap, const char *src, gsize len) {
  if (!g_utf8_validate(src, (gssize)len, NULL)) return FALSE;
  gsize n = len;
  if (n > cap) {
    const char *p = src + cap;
    /* Validated above, so walking back off continuation bytes always lands on
     * a lead byte, and never before src (the first byte cannot be one). */
    while (p > src && ((guchar)*p & 0xC0) == 0x80) p--;
    n = (gsize)(p - src);
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
  return TRUE;
}

/* The process-wide rate limit. Ghostty's is the same shape and the same two
 * numbers (Surface.zig showDesktopNotification): one notification per second
 * whatever it says, and five seconds before the same text may repeat. Ghostty
 * compares a Wyhash of title+body; pt compares the text itself, which is
 * already capped at 63+255 bytes and so costs a memcmp, and cannot collide. */
#define PT_NOTIFY_MIN_INTERVAL_US    ((gint64)1 * G_USEC_PER_SEC)
#define PT_NOTIFY_REPEAT_INTERVAL_US ((gint64)5 * G_USEC_PER_SEC)

static gboolean notify_seen;
static gint64 notify_last_us;
static char notify_last_title[PT_NOTIFY_TITLE_MAX + 1];
static char notify_last_body[PT_NOTIFY_BODY_MAX + 1];

gboolean pt_notify_gate(gint64 now_us, const char *title, const char *body) {
  if (notify_seen) {
    gint64 since = now_us - notify_last_us;
    if (since < PT_NOTIFY_MIN_INTERVAL_US) return FALSE;
    if (since < PT_NOTIFY_REPEAT_INTERVAL_US &&
        strcmp(title, notify_last_title) == 0 &&
        strcmp(body, notify_last_body) == 0)
      return FALSE;
  }
  notify_seen = TRUE;
  notify_last_us = now_us;
  g_strlcpy(notify_last_title, title, sizeof notify_last_title);
  g_strlcpy(notify_last_body, body, sizeof notify_last_body);
  return TRUE;
}

void pt_notify_gate_reset(void) {
  notify_seen = FALSE;
  notify_last_us = 0;
  notify_last_title[0] = '\0';
  notify_last_body[0] = '\0';
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

/* An OSC 9 or OSC 777 that asked for a desktop notification, on its way to the
 * consumer. Everything that can refuse it is here, cheapest first. */
static void core_notification(PtTermCore *c, int code, const char *payload,
                              gsize len) {
  PtOscNotification n = {0};
  if (pt_osc_notification(code, payload, len, &n) != PT_OSC_NOTIFY_SHOW)
    return;
  /* The pane the user is looking at right now says what it has to say on
   * screen. Checked before the rate limit, not after: a focused pane in a
   * loop must not spend the budget that an unfocused pane's one real
   * notification needs. c->focused follows the widget's focus, and GTK takes
   * focus away from the pane when the window itself goes inactive, so this is
   * "the focused pane of the focused window" and not just "the focused pane" —
   * see the leave handler in pt-terminal.c for why that second part holds. */
  if (c->focused) return;
  char title[PT_NOTIFY_TITLE_MAX + 1];
  char body[PT_NOTIFY_BODY_MAX + 1];
  if (!notify_copy(title, PT_NOTIFY_TITLE_MAX, n.title, n.title_len)) return;
  if (!notify_copy(body, PT_NOTIFY_BODY_MAX, n.body, n.body_len)) return;
  if (!pt_notify_gate(g_get_monotonic_time(), title, body)) return;
  c->cbs.notification(c, title, body, c->cbs_user);
}

static void core_osc_dispatch(int code, const char *payload, gsize len,
                              gpointer user) {
  PtTermCore *c = user;
  /* Rechecked per dispatch, not just once per read: a single read can carry
   * several sequences, and a consumer is allowed to unregister itself from
   * inside its own handler. Without this the next sequence in the same read
   * would call through a NULL pointer. */
  if ((code == 9 || code == 777) && c->cbs.notification != NULL)
    core_notification(c, code, payload, len);
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
  c->fg_running = FALSE;    /* nothing runs on a tty whose child is gone */
  c->child_source = 0;
  if (WIFEXITED(wait_status)) c->exit_status = WEXITSTATUS(wait_status);
  else if (WIFSIGNALED(wait_status)) c->exit_status = 128 + WTERMSIG(wait_status);
  else c->exit_status = -1;
  if (c->cbs.exited != NULL) c->cbs.exited(c, c->exit_status, c->cbs_user);
}

/* ---- modes a program turning on has to be answered for ----
 *
 * Ghostty answers the instant its parser sees the enable: CSI ? 1004 h resends
 * the current focus state (stream_handler.zig:754-756), CSI ? 2048 h sends a
 * size report (:750). An editor starting up in an already-focused pane, or one
 * that wants its size before the first redraw, is told straight away instead of
 * waiting for the user to click away and back or drag the window.
 *
 * libghostty-vt hands pt no such hook: terminal.h exposes modes through
 * ghostty_terminal_mode_get only, and the 2048 change is an explicit no-op
 * (stream_terminal.zig:507-511). So pt compares each mode against its value at
 * the end of the previous read and answers a 0->1 edge.
 *
 * That is a weaker rule than ghostty's, not the same behaviour, and in two ways
 * worth naming. An app enabling a mode that is already on gets no answer — the
 * case being an app killed hard enough that it never sent the disable, so the
 * next one to start finds the mode still set. And an enable sharing a read with
 * a matching disable nets out to no edge, so it is missed too. Either way the
 * app waits for the next real focus change or resize, which is where it would
 * have been with no reporting at all. Catching both means scanning the pty
 * bytes for the sequences, i.e. a second parser for pt to own; the poll is one
 * bitfield read per batch. If this ever bites, that is the trade to revisit. */

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
  char buf[64];   /* 49 bytes is the widest mode-2048 report there can be */
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

  bool in_band_resize = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_IN_BAND_RESIZE,
                            &in_band_resize);
  if (in_band_resize && !c->was_in_band_resize) send_size_report(c);
  c->was_in_band_resize = in_band_resize;
}

/* How much one dispatch may drain before handing the main loop back. A child
 * flooding the pty (`cat bigfile`) can otherwise hold this callback for as
 * long as the kernel keeps refilling the buffer; past the cap the dispatch
 * ends normally and the still-readable fd re-fires the source immediately, so
 * input and redraws stay interleaved with the flood. */
#define PT_READ_MAX_PER_DISPATCH (256u * 1024u)

static gboolean on_pty_readable(gint fd, GIOCondition cond, gpointer ud) {
  PtTermCore *c = ud;
  gboolean got_data = FALSE;
  if (cond & (G_IO_IN | G_IO_HUP)) {
    uint8_t buf[4096];
    gsize drained = 0;
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n > 0) {
        ghostty_terminal_vt_write(c->terminal, buf, (size_t)n);
        /* After the parser, so an OSC consumer sees terminal state that
         * already includes the bytes it is reacting to. Skipped entirely
         * when nobody is listening. */
        if (c->cbs.osc != NULL || c->cbs.clipboard_write != NULL ||
            c->cbs.notification != NULL)
          pt_osc_scan_feed(&c->osc, buf, (size_t)n, core_osc_dispatch, c);
        got_data = TRUE;
        drained += (gsize)n;
        if (drained >= PT_READ_MAX_PER_DISPATCH) break;
      } else if (n == 0) { c->eof = TRUE; break; }
      else {
        if (errno == EAGAIN) break;
        if (errno == EINTR) continue;
        c->eof = TRUE;      /* EIO on Linux when the slave side closes */
        break;
      }
    }
  }
  /* Bytes reached the parser, so rows may have been added, scrolled away or
   * reflowed and the cached scrollbar is stale. This is the only place the
   * terminal is written to. */
  if (got_data) { c->sb_dirty = TRUE; c->content_serial++; }
  if (got_data) poll_mode_edges(c);
  if (got_data && c->cbs.output != NULL) c->cbs.output(c, c->cbs_user);
  if (got_data && c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
  if (c->eof) { c->fd_source = 0; return G_SOURCE_REMOVE; }
  return G_SOURCE_CONTINUE;
}

/* ---- spawn ---- */
/* The shell a NULL-argv spawn execs, as the *child* will see it: the inherited
 * $SHELL with any env_pairs "SHELL=" override applied on top (last one wins,
 * exactly like the child's putenv sequence), then the passwd entry, then
 * /bin/sh. Resolved once, pre-fork, and handed to both consumers — the child's
 * exec and the parent's shell-name cache — so the two cannot disagree, and
 * nothing is ever read back from /proc/<child>/comm (a read that races the
 * child's exec and can see the parent's own comm). */
static const char *resolve_shell(char *const *env_pairs) {
  const char *shell = getenv("SHELL");
  for (int i = 0; env_pairs != NULL && env_pairs[i] != NULL; i++)
    if (strncmp(env_pairs[i], "SHELL=", 6) == 0) shell = env_pairs[i] + 6;
  if (shell != NULL && shell[0] != '\0') return shell;
  struct passwd *pw = getpwuid(getuid());
  return (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0] != '\0')
             ? pw->pw_shell : "/bin/sh";
}

/* `shell` is resolve_shell()'s answer and is consumed iff argv == NULL. */
static int spawn_pty(const char *cwd, const char *const *argv,
                     const char *shell, char *const *env_pairs,
                     const char *pane_token,
                     guint16 cols, guint16 rows, int cell_w, int cell_h,
                     pid_t *child_out) {
  struct winsize ws = {
    .ws_row = rows, .ws_col = cols,
    .ws_xpixel = (unsigned short)(cols * cell_w),
    .ws_ypixel = (unsigned short)(rows * cell_h),
  };
  /* Everything the child touches between fork and exec is computed here,
   * before the fork: getenv/getpwuid/allocation are not async-signal-safe,
   * so the child branch only follows precomputed pointers. */
  const char *shell_argv0 = NULL;
  if (shell != NULL) {
    shell_argv0 = strrchr(shell, '/');
    shell_argv0 = shell_argv0 != NULL ? shell_argv0 + 1 : shell;
  }
  int fd;
  pid_t child = forkpty(&fd, NULL, NULL, &ws);
  if (child < 0) return -1;
  if (child == 0) {
    if (cwd != NULL) { if (chdir(cwd) != 0) { /* fall through to $HOME */ chdir(g_get_home_dir()); } }
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    /* Names this pane to anything the child runs: an agent integration writes
     * its session report under this key. Set before env_pairs so a caller can
     * still override it. */
    if (pane_token != NULL) setenv("PT_PANE_TOKEN", pane_token, 1);
    /* env_pairs was copied before the fork, so putenv'ing its strings keeps
     * them alive for the (immediately following) exec without allocating. */
    for (int i = 0; env_pairs != NULL && env_pairs[i] != NULL; i++)
      putenv(env_pairs[i]);
    if (argv != NULL) {
      execvp(argv[0], (char *const *)argv);
    } else {
      execl(shell, shell_argv0, (char *)NULL);
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
  c->pad_x = PT_CONFIG_WINDOW_PADDING_X_DEFAULT;
  c->pad_y = PT_CONFIG_WINDOW_PADDING_Y_DEFAULT;
  c->pty_fd = -1;
  c->last_exit = -1;
  c->osc52 = PT_CONFIG_OSC52_DEFAULT;
  /* pt ships one theme and it is dark, so a core nobody configures answers
   * dark rather than lying about a light background it is not painting. */
  c->dark = TRUE;
  c->content_serial = 1;    /* a first frame always has everything to draw */
  if (env_pairs != NULL) c->env_pairs = g_strdupv((char **)env_pairs);
  /* Before the spawn: the child is handed this as $PT_PANE_TOKEN. */
  c->pane_token = pt_agent_session_token_new();

  /* max_scrollback is a byte budget, not a line count — the header's wording
   * is wrong, terminal/Screen.zig is not — so this is 10MB by default, the
   * same number ghostty's scrollback-limit carries. */
  GhosttyTerminalOptions opts = { .cols = cols, .rows = rows,
                                  .max_scrollback = default_scrollback };
  if (ghostty_terminal_new(NULL, &c->terminal, opts) != GHOSTTY_SUCCESS ||
      ghostty_render_state_new(NULL, &c->render_state) != GHOSTTY_SUCCESS ||
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

  /* One resolution, two consumers (the exec below and the cached name after
   * it); see resolve_shell. Resolved against env_pairs because the child
   * putenv's them before the old in-child resolution ran, so an env_pairs
   * SHELL override changes what gets exec'd. */
  const char *shell = argv == NULL ? resolve_shell(c->env_pairs) : NULL;
  c->pty_fd = spawn_pty(cwd, argv, shell, c->env_pairs, c->pane_token,
                        cols, rows, cell_w, cell_h, &c->child);
  if (c->pty_fd < 0) {
    g_set_error(error, g_quark_from_static_string("pt-term-core"), 2,
                "forkpty failed: %s", g_strerror(errno));
    pt_term_core_free(c);
    return NULL;
  }
  /* Conservative until the first poll (at the end of this function) reads the
   * tty: a spawn with a command is running it right now. */
  c->fg_running = TRUE;
  /* Named from the very resolution the exec consumed, never read back from
   * the child: comm only settles after the exec, so a /proc read here can
   * race it and cache the parent's own name instead. */
  c->shell_name = g_path_get_basename(argv != NULL ? argv[0] : shell);

  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_USERDATA, c);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                       (const void *)effect_write_pty);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_SIZE,
                       (const void *)effect_size);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                       (const void *)effect_device_attributes);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
                       (const void *)effect_color_scheme);
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

void pt_term_core_set_scrollback_limit(gsize bytes) {
  default_scrollback = bytes;
}

void pt_term_core_set_padding(PtTermCore *c, int x, int y) {
  c->pad_x = x;
  c->pad_y = y;
}

void pt_term_core_set_color_scheme(PtTermCore *c, gboolean dark) {
  dark = dark ? TRUE : FALSE;      /* the compare below is on the value */
  /* Ghostty dedupes a layer up, on the state (Surface.zig:4958), and its
   * notification path deliberately does not: every config reload re-pings mode
   * 2031 unconditionally, colors changed or not (stream_handler.zig:120-122).
   * pt cannot copy that. Its one theme-apply path runs on every settings-dialog
   * preview step (pt-window.c:92-95), so an unconditional re-ping would write
   * to the child on every arrow keypress in that dialog. Dedupe here instead,
   * where every caller passes through. */
  if (c->dark == dark) return;
  c->dark = dark;
  if (c->pty_fd < 0 || c->child_exited) return;

  /* The notification is mode-gated; the answer to a direct query above is not.
   * Unlike modes 1004 and 2048 there is no enable-time report to arrange:
   * ghostty sends nothing when an app turns 2031 on (report_color_scheme is
   * read in exactly one place tree-wide, Termio.zig:712), leaving the app to
   * ask with CSI ? 996 n if it wants the value at startup. So no edge poll. */
  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_COLOR_SCHEME_REPORT, &on);
  if (!on) return;
  /* libghostty-vt has no encoder for this one — the bytes only exist as string
   * literals inside ghostty (stream_terminal.zig:328-331, pinned by its own
   * tests at :1885 and :1914) — so pt writes them itself. */
  const char *seq = dark ? "\x1b[?997;1n" : "\x1b[?997;2n";
  pty_write_raw(c->pty_fd, seq, strlen(seq));
}

/* Theme colors pushed into libghostty: the ANSI slots the theme pins (so
 * status output matches the app chrome) plus the default bg/fg/cursor. Slots
 * the theme leaves alone (alpha 0) keep libghostty's built-in defaults. */
void pt_term_core_set_colors(PtTermCore *c, const PtTermColors *colors) {
  /* Without these the render state reports libghostty's unset defaults
   * (black on white) and the theme's bg/fg would never reach the screen.
   * Set first: they must land even if the palette read below fails. */
  GhosttyColorRgb bg = { colors->bg.r, colors->bg.g, colors->bg.b };
  GhosttyColorRgb fg = { colors->fg.r, colors->fg.g, colors->fg.b };
  GhosttyColorRgb cursor = { colors->cursor.r, colors->cursor.g,
                             colors->cursor.b };
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);
  c->content_serial++;

  /* Reset to libghostty's stock palette before reading it: on a theme
   * switch DATA_COLOR_PALETTE_DEFAULT would otherwise still report the
   * *previous* theme's pins (our own last OPT_COLOR_PALETTE write became
   * the new default), so slots the incoming theme leaves alone would keep
   * the old theme's colors and diverge from a freshly created pane.
   * set(COLOR_PALETTE, NULL) restores the stock defaults while preserving
   * any OSC 4 overrides the program set, which the dirty mask tracks. */
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, NULL);
  GhosttyColorRgb palette[256];
  if (ghostty_terminal_get(c->terminal,
                           GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT,
                           palette) != GHOSTTY_SUCCESS)
    return;
  for (int i = 0; i < 16; i++)
    if (colors->palette[i].a > 0)
      palette[i] = (GhosttyColorRgb){ colors->palette[i].r,
                                      colors->palette[i].g,
                                      colors->palette[i].b };
  ghostty_terminal_set(c->terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE,
                       palette);
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
   * cheaper one.
   *
   * The other half of the trade: the library can move its own grid behind pt's
   * back, DECCOLM pinning it at 80 or 132 columns (Terminal.zig:2854-2879)
   * while pt's cols/rows still say what the pane allocated. Before this guard
   * the next layout pass called through with unchanged numbers and snapped the
   * grid back; now it returns early and the mismatch stands until the pane
   * really changes shape. Ghostty has the identical hole one level up
   * (Surface.zig:2465-2476), and matching it is the point — pt tracks the
   * re-sync separately rather than inventing a divergence here. */
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
  /* A reflow rewrites the row count and the visible area both. */
  c->sb_dirty = TRUE;
  c->content_serial++;
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
  c->sb_dirty = TRUE;
  c->content_serial++;
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
}

void pt_term_core_scroll_bottom(PtTermCore *c) {
  /* Every keypress snaps to the bottom, where the viewport nearly always
   * already is: nothing would move, so skip the redraw. Only a clean cache
   * can say so without paying the library's expensive scrollbar walk; a
   * dirty one lets the scroll go through, which is what it did before. */
  if (c->sb_valid && !c->sb_dirty && c->sb.offset + c->sb.len == c->sb.total)
    return;
  GhosttyTerminalScrollViewport sv = { .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM };
  ghostty_terminal_scroll_viewport(c->terminal, sv);
  c->sb_dirty = TRUE;
  c->content_serial++;
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
}

/* ---- scrollbar ----
 *
 * The cached read the header describes. The dirty flag is set wherever the two
 * things that can move the numbers happen: bytes reaching the parser, and the
 * viewport being moved, resized or reset. Everything else — keys, mouse
 * reports, pastes — reaches the terminal by way of the pty and comes back
 * through the read path, so it is covered by the first of those.
 *
 * A stale read is impossible in the other direction: a caller that never asks
 * costs nothing, and a caller that asks on every frame pays the library's walk
 * only on the frames where something actually changed. */
gboolean pt_term_core_scrollbar(PtTermCore *c, guint64 *total, guint64 *offset,
                                guint64 *len) {
  if (!c->sb_valid || c->sb_dirty) {
    GhosttyTerminalScrollbar sb = {0};
    if (ghostty_terminal_get(c->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR,
                             &sb) != GHOSTTY_SUCCESS)
      return FALSE;
    c->sb = sb;
    c->sb_valid = TRUE;
    c->sb_dirty = FALSE;
    c->sb_reads++;
  }
  if (total != NULL) *total = c->sb.total;
  if (offset != NULL) *offset = c->sb.offset;
  if (len != NULL) *len = c->sb.len;
  return TRUE;
}

guint64 pt_term_core_scrollbar_reads(PtTermCore *c) { return c->sb_reads; }

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
  double cx = (px - c->pad_x) / (double)c->cell_w;
  double cy = (py - c->pad_y) / (double)c->cell_h;
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
  c->content_serial++;      /* selected cells draw differently */
  /* The draw callback owns queueing the repaint, here as everywhere the core
   * changes what a frame shows — consumers do not queue after selection calls. */
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
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
  c->content_serial++;
  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);   /* as in sel_install */
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

char *pt_term_core_link_at_cell(PtTermCore *c, int row, int col) {
  if (row < 0 || col < 0 || row >= c->rows || col >= c->cols) return NULL;
  GhosttyGridRef ref;
  if (!sel_ref_at(c, (uint16_t)col, (uint16_t)row, &ref)) return NULL;
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

char *pt_term_core_hyperlink_at(PtTermCore *c, double px, double py) {
  /* Not clamped to the grid the way a selection drag is: the padding around it
   * is not part of any cell, and clamping there would make the edge column's
   * link openable from outside it. */
  double cx = (px - c->pad_x) / (double)c->cell_w;
  double cy = (py - c->pad_y) / (double)c->cell_h;
  if (cx < 0 || cy < 0 || cx >= c->cols || cy >= c->rows) return NULL;
  return pt_term_core_link_at_cell(c, (int)cy, (int)cx);
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
  size.screen_width = (uint32_t)(c->cols * c->cell_w + 2 * c->pad_x);
  size.screen_height = (uint32_t)(c->rows * c->cell_h + 2 * c->pad_y);
  size.cell_width = (uint32_t)c->cell_w;
  size.cell_height = (uint32_t)c->cell_h;
  size.padding_left = size.padding_right = c->pad_x;
  size.padding_top = size.padding_bottom = c->pad_y;
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

/* Room for one encoded report. SGR, the longest of the formats, spends about
 * sixteen bytes on a four-digit cell; the slack is free, it is a stack byte. */
#define PT_MOUSE_REPORT_MAX 128

static void mouse_event_fill(PtTermCore *c, GhosttyMouseAction action,
                             GhosttyMouseButton button, GhosttyMods mods,
                             double px, double py) {
  ghostty_mouse_event_set_action(c->mouse_event, action);
  if (button != GHOSTTY_MOUSE_BUTTON_UNKNOWN)
    ghostty_mouse_event_set_button(c->mouse_event, button);
  else
    ghostty_mouse_event_clear_button(c->mouse_event);
  ghostty_mouse_event_set_mods(c->mouse_event, mods);
  ghostty_mouse_event_set_position(c->mouse_event,
                                   (GhosttyMousePosition){ .x = (float)px,
                                                           .y = (float)py });
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
  /* A drag is motion *with* a button, and callers hand motion over without
   * naming one: the pointer moved, and which button is held is the core's
   * business, not the widget's. Ghostty answers it the same way — "we use the
   * first mouse button we find pressed in order to report" (Surface.zig:4628),
   * the spec naming none — and the answer is what separates a drag from a
   * hover. Under mode 1002 the encoder drops buttonless motion outright
   * (input/mouse_encode.zig shouldReport, `.button => event.button != null`)
   * and under 1003 encodes it as code 35, so leaving the button off means an
   * app on 1002 sees a press, a release, and nothing that ever selected. */
  if (action == GHOSTTY_MOUSE_ACTION_MOTION &&
      button == GHOSTTY_MOUSE_BUTTON_UNKNOWN && c->buttons_down != 0)
    button = (GhosttyMouseButton)g_bit_nth_lsf(c->buttons_down, -1);
  mouse_encoder_sync(c, c->buttons_down != 0);
  mouse_event_fill(c, action, button, mods, px, py);

  char buf[PT_MOUSE_REPORT_MAX];
  size_t written = 0;
  if (ghostty_mouse_encoder_encode(c->mouse_encoder, c->mouse_event, buf,
                                   sizeof(buf), &written) != GHOSTTY_SUCCESS ||
      written == 0)
    return FALSE;
  pty_write_raw(c->pty_fd, buf, written);
  return TRUE;
}

gboolean pt_term_core_mouse_cancel(PtTermCore *c, GhosttyMods mods,
                                   double px, double py) {
  gboolean reported = FALSE;
  while (c->buttons_down != 0) {
    GhosttyMouseButton button =
        (GhosttyMouseButton)g_bit_nth_lsf(c->buttons_down, -1);
    if (pt_term_core_mouse_report(c, GHOSTTY_MOUSE_ACTION_RELEASE, button,
                                  mods, px, py))
      reported = TRUE;
    /* mouse_report clears the bit before encoding — but not on its dead-pty
     * early return, so clear by hand too or that path never leaves the loop. */
    c->buttons_down &= ~(1u << button);
  }
  return reported;
}

gboolean pt_term_core_wheel_report(PtTermCore *c, GhosttyMouseButton button,
                                   GhosttyMods mods, double px, double py,
                                   int notches) {
  if (c->child_exited || c->pty_fd < 0 || notches <= 0) return FALSE;

  /* Synced once for the whole burst. Nothing the encoder reads can move while
   * this runs — no pty bytes are parsed between the notches — so the reports
   * come out byte-identical to the old sync-and-write-per-notch loop. The
   * wheel is a press, and the encoder's cell dedupe only ever suppresses
   * motion (input/mouse_encode.zig:108), so repeats are not swallowed.
   *
   * The wheel is deliberately *not* recorded in buttons_down. Ghostty reports
   * a scroll as a button-four/five press without touching its click state
   * (Surface.zig:3565 reports, 3763 is the only writer of click_state), so
   * any-button-pressed stays false. Routing this through the plain
   * mouse_report path used to set the bit and never clear it — no wheel
   * release exists — which left every later out-of-viewport motion looking
   * like a drag. */
  mouse_encoder_sync(c, c->buttons_down != 0);
  mouse_event_fill(c, GHOSTTY_MOUSE_ACTION_PRESS, button, mods, px, py);

  /* One write for the burst instead of one syscall per notch. Four notches
   * cover any wheel event; a touchpad flick can ask for more. */
  char stack[4 * PT_MOUSE_REPORT_MAX];
  gsize cap = (gsize)notches * PT_MOUSE_REPORT_MAX;
  char *heap = cap > sizeof(stack) ? g_malloc(cap) : NULL;
  char *buf = heap != NULL ? heap : stack;

  gsize len = 0;
  for (int i = 0; i < notches; i++) {
    size_t written = 0;
    if (ghostty_mouse_encoder_encode(c->mouse_encoder, c->mouse_event,
                                     buf + len, PT_MOUSE_REPORT_MAX,
                                     &written) != GHOSTTY_SUCCESS)
      break;
    len += written;
  }
  if (len > 0) pty_write_raw(c->pty_fd, buf, len);
  g_free(heap);
  return len > 0;
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
  /* One write for the whole burst instead of one syscall per row; a fast
   * wheel spills past the stack buffer, nothing else does. */
  gsize total = (gsize)count * 3;
  char stack[512];
  char *buf = total <= sizeof(stack) ? stack : g_malloc(total);
  for (gsize i = 0; i < total; i += 3) memcpy(buf + i, seq, 3);
  pty_write_raw(c->pty_fd, buf, total);
  if (buf != stack) g_free(buf);
}

gboolean pt_term_core_bracketed_paste(PtTermCore *c) {
  bool on = false;
  ghostty_terminal_mode_get(c->terminal, GHOSTTY_MODE_BRACKETED_PASTE, &on);
  return on;
}

/* Pastes up to this many bytes are encoded without touching the heap. A line
 * or two of shell — what nearly every paste is — stays on the stack. */
#define PT_PASTE_STACK_MAX 4096
/* "\x1b[200~" + "\x1b[201~", the only bytes bracketed mode adds
 * (input/paste.zig:95-99). Nothing else changes the length. */
#define PT_PASTE_MARKERS 12

void pt_term_core_paste(PtTermCore *c, const char *text, gssize len) {
  if (c->pty_fd < 0 || c->child_exited || text == NULL) return;
  size_t n = len < 0 ? strlen(text) : (size_t)len;
  if (n == 0) return;

  bool bracketed = pt_term_core_bracketed_paste(c);
  /* Two regions: ghostty_paste_encode rewrites the unsafe bytes in place, so
   * it needs a copy of the clipboard string (which is not ours to touch), and
   * then memcpys the segments into a separate output buffer. The encoded size
   * is known up front — the input length plus the markers — so one pass does
   * it; the old size-then-fill pair walked the same bytes twice. */
  size_t need = n + (bracketed ? PT_PASTE_MARKERS : 0);
  char stack[2 * PT_PASTE_STACK_MAX + PT_PASTE_MARKERS];
  char *heap = n > PT_PASTE_STACK_MAX ? g_malloc(n + need) : NULL;
  char *data = heap != NULL ? heap : stack;
  char *out = data + n;
  memcpy(data, text, n);

  size_t written = 0;
  GhosttyResult r = ghostty_paste_encode(data, n, bracketed, out, need,
                                         &written);
  if (r != GHOSTTY_SUCCESS) {
    /* Only reachable if the library ever grows an encoding that is longer
     * than the input plus the markers; it hands back the size it wants. The
     * in-place strip already ran and is idempotent, so the retry re-encodes
     * the same copy safely. */
    char *big = g_malloc(written);
    r = ghostty_paste_encode(data, n, bracketed, big, written, &written);
    if (r == GHOSTTY_SUCCESS) pty_write_raw(c->pty_fd, big, written);
    g_free(big);
  } else {
    pty_write_raw(c->pty_fd, out, written);
  }
  g_free(heap);
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

/* ---- full reset ---- */

/* ghostty's `reset` binding action, which is one call to fullReset() and
 * nothing else (Surface.zig:5129-5133): no pty write, no signal, no
 * confirmation, and nothing at all to the child. ghostty_terminal_reset() is
 * that same call (terminal/c/terminal.zig:524-527) — the three extras on
 * ghostty's RIS-from-stream path (stream_handler.zig:947-958) belong to the
 * escape sequence, not to the action, so they are not here either.
 *
 * The library puts the modes, both screens, the scrollback, the tabstops, the
 * scrolling region, the title and its own selection back to defaults, and snaps
 * the viewport to the active area on the way (PageList.zig:760), so there is no
 * scroll-to-bottom to do afterwards. Dimensions and colors survive.
 *
 * The rest of this is pt state that mirrors what the library just threw away.
 * Ghostty leaves its equivalents (Surface.mouse.*) stale across a reset; pt
 * does not, because the reason to reach for this command is a program that died
 * mid-sequence and left something held down. */
void pt_term_core_reset(PtTermCore *c) {
  /* Before the library reset, not after. Both orders end up with the mirror
   * clear — sel_active is still set afterwards, so the early-out would not
   * fire — but only this one releases the terminal's tracked selection while
   * there is still a selection to release. Run the other way round, the
   * OPT_SELECTION write lands on a terminal that has already dropped its own
   * (Screen.zig:408) and does nothing but flip pt's flag, and it does so after
   * PageList.reset has pointed every surviving tracked pin at the new first
   * page and marked it garbage (PageList.zig:743-753). The library unwinds its
   * selection out of that state itself; pt has no reason to lean on that. */
  pt_term_core_selection_clear(c);
  c->sel_dragging = FALSE;      /* a reset mid-drag ends the drag */
  c->sel_moved = FALSE;
  c->click_count = 0;
  c->last_press_ns = 0;         /* the next press starts a fresh click run */

  ghostty_terminal_reset(c->terminal);
  c->sb_dirty = TRUE;           /* the scrollback it just threw away */
  c->content_serial++;

  /* No ghostty precedent — it does not reset its VT parser either — but it has
   * no scanner of its own to reset. pt's runs beside the library parser and can
   * be parked mid-payload with up to a megabyte buffered, which is exactly the
   * wedge this command exists to clear. */
  pt_osc_scan_clear(&c->osc);

  /* The encoder's protocol options are re-synced per event (mouse_encoder_sync)
   * but its motion dedupe — the last cell it reported — is not; this is the
   * only call that clears it. */
  ghostty_mouse_encoder_reset(c->mouse_encoder);
  c->buttons_down = 0;          /* nothing is held after a reset */

  /* fullReset() puts the modes back to their defaults, so the edges polled off
   * the pty read are stale. They would self-correct on the next read; clearing
   * them means the state is never briefly wrong, and re-arms the enable-time
   * report for an app that turns either mode on again. */
  c->was_focus_event = FALSE;
  c->was_in_band_resize = FALSE;

  /* The color scheme is deliberately *not* cleared: it mirrors the theme pt is
   * painting, which a reset does not touch (colors survive fullReset), so the
   * next 996 query must still be answered with the truth. Nothing is reported
   * either — ghostty re-emits for mode 2031 after RIS from the stream
   * (stream_handler.zig:951-954), but that belongs to the escape sequence, not
   * to the reset action this is, and fullReset has just turned 2031 off
   * anyway, so there is nobody left listening. */

  if (c->cbs.draw != NULL) c->cbs.draw(c, c->cbs_user);
}

void pt_term_core_sync(PtTermCore *c) {
  ghostty_render_state_update(c->render_state, c->terminal);
}

/* Set wherever the terminal's content, viewport, colors, selection or shape
 * change: the pty read path, resize, the scroll calls, reset, set_colors and
 * the selection installs/clears. Everything else that mutates the terminal
 * reaches it by way of the pty and is covered by the read path. */
gboolean pt_term_core_take_render_dirty(PtTermCore *c) {
  gboolean dirty = c->content_serial != c->taken_serial;
  c->taken_serial = c->content_serial;
  return dirty;
}

guint pt_term_core_content_serial(PtTermCore *c) { return c->content_serial; }

/* ---- row walks ----
 *
 * Every walk over the synced render state allocates its own iterator pair, so
 * no two walks can trample each other and any of these may be called from
 * anywhere — a link lookup from inside a draw callback included. libghostty
 * populates a pre-allocated handle per walk (render.h: DATA_ROW_ITERATOR,
 * ROW_DATA_CELLS), so "local" still costs one small allocation each; both
 * objects are tiny. */
typedef struct {
  GhosttyRenderStateRowIterator iter;
  GhosttyRenderStateRowCells cells;
} PtRowWalk;

static void row_walk_end(PtRowWalk *w) {
  if (w->cells != NULL) ghostty_render_state_row_cells_free(w->cells);
  if (w->iter != NULL) ghostty_render_state_row_iterator_free(w->iter);
  w->cells = NULL;
  w->iter = NULL;
}

static gboolean row_walk_begin(PtRowWalk *w, GhosttyRenderState rs) {
  w->iter = NULL;
  w->cells = NULL;
  if (ghostty_render_state_row_iterator_new(NULL, &w->iter) == GHOSTTY_SUCCESS &&
      ghostty_render_state_row_cells_new(NULL, &w->cells) == GHOSTTY_SUCCESS &&
      ghostty_render_state_get(rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                               &w->iter) == GHOSTTY_SUCCESS)
    return TRUE;
  row_walk_end(w);
  return FALSE;
}

/* Advance to visible row `row` (0-based) and load its cells. The iterator is
 * forward-only, so this only moves down from wherever the walk stands. */
static gboolean row_walk_seek(PtRowWalk *w, int row) {
  for (int r = 0; r <= row; r++)
    if (!ghostty_render_state_row_iterator_next(w->iter)) return FALSE;
  return ghostty_render_state_row_get(w->iter,
                                      GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                      &w->cells) == GHOSTTY_SUCCESS;
}

/* ---- cursor shape and blink ----
 *
 * Straight reads off the last synced render state, all internal to
 * pt_term_core_cursor_info since Task 3 folded the renderer onto that one
 * call. Each falls back to what a terminal nobody has configured looks like,
 * so a failed query cannot turn the cursor into something stranger than a
 * steady block. */
static GhosttyRenderStateCursorVisualStyle cursor_style(PtTermCore *c) {
  GhosttyRenderStateCursorVisualStyle style =
      GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
                           &style);
  return style;
}

static gboolean cursor_blinking(PtTermCore *c) {
  bool on = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &on);
  return on;
}

static gboolean cursor_password_input(PtTermCore *c) {
  bool on = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_PASSWORD_INPUT,
                           &on);
  return on;
}

static gboolean cursor_wide_tail(PtTermCore *c) {
  /* Undefined unless the cursor is actually in the viewport, so the guard is
   * here rather than at every call site. */
  bool in_vp = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                           &in_vp);
  if (!in_vp) return FALSE;
  bool tail = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_WIDE_TAIL,
                           &tail);
  return tail;
}

static gboolean cursor_wide(PtTermCore *c) {
  bool in_vp = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                           &in_vp);
  if (!in_vp) return FALSE;
  uint16_t cx = 0, cy = 0;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);

  /* The render state answers the tail question and not this one, so the only
   * way to it is the cell itself: walk to the cursor's row, jump to its
   * column, ask the raw cell. One row walk, no grid copy. */
  PtRowWalk w;
  if (!row_walk_begin(&w, c->render_state)) return FALSE;
  gboolean wide = FALSE;
  GhosttyCell cell = 0;
  if (row_walk_seek(&w, cy) &&
      ghostty_render_state_row_cells_select(w.cells, cx) == GHOSTTY_SUCCESS &&
      ghostty_render_state_row_cells_get(
          w.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
          &cell) == GHOSTTY_SUCCESS) {
    GhosttyCellWide cw = GHOSTTY_CELL_WIDE_NARROW;
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &cw);
    wide = cw == GHOSTTY_CELL_WIDE_WIDE;
  }
  row_walk_end(&w);
  return wide;
}

gboolean pt_term_core_cursor_info(PtTermCore *c, PtCursorInfo *out) {
  out->style = (int)cursor_style(c);
  out->blinking = cursor_blinking(c);
  out->password = cursor_password_input(c);
  bool visible = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &visible);
  out->visible = visible;
  out->x = 0;
  out->y = 0;
  out->width = 1;

  /* The cursor color a program set with OSC 12, falling back to the default
   * foreground — resolved here so the renderer never asks the state twice. */
  GhosttyColorRgb cc = {0};
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND, &cc);
  bool cc_set = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR_HAS_VALUE,
                           &cc_set);
  if (cc_set)
    ghostty_render_state_get(c->render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR, &cc);
  out->color = (PtColor){ cc.r, cc.g, cc.b, 1.0 };

  bool in_vp = false;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
                           &in_vp);
  if (!in_vp) return FALSE;
  uint16_t cx = 0, cy = 0;
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cx);
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cy);
  out->x = cx;
  out->y = cy;
  /* A wide character owns two cells and the cursor has to cover both. Two
   * ways in, tested in ghostty's order (renderer/generic.zig:3232): on the
   * spacer tail back up onto the head, on the head just widen. The tail
   * answer is a render-state field; only the head question walks a row. */
  if (cursor_wide_tail(c) && cx > 0) {
    out->x = cx - 1;
    out->width = 2;
  } else if (cursor_wide(c)) {
    out->width = 2;
  }
  return TRUE;
}

void pt_term_core_default_colors(PtTermCore *c, PtColor *bg, PtColor *fg) {
  GhosttyColorRgb rgb = {0};
  if (bg != NULL &&
      ghostty_render_state_get(c->render_state,
                               GHOSTTY_RENDER_STATE_DATA_COLOR_BACKGROUND,
                               &rgb) == GHOSTTY_SUCCESS)
    *bg = (PtColor){ rgb.r, rgb.g, rgb.b, 1.0 };
  if (fg != NULL &&
      ghostty_render_state_get(c->render_state,
                               GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND,
                               &rgb) == GHOSTTY_SUCCESS)
    *fg = (PtColor){ rgb.r, rgb.g, rgb.b, 1.0 };
}

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

static char *grid_text(GhosttyRenderState rs) {
  GString *out = g_string_new(NULL);
  PtRowWalk w;
  if (!row_walk_begin(&w, rs)) return g_string_free(out, FALSE);
  while (ghostty_render_state_row_iterator_next(w.iter)) {
    if (ghostty_render_state_row_get(w.iter,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &w.cells) != GHOSTTY_SUCCESS)
      continue;
    /* Blank cells are counted, not appended: a run between non-blank cells is
       flushed in one grow+memset, and the run a row ends on — most of most
       rows — is simply dropped, which is the old byte-at-a-time trailing
       trim with no bytes to trim. */
    gsize row_start = out->len;
    gsize blanks = 0;
    while (ghostty_render_state_row_cells_next(w.cells)) {
      uint32_t glen = 0;
      ghostty_render_state_row_cells_get(w.cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
      if (glen == 0) { blanks++; continue; }
      if (blanks > 0) {
        gsize at = out->len;
        g_string_set_size(out, at + blanks);
        memset(out->str + at, ' ', blanks);
        blanks = 0;
      }
      /* GRAPHEMES_BUF writes ALL glen codepoints; the buffer must hold glen.
         Use a stack buffer for the common case, heap for long clusters. */
      uint32_t cps_stack[16];
      uint32_t *cps = glen <= 16 ? cps_stack : g_new(uint32_t, glen);
      ghostty_render_state_row_cells_get(w.cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      for (uint32_t i = 0; i < glen; i++) {
        char u8[4];
        g_string_append_len(out, u8, utf8_encode_cp(cps[i], u8));
      }
      if (cps != cps_stack) g_free(cps);
    }
    /* A row can still end in spaces a program wrote (glen 1, ' '); trim those
       as before — rare enough that byte-at-a-time costs nothing here. */
    while (out->len > row_start && out->str[out->len - 1] == ' ')
      g_string_truncate(out, out->len - 1);
    g_string_append_c(out, '\n');
  }
  row_walk_end(&w);
  return g_string_free(out, FALSE);
}

char *pt_term_core_grid_text(PtTermCore *c) {
  return grid_text(c->render_state);
}

/* One PtCell from the walk's current cell. `fg_default` is the render state's
 * default foreground, resolved here so a consumer never asks twice. Zeroed
 * first, so every byte of the struct — padding and the text tail past the NUL
 * included — is deterministic and two fills of the same cell compare equal
 * bytewise, whatever the buffer held before. */
static void fill_cell(PtCell *out, GhosttyRenderStateRowCells cells,
                      GhosttyColorRgb fg_default) {
  memset(out, 0, sizeof *out);
  uint32_t glen = 0;
  ghostty_render_state_row_cells_get(cells,
      GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
  int pos = 0;
  if (glen > 0) {
    uint32_t cps_stack[16];
    uint32_t *cps = glen <= 16 ? cps_stack : g_new(uint32_t, glen);
    ghostty_render_state_row_cells_get(cells,
        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
    /* Codepoints past the text cap are dropped whole: a cluster that long is
     * far past anything a real grapheme carries, and a partial UTF-8 byte
     * must never land in text. utf8_encode_cp turns invalid ones to U+FFFD. */
    for (uint32_t i = 0; i < glen; i++) {
      char u8[4];
      int n = utf8_encode_cp(cps[i], u8);
      if (pos + n > PT_CELL_TEXT_MAX - 1) break;
      memcpy(out->text + pos, u8, (gsize)n);
      pos += n;
    }
    if (cps != cps_stack) g_free(cps);
  }
  out->text[pos] = '\0';

  GhosttyCell cell = 0;
  GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
  bool linked = false;
  if (ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &cell) == GHOSTTY_SUCCESS) {
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
    ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &linked);
  }
  out->has_link = linked;
  /* Spacer heads (end-of-line stubs before a wrapped wide char) hold nothing
   * to draw either, so they report 0 with the tails. */
  out->width = wide == GHOSTTY_CELL_WIDE_WIDE     ? 2
             : wide == GHOSTTY_CELL_WIDE_NARROW   ? 1
                                                  : 0;

  GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
  ghostty_render_state_row_cells_get(cells,
      GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
  out->style = (guint8)((style.bold ? PT_CELL_STYLE_BOLD : 0) |
                        (style.italic ? PT_CELL_STYLE_ITALIC : 0) |
                        (style.underline != GHOSTTY_SGR_UNDERLINE_NONE
                             ? PT_CELL_STYLE_UNDERLINE : 0) |
                        (style.strikethrough ? PT_CELL_STYLE_STRIKE : 0) |
                        (style.faint ? PT_CELL_STYLE_FAINT : 0) |
                        (style.inverse ? PT_CELL_STYLE_INVERSE : 0));

  bool selected = false;
  ghostty_render_state_row_cells_get(cells,
      GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED, &selected);
  out->selected = selected;

  GhosttyColorRgb bg = {0};
  out->has_bg = ghostty_render_state_row_cells_get(cells,
      GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
  out->bg = out->has_bg ? (PtColor){ bg.r, bg.g, bg.b, 1.0 } : (PtColor){0};

  GhosttyColorRgb fg = fg_default;
  if (ghostty_render_state_row_cells_get(cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg) != GHOSTTY_SUCCESS)
    fg = fg_default;
  out->fg = (PtColor){ fg.r, fg.g, fg.b, 1.0 };
}

int pt_term_core_row_cells(PtTermCore *c, int row, PtCell *out, int max) {
  if (row < 0 || out == NULL || max <= 0) return 0;
  PtRowWalk w;
  if (!row_walk_begin(&w, c->render_state)) return 0;
  int n = 0;
  if (row_walk_seek(&w, row)) {
    GhosttyColorRgb fg_default = {0};
    ghostty_render_state_get(c->render_state,
                             GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND,
                             &fg_default);
    while (n < max && ghostty_render_state_row_cells_next(w.cells))
      fill_cell(&out[n++], w.cells, fg_default);
  }
  row_walk_end(&w);
  return n;
}

/* ---- sequential row walk ----
 *
 * The frame-shaped read: one iterator pair for the whole pass instead of the
 * per-call seek row_cells pays, so a full repaint is O(rows) iterator steps
 * rather than O(rows²). Each next() fills exactly what row_cells would for
 * that row — both go through fill_cell with the same resolved default fg.
 *
 * The reader owns the row buffer and grows it to the widest row it meets, so
 * no caller ever guesses a column count and no width is ever truncated — an
 * ultrawide pane at a small font can pass 512 columns, and DECCOLM can move
 * the grid's width away from what the pty was told. */
struct PtRowReader {
  PtRowWalk w;
  GhosttyColorRgb fg_default;
  PtCell *cells;              /* owned; handed out by rows_next */
  int cap;
};

PtRowReader *pt_term_core_rows_begin(PtTermCore *c) {
  PtRowReader *r = g_new0(PtRowReader, 1);
  if (!row_walk_begin(&r->w, c->render_state)) {
    g_free(r);
    return NULL;
  }
  ghostty_render_state_get(c->render_state,
                           GHOSTTY_RENDER_STATE_DATA_COLOR_FOREGROUND,
                           &r->fg_default);
  /* Seeded from the pty size so the common case never reallocates; the walk
   * below still grows past it whenever a row turns out wider. */
  r->cap = MAX(c->cols, 1);
  r->cells = g_new(PtCell, r->cap);
  return r;
}

int pt_term_core_rows_next(PtRowReader *r, const PtCell **out) {
  *out = r->cells;
  if (!ghostty_render_state_row_iterator_next(r->w.iter)) return -1;
  if (ghostty_render_state_row_get(r->w.iter,
                                   GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                   &r->w.cells) != GHOSTTY_SUCCESS)
    return 0;                 /* this row is unreadable; the walk goes on */
  int n = 0;
  while (ghostty_render_state_row_cells_next(r->w.cells)) {
    if (n == r->cap) {
      r->cap *= 2;
      r->cells = g_renew(PtCell, r->cells, r->cap);
    }
    fill_cell(&r->cells[n++], r->w.cells, r->fg_default);
  }
  *out = r->cells;            /* again: the grow may have moved the buffer */
  return n;
}

void pt_term_core_rows_end(PtRowReader *r) {
  if (r == NULL) return;
  row_walk_end(&r->w);
  g_free(r->cells);
  g_free(r);
}

gboolean pt_term_core_row_has_link(PtTermCore *c, int row) {
  if (row < 0) return FALSE;
  PtRowWalk w;
  if (!row_walk_begin(&w, c->render_state)) return FALSE;
  gboolean linked = FALSE;
  gboolean found = TRUE;
  for (int r = 0; r <= row && found; r++)
    found = ghostty_render_state_row_iterator_next(w.iter);
  if (found) {
    GhosttyRow raw = 0;
    if (ghostty_render_state_row_get(w.iter, GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                                     &raw) == GHOSTTY_SUCCESS) {
      bool l = false;
      ghostty_row_get(raw, GHOSTTY_ROW_DATA_HYPERLINK, &l);
      linked = l;
    }
  }
  row_walk_end(&w);
  return linked;
}

/* ---- logical lines ----
 *
 * Two walks rather than one: the wrap flags decide which rows belong to the
 * line, and the iterator only moves forward, so the group has to be known
 * before any of it can be rendered. This runs when the pointer changes cell
 * with the link modifier down — hover rate, not frame rate — which is the
 * same budget ghostty spends selecting a line per hover. */
gboolean pt_term_core_line_at(PtTermCore *c, int row, PtLine *out) {
  if (out == NULL || row < 0 || row >= c->rows || c->cols <= 0) return FALSE;

  /* Pass 1: which rows a program wrapped into which. The flag means "this row
   * continues onto the next", so the group runs back while the row above says
   * yes and forward while this one does. */
  gboolean *wrap = g_new0(gboolean, c->rows);
  PtRowWalk w;
  if (!row_walk_begin(&w, c->render_state)) {
    g_free(wrap);
    return FALSE;
  }
  for (int r = 0; r < c->rows; r++) {
    if (!ghostty_render_state_row_iterator_next(w.iter)) break;
    GhosttyRow raw = 0;
    if (ghostty_render_state_row_get(w.iter, GHOSTTY_RENDER_STATE_ROW_DATA_RAW,
                                     &raw) != GHOSTTY_SUCCESS)
      continue;
    bool v = false;
    ghostty_row_get(raw, GHOSTTY_ROW_DATA_WRAP, &v);
    wrap[r] = v;
  }
  row_walk_end(&w);

  int first = row, last = row;
  while (first > 0 && wrap[first - 1]) first--;
  while (last < c->rows - 1 && wrap[last]) last++;
  g_free(wrap);

  /* Pass 2: the group as one string, every byte tagged with the cell it came
   * from. Blank cells become spaces instead of being trimmed — a URL is found
   * by what surrounds it, and a run of nothing is as good a boundary as a
   * space is. */
  GString *text = g_string_sized_new((gsize)(last - first + 1) * c->cols + 1);
  GArray *at = g_array_sized_new(FALSE, FALSE, sizeof(PtCellPos),
                                 (guint)((last - first + 1) * c->cols));
  PtCell *cells = g_new(PtCell, c->cols);
  for (int r = first; r <= last; r++) {
    int n = pt_term_core_row_cells(c, r, cells, c->cols);
    for (int col = 0; col < n; col++) {
      /* The spacer half of a wide character drew nothing, so it contributes
       * no byte: its head cell already carries the whole cluster. */
      if (cells[col].width == 0) continue;
      const char *s = cells[col].text[0] != '\0' ? cells[col].text : " ";
      PtCellPos pos = { (gint16)r, (gint16)col };
      for (const char *p = s; *p != '\0'; p++) g_array_append_val(at, pos);
      g_string_append(text, s);
    }
  }
  g_free(cells);

  out->len = text->len;
  out->text = g_string_free(text, FALSE);
  out->at = (PtCellPos *)g_array_free(at, FALSE);
  return TRUE;
}

void pt_term_core_line_clear(PtLine *l) {
  if (l == NULL) return;
  g_clear_pointer(&l->text, g_free);
  g_clear_pointer(&l->at, g_free);
  l->len = 0;
}

gboolean pt_term_core_last_nonempty_row(PtTermCore *c, char *buf, gsize cap) {
  if (cap == 0) return FALSE;
  buf[0] = '\0';

  /* The row iterator only walks forward, so two passes: find the last row
   * holding a non-blank cell, then re-walk to it and render it. Rendering as
   * we scan would need a per-row buffer to survive the blank rows after it;
   * this path allocates nothing beyond the walk's own iterator pair. */
  PtRowWalk w;
  if (!row_walk_begin(&w, c->render_state)) return FALSE;
  gssize last = -1;
  for (gssize row = 0; ghostty_render_state_row_iterator_next(w.iter); row++) {
    if (ghostty_render_state_row_get(w.iter,
                                     GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                     &w.cells) != GHOSTTY_SUCCESS)
      continue;
    while (ghostty_render_state_row_cells_next(w.cells)) {
      uint32_t glen = 0;
      ghostty_render_state_row_cells_get(w.cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
      if (glen > 0) { last = row; break; }
    }
  }
  /* Rewind for the second pass: re-populating the iterator from the render
   * state puts it back at the top. */
  if (last < 0 ||
      ghostty_render_state_get(c->render_state,
                               GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                               &w.iter) != GHOSTTY_SUCCESS ||
      !row_walk_seek(&w, (int)last)) {
    row_walk_end(&w);
    return FALSE;
  }

  gsize len = 0;    /* bytes written */
  gsize keep = 0;   /* len as of the last non-blank cell: the trailing-blank cut */
  while (ghostty_render_state_row_cells_next(w.cells)) {
    uint32_t glen = 0;
    ghostty_render_state_row_cells_get(w.cells,
        GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &glen);
    if (glen == 0) {
      if (len + 1 >= cap) break;    /* full; whatever remains is dropped */
      buf[len++] = ' ';
      continue;
    }
    /* GRAPHEMES_BUF writes all glen codepoints and there is no heap to spill
     * to here, so a cluster too long for the stack buffer renders as U+FFFD —
     * far past anything a real grapheme carries (grid_text's common case is
     * 16), and the row still counts as non-blank. */
    uint32_t cps[64];
    if (glen <= G_N_ELEMENTS(cps)) {
      ghostty_render_state_row_cells_get(w.cells,
          GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
    } else {
      cps[0] = 0xFFFD;
      glen = 1;
    }
    gboolean full = FALSE;
    for (uint32_t i = 0; i < glen; i++) {
      char u8[4];
      int n = utf8_encode_cp(cps[i], u8);
      if (len + (gsize)n >= cap) { full = TRUE; break; }
      memcpy(buf + len, u8, (gsize)n);
      len += (gsize)n;
    }
    keep = len;
    if (full) break;
  }
  /* Spaces a program wrote (glen 1, ' ') are trailing blanks too. */
  while (keep > 0 && buf[keep - 1] == ' ') keep--;
  buf[keep] = '\0';
  row_walk_end(&w);
  return TRUE;
}

char *pt_term_grid_text_raw(GhosttyTerminal t) {
  GhosttyRenderState rs = NULL;
  char *out = NULL;
  if (ghostty_render_state_new(NULL, &rs) == GHOSTTY_SUCCESS) {
    ghostty_render_state_update(rs, t);
    out = grid_text(rs);
  }
  if (rs != NULL) ghostty_render_state_free(rs);
  return out;
}

gboolean pt_term_core_exited(PtTermCore *c, int *status) {
  if (status != NULL) *status = c->exit_status;
  return c->child_exited;
}

pid_t pt_term_core_shell_pid(PtTermCore *c) { return c->child; }

const char *pt_term_core_shell_name(PtTermCore *c) { return c->shell_name; }

const char *pt_term_core_pane_token(PtTermCore *c) { return c->pane_token; }

/* A field read: the 700ms foreground poll keeps it current, spawn seeds it
 * TRUE and child exit clears it, so per-frame callers cost no syscall. */
gboolean pt_term_core_running(PtTermCore *c) {
  return c->fg_running;
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
  if (c->render_state != NULL) ghostty_render_state_free(c->render_state);
  if (c->terminal != NULL) ghostty_terminal_free(c->terminal);
  if (c->pane_token != NULL) {
    /* A closed pane's report must not outlive it: the next save would read
     * a session for a pane that no longer exists. Crash leftovers are the
     * sweep's job; this is the orderly path. */
    char *rp = pt_agent_session_report_path(c->pane_token);
    g_unlink(rp);
    g_free(rp);
    g_free(c->pane_token);
  }
  g_strfreev(c->env_pairs);
  g_free(c->last_title);
  g_free(c->shell_name);
  g_free(c);
}
