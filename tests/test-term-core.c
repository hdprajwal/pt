#include "pt-term-core.h"
#include "pt-term-core-internal.h"   /* pt_osc52_decode, the OSC 52 caps */
#include "pt-config.h"
#include <stdio.h>
#include <string.h>

typedef struct { GMainLoop *loop; PtTermCore *core;
                 gboolean found; int exit_status; gboolean exited;
                 char comm[64]; char title[128];
                 int osc_code; char osc_payload[128]; int osc_count;
                 char clip[256]; gsize clip_len; gboolean clip_primary;
                 int clip_count;
                 int draw_count, output_count; } Ctx;

static void on_draw(PtTermCore *core, gpointer user) {
  Ctx *ctx = user;
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  if (text != NULL && strstr(text, "hello-from-pt") != NULL) {
    ctx->found = TRUE;
    g_main_loop_quit(ctx->loop);
  }
  g_free(text);
}

static void on_exit_cb(PtTermCore *core, int status, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  ctx->exited = TRUE;
  ctx->exit_status = status;
  g_main_loop_quit(ctx->loop);
}

static gboolean on_timeout(gpointer user) {
  g_main_loop_quit(((Ctx *)user)->loop);
  return G_SOURCE_REMOVE;
}

static void test_output_reaches_grid(void) {
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c", "printf 'hello-from-pt\\n'; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  pt_term_core_free(core);   /* must kill+reap the sleeping child */
  g_main_loop_unref(ctx.loop);
}

static void test_exit_status_reported(void) {
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c", "exit 7", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  PtTermCoreCallbacks cbs = { .exited = on_exit_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.exited);
  g_assert_cmpint(ctx.exit_status, ==, 7);
  int st = -1;
  g_assert_true(pt_term_core_exited(core, &st));
  g_assert_cmpint(st, ==, 7);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void test_key_send_echoes(void) {
  /* `cat` echoes stdin; typing 'h' 'i' Enter must appear in the grid. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/cat", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  ctx.core = core;
  PtTermCoreCallbacks cbs = {0};
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  pt_term_core_send_key(core, GHOSTTY_KEY_H, GHOSTTY_KEY_ACTION_PRESS, 0, 'h', "h", 1);
  pt_term_core_send_key(core, GHOSTTY_KEY_I, GHOSTTY_KEY_ACTION_PRESS, 0, 'i', "i", 1);
  /* Pump the loop until the echo comes back. */
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  gboolean ok = FALSE;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    pt_term_core_sync(core);
    char *text = pt_term_core_grid_text(core);
    ok = (text != NULL && strstr(text, "hi") != NULL);
    g_free(text);
    if (ok) break;
    g_usleep(10000);
  }
  g_assert_true(ok);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void on_draw_marker(PtTermCore *core, gpointer user) {
  Ctx *ctx = user;
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  if (text != NULL && strstr(text, "done-marker") != NULL) {
    ctx->found = TRUE;
    g_main_loop_quit(ctx->loop);
  }
  g_free(text);
}

static void test_long_grapheme_cluster(void) {
  /* Regression: a single cell holding a base char + 19 combining marks is
     >16 codepoints. GRAPHEMES_BUF writes all of them, so grid_text must not
     overflow its stack buffer. UTF-8 bytes are inlined (shell printf \u is
     not portable): 'A' U+0041 then combining U+0301..U+0313. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '%s\\n' 'A"
    "\xCC\x81\xCC\x82\xCC\x83\xCC\x84\xCC\x85\xCC\x86\xCC\x87\xCC\x88\xCC\x89"
    "\xCC\x8A\xCC\x8B\xCC\x8C\xCC\x8D\xCC\x8E\xCC\x8F\xCC\x90\xCC\x91\xCC\x92"
    "\xCC\x93 done-marker'; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_nonnull(strstr(text, "A"));
  g_free(text);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void test_selection(void) {
  /* Print a known word, then programmatically drag-select across it and assert
     pt_term_core_selection_text returns it. Cells are 8x16 with the grid
     inset by PT_PAD_X=20 / PT_PAD_Y=18, so column N spans pixels
     [20 + 8N, 20 + 8N + 8) and row 0 spans [18, 34). "SELECTME" lands at
     row 0, columns 0..7. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf 'SELECTME done-marker\\n'; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  pt_term_core_sync(core);

  /* No selection yet → NULL. */
  g_assert_null(pt_term_core_selection_text(core));

  /* Drag from the first cell of "SELECTME" to just past its last char.
     Anchor at col 0 (px ~21), release near col 8 (px 20 + 8*8 = 84). */
  pt_term_core_selection_press(core, 21.0, 20.0, 1000000000ULL);
  pt_term_core_selection_drag(core, 84.0, 20.0);
  pt_term_core_selection_release(core, 84.0, 20.0);

  char *sel = pt_term_core_selection_text(core);
  g_assert_nonnull(sel);
  g_assert_nonnull(strstr(sel, "SELECTME"));
  g_assert_null(strstr(sel, "done-marker"));  /* stopped before the marker */
  g_free(sel);

  /* Double-click selects just the word under the pointer (col 2). */
  pt_term_core_selection_press(core, 36.0, 20.0, 2000000000ULL);
  pt_term_core_selection_press(core, 36.0, 20.0, 2000100000ULL);  /* +100us=word */
  pt_term_core_selection_release(core, 36.0, 20.0);
  char *word = pt_term_core_selection_text(core);
  g_assert_nonnull(word);
  g_assert_cmpstr(g_strstrip(word), ==, "SELECTME");
  g_free(word);

  /* Clear drops the selection. */
  pt_term_core_selection_clear(core);
  g_assert_null(pt_term_core_selection_text(core));

  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* ---- OSC 8 hyperlinks ---- */
static void test_hyperlink_is_safe(void) {
  g_assert_true(pt_term_core_hyperlink_is_safe("https://example.com/x"));
  g_assert_true(pt_term_core_hyperlink_is_safe("http://example.com"));
  g_assert_true(pt_term_core_hyperlink_is_safe("file:///tmp/a.txt"));
  g_assert_true(pt_term_core_hyperlink_is_safe("mailto:a@example.com"));
  /* The scheme is case-insensitive per RFC 3986, so the check has to be too —
     otherwise "JavaScript:" walks straight past a lowercase-only blocklist. */
  g_assert_true(pt_term_core_hyperlink_is_safe("HTTPS://example.com"));
  g_assert_true(pt_term_core_hyperlink_is_safe("MailTo:a@example.com"));

  g_assert_false(pt_term_core_hyperlink_is_safe("javascript:alert(1)"));
  g_assert_false(pt_term_core_hyperlink_is_safe("JavaScript:alert(1)"));
  g_assert_false(pt_term_core_hyperlink_is_safe("ssh://host/x"));
  g_assert_false(pt_term_core_hyperlink_is_safe("vscode://x"));
  g_assert_false(pt_term_core_hyperlink_is_safe("data:text/html,<script>"));
  /* No scheme at all: nothing decides what opens it. */
  g_assert_false(pt_term_core_hyperlink_is_safe("example.com"));
  g_assert_false(pt_term_core_hyperlink_is_safe("://example.com"));
  g_assert_false(pt_term_core_hyperlink_is_safe(""));
  g_assert_false(pt_term_core_hyperlink_is_safe(NULL));
  /* A prefix of an allowed scheme is not that scheme. */
  g_assert_false(pt_term_core_hyperlink_is_safe("httpsx://example.com"));
  g_assert_false(pt_term_core_hyperlink_is_safe("nothttp://example.com"));
  /* Whitespace and control bytes are out, wherever they sit: leading space
     would let " javascript:" through a scheme test that trims, and a newline
     in the middle can carry a second line into whatever opens the URI. */
  g_assert_false(pt_term_core_hyperlink_is_safe(" https://example.com"));
  g_assert_false(pt_term_core_hyperlink_is_safe("https://example.com\nrm -rf ~"));
  g_assert_false(pt_term_core_hyperlink_is_safe("https://exam ple.com"));
  g_assert_false(pt_term_core_hyperlink_is_safe("https://example.com\x7f"));
}

static void test_hyperlink_at(void) {
  /* Two OSC 8 links, one per row: an https one pt opens and a javascript: one
     it must not. Cells are 8x16 inset by 20/18, so column N of row R spans
     pixels [20 + 8N, 20 + 8N + 8) x [18 + 16R, 18 + 16R + 16). */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '\\033]8;;https://example.com/x\\033\\\\GOOD\\033]8;;\\033\\\\\\n'; "
    "printf '\\033]8;;javascript:alert(1)\\033\\\\EVIL\\033]8;;\\033\\\\\\n'; "
    "printf 'done-marker\\n'; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);

  /* Row 0, anywhere across "GOOD" (columns 0..3). */
  char *uri = pt_term_core_hyperlink_at(core, 21.0, 20.0);
  g_assert_cmpstr(uri, ==, "https://example.com/x");
  g_free(uri);
  uri = pt_term_core_hyperlink_at(core, 45.0, 30.0);      /* column 3 */
  g_assert_cmpstr(uri, ==, "https://example.com/x");
  g_free(uri);

  /* Column 4 of row 0 is past the link text. */
  g_assert_null(pt_term_core_hyperlink_at(core, 53.0, 20.0));
  /* Row 1 has a link, but not one pt will open. */
  g_assert_null(pt_term_core_hyperlink_at(core, 21.0, 36.0));
  /* The padding around the grid belongs to no cell, so the edge column's link
     cannot be reached from outside it. */
  g_assert_null(pt_term_core_hyperlink_at(core, 5.0, 20.0));
  g_assert_null(pt_term_core_hyperlink_at(core, 21.0, 5.0));
  g_assert_null(pt_term_core_hyperlink_at(core, 20.0 + 80 * 8 + 1, 20.0));
  g_assert_null(pt_term_core_hyperlink_at(core, 21.0, 18.0 + 24 * 16 + 1));

  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void on_command_cb(PtTermCore *core, const char *comm, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  g_strlcpy(ctx->comm, comm, sizeof(ctx->comm));
  /* The pty's foreground pgrp is the shell ("sh") until it execs/forks sleep;
     accept either non-empty comm to avoid a timing race. */
  if (g_strcmp0(comm, "sleep") == 0 || g_strcmp0(comm, "sh") == 0) {
    ctx->found = TRUE;
    g_main_loop_quit(ctx->loop);
  }
}

static void test_foreground_command(void) {
  /* tcgetpgrp path: the foreground program of the pty should be reported. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c", "sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .command = on_command_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(3, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  g_assert_cmpuint(strlen(ctx.comm), >, 0);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* ---- run state ---- */

/* Field 8 of /proc/<pid>/stat is tpgid: the foreground process group of the
   process's controlling terminal. Reading it from the child gives the tests an
   oracle for the pty's fg pgrp that is independent of pt_term_core_running, so
   the assertions below cannot pass by accident (e.g. an inverted comparison,
   or a not-yet-settled tty where tcgetpgrp still returns 0). */
static pid_t child_tpgid(pid_t pid) {
  char path[64];
  g_snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
  char *buf = NULL;
  if (!g_file_get_contents(path, &buf, NULL, NULL)) return -1;
  /* comm (field 2) may contain spaces and parens; scan after the LAST ')'. */
  char *close = strrchr(buf, ')');
  int ppid, pgrp, sess, tty, tpgid = -1;
  if (close != NULL)
    sscanf(close + 2, "%*c %d %d %d %d %d",
           &ppid, &pgrp, &sess, &tty, &tpgid);
  g_free(buf);
  return (pid_t)tpgid;
}

/* Pump the main loop until pred() holds or the deadline passes. */
static gboolean wait_until(gboolean (*pred)(PtTermCore *), PtTermCore *core) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    if (pred(core)) return TRUE;
    g_usleep(5000);
  }
  return pred(core);
}

static gboolean fg_is_child(PtTermCore *c) {
  return child_tpgid(pt_term_core_shell_pid(c)) == pt_term_core_shell_pid(c);
}

static gboolean fg_is_not_child(PtTermCore *c) {
  pid_t tp = child_tpgid(pt_term_core_shell_pid(c));
  return tp > 0 && tp != pt_term_core_shell_pid(c);
}

static void test_running_state_idle(void) {
  /* Negative case: the spawned program is itself the pty's foreground
     process-group leader (forkpty/login_tty), so nothing is "running" on top
     of it. Wait for the tty to actually settle on the child first, otherwise
     the assertion would only exercise the `fg > 0` guard. */
  const char *argv[] = {"/bin/cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_nonnull(core);
  g_assert_true(wait_until(fg_is_child, core));       /* tty settled on cat */
  g_assert_false(pt_term_core_running(core));
  /* No prompt snippet has reported an exit code yet. */
  g_assert_cmpint(pt_term_core_last_exit(core), ==, -1);
  pt_term_core_free(core);
}

static void test_running_state_foreground_job(void) {
  /* Positive case: -m turns on job control, so the shell puts `sleep` in its
     own process group and hands it the tty. fg pgrp != shell pid → running. */
  const char *argv[] = {"/bin/sh", "-mc", "sleep 30; true", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_nonnull(core);
  g_assert_true(wait_until(fg_is_not_child, core));   /* sleep owns the tty */
  g_assert_true(pt_term_core_running(core));
  pt_term_core_free(core);
}

/* ---- spawn env ---- */
static void on_draw_env(PtTermCore *core, gpointer user) {
  Ctx *ctx = user;
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  if (text != NULL && strstr(text, "marker=alphaproj") != NULL) {
    ctx->found = TRUE;
    g_main_loop_quit(ctx->loop);
  }
  g_free(text);
}

static void test_spawn_env(void) {
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "echo marker=$PT_PROJECT; sleep 30", NULL};
  const char *envp[] = {"PT_PROJECT=alphaproj", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, envp, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_nonnull(core);
  PtTermCoreCallbacks cbs = { .draw = on_draw_env };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* ---- exit-code title marker ---- */
static void on_title_cb(PtTermCore *core, const char *title, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  g_strlcpy(ctx->title, title, sizeof(ctx->title));
}

static void test_exit_marker_from_title(void) {
  /* The prompt snippet reports the previous command's status by prefixing the
     OSC-0 title with "pt-exit:<code>;". The core strips it and remembers. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '\\033]0;pt-exit:7;proj-title\\007'; printf 'done-marker\\n'; "
    "sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker, .title = on_title_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  g_assert_cmpint(pt_term_core_last_exit(core), ==, 7);
  g_assert_cmpstr(ctx.title, ==, "proj-title");   /* marker stripped */
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* ---- mouse reporting ----
 *
 * `stty -echo` stops the tty from echoing our bytes back through the parser
 * (where an escape sequence would be swallowed as a real one) and `-icanon`
 * stops the line discipline from holding them until a newline, which a mouse
 * report never sends. `cat -v` then renders what it reads printably: an ESC
 * comes back as the two characters "^[", so the encoded report shows up as
 * ordinary text in the grid. */
static gboolean wait_for_text(PtTermCore *core, const char *needle) {
  gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    pt_term_core_sync(core);
    char *text = pt_term_core_grid_text(core);
    gboolean hit = text != NULL && strstr(text, needle) != NULL;
    g_free(text);
    if (hit) return TRUE;
    g_usleep(5000);
  }
  return FALSE;
}

static gboolean tracking_on(PtTermCore *c) {
  return pt_term_core_mouse_tracking(c);
}

static gboolean alt_screen_on(PtTermCore *c) {
  return pt_term_core_alt_screen(c);
}

static void test_mouse_report_sgr(void) {
  /* Mode 1000 (normal tracking) + 1006 (SGR format), the pair Claude Code and
     most modern TUIs ask for. Cells are 8x16 inset by 20/18, so pixel (21, 20)
     is column 0, row 0 — reported 1-based as ";1;1". */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1000h\\033[?1006h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(tracking_on, core));

  /* Left press then release: SGR button 0, "M" for press and "m" for release. */
  g_assert_true(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_PRESS,
                                          GHOSTTY_MOUSE_BUTTON_LEFT, 0,
                                          21.0, 20.0));
  g_assert_true(wait_for_text(core, "^[[<0;1;1M"));
  g_assert_true(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_RELEASE,
                                          GHOSTTY_MOUSE_BUTTON_LEFT, 0,
                                          21.0, 20.0));
  g_assert_true(wait_for_text(core, "^[[<0;1;1m"));

  /* The wheel rides the same protocol: button four is 64, five is 65. */
  g_assert_true(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_PRESS,
                                          GHOSTTY_MOUSE_BUTTON_FOUR, 0,
                                          29.0, 36.0));
  g_assert_true(wait_for_text(core, "^[[<64;2;2M"));
  g_assert_true(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_PRESS,
                                          GHOSTTY_MOUSE_BUTTON_FIVE, 0,
                                          29.0, 36.0));
  g_assert_true(wait_for_text(core, "^[[<65;2;2M"));

  pt_term_core_free(core);
}

static void test_mouse_report_needs_tracking(void) {
  /* No mouse mode set: the encoder must produce nothing at all, so a click in
     a plain shell can never leak escape bytes into the command line. */
  const char *argv[] = {"/bin/cat", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  g_assert_nonnull(core);
  g_assert_false(pt_term_core_mouse_tracking(core));
  g_assert_false(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_PRESS,
                                           GHOSTTY_MOUSE_BUTTON_LEFT, 0,
                                           21.0, 20.0));
  pt_term_core_free(core);
}

/* ---- focus reporting (mode 1004) ----
 *
 * Same `cat -v` recipe as the mouse tests. A core starts out unfocused, so the
 * moment the child enables 1004 the core resends that state and an unsolicited
 * "^[[O" lands in the grid ahead of anything the test drives. Every assertion
 * below therefore matches on a pair of reports rather than a lone one, which
 * also proves what did *not* get written between them. */
static void test_focus_report(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1004hready'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  /* One write, so the parser has consumed 1004h by the time "ready" prints. */
  g_assert_true(wait_for_text(core, "ready"));

  g_assert_true(pt_term_core_focus_report(core, TRUE, FALSE));
  g_assert_true(wait_for_text(core, "^[[I"));
  g_assert_true(pt_term_core_focus_report(core, FALSE, FALSE));
  g_assert_true(wait_for_text(core, "^[[I^[[O"));

  pt_term_core_free(core);
}

static void test_focus_report_needs_mode(void) {
  /* Without mode 1004 a focus change must not put a single byte on the pty:
     a shell would run it as typed input. */
  const char *argv[] = {"/bin/cat", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  g_assert_nonnull(core);
  g_assert_false(pt_term_core_focus_report(core, TRUE, FALSE));
  g_assert_false(pt_term_core_focus_report(core, FALSE, FALSE));

  /* cat echoes (this one has the stock line discipline), so anything written
     would come back as "^[" in the grid. Give it time to. */
  for (int i = 0; i < 40; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "^["));
  g_free(text);
  pt_term_core_free(core);
}

/* How many times `needle` appears in the grid. Substring matching alone cannot
 * tell one report from two, since "^[[I^[[I^[[O" contains "^[[I^[[O". */
static int count_text(PtTermCore *core, const char *needle) {
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  int n = 0;
  for (const char *p = strstr(text, needle); p != NULL;
       p = strstr(p + strlen(needle), needle))
    n++;
  g_free(text);
  return n;
}

static void test_focus_report_dedupes(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1004hready'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  g_assert_true(pt_term_core_focus_report(core, TRUE, FALSE));
  g_assert_false(pt_term_core_focus_report(core, TRUE, FALSE));
  g_assert_true(pt_term_core_focus_report(core, FALSE, FALSE));
  /* The loss arriving proves the second gained had its turn and produced
     nothing: exactly one "^[[I" is on the wire. */
  g_assert_true(wait_for_text(core, "^[[I^[[O"));
  g_assert_cmpint(count_text(core, "^[[I"), ==, 1);

  pt_term_core_free(core);
}

static void test_focus_report_forced(void) {
  /* The path the resend on mode enable uses: same state, written anyway. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1004hready'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  g_assert_true(pt_term_core_focus_report(core, TRUE, FALSE));
  g_assert_true(pt_term_core_focus_report(core, TRUE, TRUE));
  g_assert_true(wait_for_text(core, "^[[I^[[I"));

  pt_term_core_free(core);
}

static void test_focus_report_resent_on_mode_enable(void) {
  /* The pane is focused, nothing has been reported because no app was asking,
     and then one starts and asks. It must be told without waiting for the user
     to click away and back. `read` holds the child until the test has set the
     focus state, so the enable can only happen afterwards. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; read x; printf '\\033[?1004h'; cat -v",
    NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  g_assert_false(pt_term_core_focus_report(core, TRUE, FALSE));  /* 1004 off */
  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, "^[[I"));

  pt_term_core_free(core);
}

/* ---- in-band resize reports (mode 2048) ----
 *
 * Same `cat -v` recipe again. The report is
 * "ESC [ 48 ; rows ; cols ; rows*cell_h ; cols*cell_w t", so a pane spawned at
 * 80x24 with 8x16 cells reports "^[[48;24;80;384;640t" and the same pane at
 * 100x30 reports "^[[48;30;100;480;800t". Every core here starts at 80x24, and
 * enabling the mode produces a report at once, so that first string doubles as
 * the signal that the parser has seen the enable. */
#define REPORT_80x24 "^[[48;24;80;384;640t"
#define REPORT_100x30 "^[[48;30;100;480;800t"

static void test_in_band_resize_report(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?2048h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  /* Waiting on the enable-time report is what proves the mode is on before the
     resize below; there is no getter, and racing it would pass vacuously. */
  g_assert_true(wait_for_text(core, REPORT_80x24));

  pt_term_core_resize(core, 100, 30, 8, 16);
  g_assert_true(wait_for_text(core, REPORT_100x30));

  pt_term_core_free(core);
}

static void test_in_band_resize_needs_mode(void) {
  /* Without mode 2048 a resize must put nothing on the pty: the child gets
     SIGWINCH and nothing else, as it always has. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  /* Different dimensions from the spawn size, or the change guard would carry
     this test on its own and it would prove nothing about the mode. */
  pt_term_core_resize(core, 100, 30, 8, 16);
  /* The pty is a FIFO: once PROBE has come back, anything written before it
     has too. */
  pt_term_core_write(core, "PROBE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE"));
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "^["));
  g_free(text);

  pt_term_core_free(core);
}

static void test_in_band_resize_unchanged_is_silent(void) {
  /* With the mode on, so this cannot pass by accident. libghostty-vt does not
     dedupe — ghostty_terminal_resize() re-sends the same bytes for the same
     size — so a second report here would mean pt's guard is gone. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?2048h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, REPORT_80x24));

  pt_term_core_resize(core, 80, 24, 8, 16);    /* what the core already has */
  pt_term_core_write(core, "PROBE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE"));
  g_assert_cmpint(count_text(core, "^[[48;"), ==, 1);

  pt_term_core_free(core);
}

static void test_in_band_resize_on_mode_enable(void) {
  /* An app that turns 2048 on at startup wants the size straight away, not on
     the next window drag. `read` holds the enable until the pane is settled at
     a known size, and no resize call follows it. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; read x; printf '\\033[?2048h'; cat -v",
    NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, REPORT_80x24));

  pt_term_core_free(core);
}

/* ---- color scheme (CSI ? 996 n, mode 2031) ----
 *
 * Same `cat -v` recipe once more, with one twist: the query has to come from
 * the *child*, because only what the child writes reaches the VT parser. Bytes
 * the test writes go the other way, into the child's input, where `cat -v`
 * prints them as text and no parser ever sees them. So the child sends the
 * query and prints pt's reply, which lands in the grid as "^[[?997;1n" (dark)
 * or "^[[?997;2n" (light). `read` holds it until the test has set the scheme,
 * since a core starts out dark and would otherwise answer before the flip. */
#define REPLY_DARK "^[[?997;1n"
#define REPLY_LIGHT "^[[?997;2n"

static void test_color_scheme_query_dark(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; read x; printf '\\033[?996n'; cat -v",
    NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  /* Light first so the dark set below is a real change, not the default. */
  pt_term_core_set_color_scheme(core, FALSE);
  pt_term_core_set_color_scheme(core, TRUE);
  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, REPLY_DARK));
  /* Mode 2031 is off throughout: a direct question is answered anyway. */
  g_assert_cmpint(count_text(core, REPLY_LIGHT), ==, 0);

  pt_term_core_free(core);
}

static void test_color_scheme_query_light(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; read x; printf '\\033[?996n'; cat -v",
    NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_set_color_scheme(core, FALSE);
  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, REPLY_LIGHT));

  pt_term_core_free(core);
}

static void test_color_scheme_notifies_on_change(void) {
  /* With mode 2031 on, a theme flip is announced unasked — and only when it is
     really a flip: setting the same value again must put nothing on the pty. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?2031hready'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  /* One write, so the parser has consumed 2031h by the time "ready" prints. */
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_set_color_scheme(core, FALSE);      /* dark -> light: reported */
  g_assert_true(wait_for_text(core, REPLY_LIGHT));
  pt_term_core_set_color_scheme(core, FALSE);      /* no change: silent */
  pt_term_core_set_color_scheme(core, TRUE);       /* light -> dark: reported */
  /* The dark reply arriving proves the repeat had its turn and wrote nothing:
     the pty is FIFO, so a second light reply would already be in the grid. */
  g_assert_true(wait_for_text(core, REPLY_DARK));
  g_assert_cmpint(count_text(core, REPLY_LIGHT), ==, 1);

  pt_term_core_free(core);
}

static void test_color_scheme_needs_mode(void) {
  /* Mode 2031 off: a scheme change writes nothing at all. Unsolicited bytes
     would be typed input to whatever is running in the pane. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_set_color_scheme(core, FALSE);
  pt_term_core_set_color_scheme(core, TRUE);

  /* The probe is the proof the notifications had their chance. */
  pt_term_core_write(core, "PROBE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE"));
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "997"));
  g_assert_null(strstr(text, "^["));
  g_free(text);

  pt_term_core_free(core);
}

static void test_alt_screen_arrows(void) {
  /* On the alt screen with no mouse tracking, the wheel becomes cursor keys.
     Mode 1007 is on by default, and DECCKM is off here, so the normal form
     ESC [ B is what a pager should receive. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1049h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(alt_screen_on, core));
  g_assert_true(pt_term_core_alt_scroll(core));      /* default-on */
  g_assert_false(pt_term_core_mouse_tracking(core));

  pt_term_core_send_arrows(core, FALSE, 2);
  g_assert_true(wait_for_text(core, "^[[B^[[B"));
  pt_term_core_send_arrows(core, TRUE, 1);
  g_assert_true(wait_for_text(core, "^[[A"));

  pt_term_core_free(core);
}

static void test_alt_screen_tracking_wheel(void) {
  /* Claude Code's startup set, captured from a real session: alt screen, all
     three tracking modes, SGR encoding. This is the shape that made the wheel
     dead — there is no alternate-scroll fallback here (the app *is* tracking)
     and the alt screen has no scrollback to move, so a wheel notch the widget
     keeps to itself goes nowhere. It has to be reported. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; "
    "printf '\\033[?1049h\\033[?1000h\\033[?1002h\\033[?1003h\\033[?1006h'; "
    "cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(alt_screen_on, core));
  g_assert_true(wait_until(tracking_on, core));

  pt_term_core_sync(core);
  char *before = pt_term_core_grid_text(core);
  pt_term_core_scroll_delta(core, -3);   /* nothing above: the view cannot move */
  pt_term_core_sync(core);
  char *after = pt_term_core_grid_text(core);
  g_assert_cmpstr(before, ==, after);
  g_free(before);
  g_free(after);

  g_assert_true(pt_term_core_mouse_report(core, GHOSTTY_MOUSE_ACTION_PRESS,
                                          GHOSTTY_MOUSE_BUTTON_FOUR, 0,
                                          29.0, 36.0));
  g_assert_true(wait_for_text(core, "^[[<64;2;2M"));

  pt_term_core_free(core);
}

static void test_scroll_bottom(void) {
  /* Typing while scrolled up must snap back to the prompt. Fill the scrollback
     past one screen, scroll up until the last line is off-view, then assert
     scroll_bottom brings it back. */
  const char *argv[] = {"/bin/sh", "-c",
    "i=1; while [ $i -le 60 ]; do echo line$i; i=$((i+1)); done; "
    "echo bottom-marker; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "bottom-marker"));

  pt_term_core_scroll_delta(core, -40);
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "bottom-marker"));   /* scrolled out of view */
  g_free(text);

  pt_term_core_scroll_bottom(core);
  pt_term_core_sync(core);
  text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_nonnull(strstr(text, "bottom-marker"));
  g_free(text);

  pt_term_core_free(core);
}

/* ---- full reset ----
 *
 * ghostty's `reset` action: everything the terminal knows goes back to
 * defaults, the child is left alone. */

static void test_reset_clears_mouse_tracking(void) {
  /* The wedge the command exists for: an app turned tracking on and died
     without turning it off, so the mouse no longer selects. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1000h\\033[?1006h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(tracking_on, core));

  pt_term_core_reset(core);
  g_assert_false(pt_term_core_mouse_tracking(core));

  pt_term_core_free(core);
}

static void test_reset_clears_grid_and_scrollback(void) {
  /* Fill past one screen, scroll up, reset: the grid is empty, the scrollback
     is gone, and the viewport is back on the active area — which is proved by
     new output showing up without a scroll_bottom of its own. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; "
    "i=1; while [ $i -le 60 ]; do echo line$i; i=$((i+1)); done; "
    "echo bottom-marker; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "bottom-marker"));

  pt_term_core_scroll_delta(core, -40);
  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "bottom-marker"));   /* scrolled out of view */
  g_free(text);

  pt_term_core_reset(core);
  pt_term_core_sync(core);
  text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "bottom-marker"));
  g_assert_null(strstr(text, "line1"));           /* scrollback discarded */
  g_assert_cmpstr(g_strstrip(text), ==, "");      /* nothing left at all */
  g_free(text);

  /* Still scrolled up and the reset would have to have left the viewport in
     the discarded scrollback for this to fail. */
  pt_term_core_write(core, "after-reset\n", -1);
  g_assert_true(wait_for_text(core, "after-reset"));

  pt_term_core_free(core);
}

static void test_reset_keeps_the_child(void) {
  /* Nothing is signalled and nothing is respawned: the same shell is still
     there afterwards and still reading what is typed at it. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));
  pid_t before = pt_term_core_shell_pid(core);
  g_assert_cmpint(before, >, 0);

  pt_term_core_reset(core);
  g_assert_cmpint(pt_term_core_shell_pid(core), ==, before);
  g_assert_false(pt_term_core_exited(core, NULL));

  pt_term_core_send_key(core, GHOSTTY_KEY_H, GHOSTTY_KEY_ACTION_PRESS, 0,
                        'h', "h", 1);
  pt_term_core_send_key(core, GHOSTTY_KEY_I, GHOSTTY_KEY_ACTION_PRESS, 0,
                        'i', "i", 1);
  g_assert_true(wait_for_text(core, "hi"));       /* cat echoed it back */
  g_assert_cmpint(pt_term_core_shell_pid(core), ==, before);

  pt_term_core_free(core);
}

static void test_reset_clears_selection(void) {
  /* The library drops its own selection in Screen.reset(), so pt's mirror of
     it has to go too or selection_text would name a selection that is gone. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf 'SELECTME done-marker\\n'; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "SELECTME"));

  pt_term_core_selection_press(core, 21.0, 20.0, 1000000000ULL);
  pt_term_core_selection_drag(core, 84.0, 20.0);
  pt_term_core_selection_release(core, 84.0, 20.0);
  char *sel = pt_term_core_selection_text(core);
  g_assert_nonnull(sel);
  g_free(sel);

  pt_term_core_reset(core);
  g_assert_null(pt_term_core_selection_text(core));

  /* A reset mid-drag also ends the drag. Fresh output goes where the old
     anchor pointed, so a stray motion afterwards would select it if the drag
     had survived. */
  pt_term_core_selection_press(core, 21.0, 20.0, 3000000000ULL);
  pt_term_core_selection_drag(core, 84.0, 20.0);
  pt_term_core_reset(core);
  pt_term_core_write(core, "AFTER-RESET\n", -1);
  g_assert_true(wait_for_text(core, "AFTER-RESET"));
  pt_term_core_selection_drag(core, 100.0, 20.0);
  g_assert_null(pt_term_core_selection_text(core));

  pt_term_core_free(core);
}

static void test_reset_rearms_mode_edges(void) {
  /* fullReset turns 1004 back off, so the shadow pt polls the mode edge against
     has to go off with it. Left set, the re-enable below looks like no edge at
     all and the app that just asked for focus reports is told nothing until the
     user happens to click away and back. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1004hready'; read x; "
    "printf '\\033[?1004h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));
  /* Returning TRUE only happens with 1004 on, so this is the proof the first
     enable was parsed — without it the test would pass on a stale shadow that
     was never set. `read` is swallowing everything written meanwhile. */
  g_assert_true(pt_term_core_focus_report(core, TRUE, FALSE));

  pt_term_core_reset(core);
  /* The re-enable, on a grid the reset just wiped: the report below can only be
     the new one. */
  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, "^[[I"));
  g_assert_cmpint(count_text(core, "^[[I"), ==, 1);

  pt_term_core_free(core);
}

static void test_reset_rearms_in_band_resize(void) {
  /* The same re-arming for mode 2048, and it needs its own test: the 1004 one
     above cannot cover it, since either shadow left set is a separate silence.
     The child proves the first enable really fired by only continuing when the
     enable-time report reached it — `read` is holding that report, so a stale
     shadow that was never set cannot be what the assertions below pass on.

     Both the marker and the re-enable are one printf, so pt reads them in one
     go and polls the modes once. Split across two reads, the poll in between
     would see 2048 off after the reset and clear the shadow by itself, which
     is exactly the self-correction this test must not be rescued by. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?2048hready'; read x; "
    "[ -n \"$x\" ] && printf 'FIRST-REPORT-SEEN\\033[?2048h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_reset(core);
  pt_term_core_write(core, "\n", 1);
  g_assert_true(wait_for_text(core, "FIRST-REPORT-SEEN"));
  /* On a grid the reset wiped, so this is the report the re-enable earned. */
  g_assert_true(wait_for_text(core, REPORT_80x24));
  g_assert_cmpint(count_text(core, REPORT_80x24), ==, 1);

  pt_term_core_free(core);
}

static void test_reset_keeps_color_scheme(void) {
  /* The colors pt paints survive a reset — fullReset does not touch them — so
     the scheme it answers CSI ? 996 n with must survive too. Cleared back to
     the dark default, the next query would lie about a light theme. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; read x; printf '\\033[?996n'; cat -v",
    NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready"));

  pt_term_core_set_color_scheme(core, FALSE);
  pt_term_core_reset(core);
  pt_term_core_write(core, "\n", 1);            /* now the child queries */
  g_assert_true(wait_for_text(core, REPLY_LIGHT));
  g_assert_cmpint(count_text(core, REPLY_DARK), ==, 0);

  pt_term_core_free(core);
}

static void on_osc_count_cb(PtTermCore *core, int code, const char *payload,
                            gsize len, gpointer user) {
  (void)core; (void)len;
  Ctx *ctx = user;
  if (code != 9) return;
  ctx->osc_count++;
  ctx->osc_code = code;
  g_strlcpy(ctx->osc_payload, payload, sizeof(ctx->osc_payload));
}

/* An integration smoke test, deliberately: it proves the reset runs against a
   live core with the scanner wired to a real pty and leaves it dispatching, and
   it asserts nothing that depends on how the kernel batched the reads. The real
   guard for pt_osc_scan_clear() is /oscscan/clear-drops-partial in
   test-osc-scan, which drives PtOscScan directly and is deterministic. Proving
   the clear here would mean inferring, from "ARMED" reaching the grid, that the
   unterminated OSC written behind it in the same write() had already been fed
   to the scanner before the reset — and nothing guarantees the pty delivers
   both halves in one read. */
static void test_reset_clears_osc_scanner(void) {
  /* Plain `cat` (not `cat -v`) so the escape bytes come back raw and the
     scanner really sees them. */
  Ctx ctx = {0};
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .osc = on_osc_count_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  g_assert_true(wait_for_text(core, "ready"));

  /* A program that died mid-OSC. */
  pt_term_core_write(core, "ARMED\033]9;stale-payload", -1);
  g_assert_true(wait_for_text(core, "ARMED"));
  g_assert_cmpint(ctx.osc_count, ==, 0);          /* never terminated */

  pt_term_core_reset(core);

  /* A whole unrelated OSC after the reset still dispatches on its own terms,
     with its own payload and nothing of the dead one's. */
  pt_term_core_write(core, "\033]9;fresh-payload\007PROBE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE"));
  g_assert_cmpint(ctx.osc_count, ==, 1);
  g_assert_cmpstr(ctx.osc_payload, ==, "fresh-payload");

  pt_term_core_free(core);
}

static void on_osc_cb(PtTermCore *core, int code, const char *payload,
                      gsize len, gpointer user) {
  (void)core; (void)len;
  Ctx *ctx = user;
  if (code != 9) return;             /* consumers filter; so does this test */
  ctx->osc_code = code;
  g_strlcpy(ctx->osc_payload, payload, sizeof(ctx->osc_payload));
}

static void test_osc_reaches_callback(void) {
  /* The scanner itself is covered in test-osc-scan; this only proves it is
     wired into the read loop, on the real pty, past the parser. */
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '\\033]9;a notification\\007'; printf 'done-marker\\n'; "
    "sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker, .osc = on_osc_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  g_assert_cmpint(ctx.osc_code, ==, 9);
  g_assert_cmpstr(ctx.osc_payload, ==, "a notification");
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

static void on_osc_unregister(PtTermCore *core, int code, const char *payload,
                              gsize len, gpointer user) {
  (void)code; (void)payload; (void)len;
  Ctx *ctx = user;
  ctx->osc_count++;
  /* Drop the osc callback from inside the handler. Both sequences arrive in
     one write and so, normally, one read — the second dispatch of that read
     must notice and not call through the NULL. */
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker };
  pt_term_core_set_callbacks(core, &cbs, ctx);
}

static void test_osc_consumer_can_unregister(void) {
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '\\033]9;one\\007\\033]9;two\\007done-marker\\n'; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker, .osc = on_osc_unregister };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  guint to = g_timeout_add_seconds(10, on_timeout, &ctx);
  g_main_loop_run(ctx.loop);
  g_source_remove(to);
  g_assert_true(ctx.found);
  g_assert_cmpint(ctx.osc_count, ==, 1);   /* and no crash on the second */
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* ---- OSC 52 clipboard writes ----
 *
 * The payload decoder first, on its own, then the whole path on a real pty. */
static void test_osc52_decode(void) {
  gboolean primary = TRUE;
  gsize len = 0;
  char *out = pt_osc52_decode("c;aGVsbG8=", 10, &primary, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_cmpuint(len, ==, 5);
  g_assert_false(primary);
  g_free(out);

  /* The target says which selection. */
  out = pt_osc52_decode("p;aGVsbG8=", 10, &primary, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_true(primary);
  g_free(out);
  /* Several at once: pt has one of each, and the clipboard wins. */
  out = pt_osc52_decode("pc;aGVsbG8=", 11, &primary, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_false(primary);
  g_free(out);
  /* Anything else, including naming nothing, means the clipboard. */
  out = pt_osc52_decode("s0;aGVsbG8=", 11, &primary, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_false(primary);
  g_free(out);
  primary = TRUE;
  out = pt_osc52_decode(";aGVsbG8=", 9, &primary, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_false(primary);
  g_free(out);

  /* Padding left off is still unambiguous, so it decodes whole rather than
     losing the tail. */
  out = pt_osc52_decode("c;aGVsbG8", 9, NULL, &len);
  g_assert_cmpstr(out, ==, "hello");
  g_assert_cmpuint(len, ==, 5);
  g_free(out);

  /* The read form: nothing comes back, at any setting. */
  g_assert_null(pt_osc52_decode("c;?", 3, NULL, NULL));
  g_assert_null(pt_osc52_decode("p;?", 3, NULL, NULL));
  g_assert_null(pt_osc52_decode(";?", 2, NULL, NULL));

  /* Not base64. g_base64_decode() would answer garbage for every one of
     these rather than fail, which is exactly why they are checked first. */
  g_assert_null(pt_osc52_decode("c;!!!!", 6, NULL, NULL));
  g_assert_null(pt_osc52_decode("c;aGVsbG8!", 10, NULL, NULL));
  g_assert_null(pt_osc52_decode("c;aGVs bG8=", 11, NULL, NULL));  /* space */
  g_assert_null(pt_osc52_decode("c;aGV=sbG8=", 11, NULL, NULL));  /* pad inside */
  g_assert_null(pt_osc52_decode("c;aGVsbG8===", 12, NULL, NULL)); /* 3 pad */
  g_assert_null(pt_osc52_decode("c;a", 3, NULL, NULL));      /* lone character */
  g_assert_null(pt_osc52_decode("c;aGVsbG8=a", 11, NULL, NULL));
  g_assert_null(pt_osc52_decode("c;", 2, NULL, NULL));       /* nothing to copy */
  g_assert_null(pt_osc52_decode("c", 1, NULL, NULL));        /* no ';' at all */

  /* An embedded NUL would put only the head of the text on the clipboard
     while every length here still counted the rest. "YQBi" is "a\0b". */
  g_assert_null(pt_osc52_decode("c;YQBi", 6, NULL, NULL));

  /* Valid base64 is not the same as text. A clipboard is offered to the rest
     of the desktop as UTF-8, so bytes that are not UTF-8 would be advertised
     as text and come back mangled. "/w==" is a lone 0xFF, and "ww==" is a
     lead byte with the rest of its character missing, which is what a yank
     cut off mid-character looks like. */
  g_assert_null(pt_osc52_decode("c;/w==", 6, NULL, NULL));
  g_assert_null(pt_osc52_decode("c;ww==", 6, NULL, NULL));

  /* Text that is not ASCII is still text, and has to survive whole. */
  const char *utf8 = "héllo — ✓ 日本語";
  char *b64 = g_base64_encode((const guchar *)utf8, strlen(utf8));
  char *payload = g_strconcat("c;", b64, NULL);
  out = pt_osc52_decode(payload, strlen(payload), NULL, &len);
  g_assert_cmpstr(out, ==, utf8);
  g_assert_cmpuint(len, ==, strlen(utf8));
  g_free(out);
  g_free(payload);
  g_free(b64);
}

static void test_osc52_decode_cap(void) {
  /* Judged on the encoded length, before anything is allocated: four
     characters carry three bytes, so this is a hair over the cap. */
  gsize n = (PT_OSC_52_TEXT_MAX / 3 + 4) * 4;
  GString *s = g_string_new("c;");
  for (gsize i = 0; i < n; i++) g_string_append_c(s, 'A');
  g_assert_null(pt_osc52_decode(s->str, s->len, NULL, NULL));
  g_string_free(s, TRUE);

  /* A large but allowed clipboard still comes through whole. */
  gsize body = 600u * 1024u;
  char *big = g_malloc(body);
  memset(big, 'x', body);
  char *b64 = g_base64_encode((const guchar *)big, body);
  char *payload = g_strconcat("c;", b64, NULL);
  gsize out_len = 0;
  char *out = pt_osc52_decode(payload, strlen(payload), NULL, &out_len);
  g_assert_nonnull(out);
  g_assert_cmpuint(out_len, ==, body);
  g_assert_cmpint(memcmp(out, big, body), ==, 0);
  g_free(out);
  g_free(payload);
  g_free(b64);
  g_free(big);
}

static void on_clipboard_write(PtTermCore *core, const char *text, gsize len,
                               gboolean primary, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  ctx->clip_count++;
  ctx->clip_len = len;
  ctx->clip_primary = primary;
  g_strlcpy(ctx->clip, text, sizeof(ctx->clip));
}

/* Run `cmd` in a pane with only the clipboard consumer registered — which is
 * also what proves the scanner runs off that callback alone — and return once
 * the marker the command prints after the sequence is on the grid. The pty is
 * in order, so by then the sequence has been through the scanner. */
static void run_osc52(Ctx *ctx, const char *cmd, PtOsc52Mode mode) {
  ctx->loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = on_draw_marker,
                              .clipboard_write = on_clipboard_write };
  pt_term_core_set_callbacks(core, &cbs, ctx);
  pt_term_core_set_osc52(core, mode);
  guint to = g_timeout_add_seconds(10, on_timeout, ctx);
  g_main_loop_run(ctx->loop);
  g_source_remove(to);
  g_assert_true(ctx->found);      /* the marker arrived; so did the sequence */
  pt_term_core_free(core);
  g_main_loop_unref(ctx->loop);
}

static void test_osc52_clipboard(void) {
  Ctx ctx = {0};
  run_osc52(&ctx,
            "printf '\\033]52;c;aGVsbG8=\\007'; printf 'done-marker\\n'; "
            "sleep 30", PT_OSC52_WRITE);
  g_assert_cmpint(ctx.clip_count, ==, 1);
  g_assert_cmpstr(ctx.clip, ==, "hello");
  g_assert_cmpuint(ctx.clip_len, ==, 5);
  g_assert_false(ctx.clip_primary);
}

static void test_osc52_primary(void) {
  Ctx ctx = {0};
  run_osc52(&ctx,
            "printf '\\033]52;p;aGVsbG8=\\007'; printf 'done-marker\\n'; "
            "sleep 30", PT_OSC52_WRITE);
  g_assert_cmpint(ctx.clip_count, ==, 1);
  g_assert_cmpstr(ctx.clip, ==, "hello");
  g_assert_true(ctx.clip_primary);
}

static void test_osc52_invalid_base64(void) {
  Ctx ctx = {0};
  run_osc52(&ctx,
            "printf '\\033]52;c;not base64!\\007'; printf 'done-marker\\n'; "
            "sleep 30", PT_OSC52_WRITE);
  g_assert_cmpint(ctx.clip_count, ==, 0);
}

static void test_osc52_over_cap(void) {
  /* Past PT_OSC_52_MAX the scanner drops the sequence outright, so the decoder
     never sees it and nothing is allocated for it. The filler is 'B' rather
     than 'A' on purpose: "AAAA" decodes to NUL bytes, which the decoder throws
     out for its own reasons, and this has to fail on the cap. */
  Ctx ctx = {0};
  run_osc52(&ctx,
            "printf '\\033]52;c;'; head -c 1100000 /dev/zero | tr '\\0' 'B'; "
            "printf '\\007'; printf 'done-marker\\n'; sleep 30",
            PT_OSC52_WRITE);
  g_assert_cmpint(ctx.clip_count, ==, 0);

  /* The same clipboard a size down arrives whole — so the drop above was the
     cap doing its job, not a megabyte of output getting lost on the way. */
  Ctx under = {0};
  run_osc52(&under,
            "printf '\\033]52;c;'; head -c 900000 /dev/zero | tr '\\0' 'B'; "
            "printf '\\007'; printf 'done-marker\\n'; sleep 30",
            PT_OSC52_WRITE);
  g_assert_cmpint(under.clip_count, ==, 1);
  g_assert_cmpuint(under.clip_len, ==, 900000 / 4 * 3);
}

static void test_osc52_off_in_config(void) {
  /* The `osc52` config key, end to end: parsed from a file's text, handed to
     the core, and a perfectly good sequence does nothing. */
  PtConfig *cfg = pt_config_parse("osc52 = off\n");
  g_assert_cmpint(cfg->osc52, ==, PT_OSC52_OFF);
  Ctx ctx = {0};
  run_osc52(&ctx,
            "printf '\\033]52;c;aGVsbG8=\\007'; printf 'done-marker\\n'; "
            "sleep 30", cfg->osc52);
  g_assert_cmpint(ctx.clip_count, ==, 0);
  pt_config_free(cfg);
}

static void test_osc52_query_is_never_answered(void) {
  /* The read form lets whatever is running in the pane — or on the far end of
     an ssh session — read the clipboard, so pt answers nothing at all: no
     callback, and not one byte to the pty. `cat -v` prints everything pt
     writes, so a reply would land in the grid. The probe afterwards is the
     proof that a reply had its chance: the pty is FIFO, so anything written
     before it would already be echoed by the time the probe shows up. */
  Ctx ctx = {0};
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033]52;c;?\\007'; printf 'sent-marker\\n'; "
    "cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .clipboard_write = on_clipboard_write };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  g_assert_true(wait_for_text(core, "sent-marker"));   /* query scanned */

  pt_term_core_write(core, "PROBE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE"));

  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "]52"));        /* no reply came back */
  g_assert_null(strstr(text, "^["));         /* nor anything else escaped */
  g_free(text);
  g_assert_cmpint(ctx.clip_count, ==, 0);

  pt_term_core_free(core);
}

/* ---- paste ----
 *
 * Same `stty -echo -icanon; cat -v` trick as the mouse tests: whatever we write
 * to the pty comes back printably, so the bytes the paste path produced are
 * visible in the grid (ESC as "^[", CR as "^M"). */
static gboolean bracketed_on(PtTermCore *c) {
  return pt_term_core_bracketed_paste(c);
}

static void test_paste_bracketed_strips_end_sequence(void) {
  /* The attack the sanitizer exists for: clipboard text carrying its own
     ESC [ 201 ~ would close bracketed paste early and hand the rest to the
     shell as typed input. The ESC must come out as a space, leaving exactly
     one opening and one closing marker with no escape between them. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?2004h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(bracketed_on, core));

  const char payload[] = "AAA\x1b[201~BBB";
  pt_term_core_paste(core, payload, sizeof(payload) - 1);
  g_assert_true(wait_for_text(core, "^[[201~"));

  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);

  char *open = strstr(text, "^[[200~");
  g_assert_nonnull(open);
  char *body = open + strlen("^[[200~");
  g_assert_null(strstr(body, "^[[200~"));        /* exactly one opener */
  char *close = strstr(body, "^[[201~");
  g_assert_nonnull(close);
  g_assert_null(strstr(close + strlen("^[[201~"), "^[[201~"));  /* one closer */

  char *inner = g_strndup(body, (gsize)(close - body));
  g_assert_null(strstr(inner, "^["));            /* no escape inside the paste */
  g_assert_cmpstr(inner, ==, "AAA [201~BBB");    /* ESC replaced by a space */
  g_free(inner);
  g_free(text);

  pt_term_core_free(core);
}

static void test_paste_unbracketed_newline_becomes_cr(void) {
  /* Without mode 2004 there are no markers, and newlines become carriage
     returns — one line submitted, not two. */
  /* -icrnl as well: the line discipline turns an input CR into NL by default,
     which would hide the very byte under test. No mode change to wait on
     here either, so the child announces when the tty is configured — pasting
     before `stty` lands would be echoed back and the CR would just move the
     cursor. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon -icrnl; printf 'ready-marker\\n'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready-marker"));
  g_assert_false(pt_term_core_bracketed_paste(core));

  pt_term_core_paste(core, "a\nb", 3);
  g_assert_true(wait_for_text(core, "a^Mb"));

  /* A CR already in the clipboard is neither stripped nor rewritten — 0x0D is
     not in ghostty's strip set — so it reaches the child exactly as it was.
     That is why pt_term_core_paste_is_safe has to catch it: on a normal tty
     (icrnl on, unlike here) it arrives as a newline and submits the line. */
  pt_term_core_paste(core, "c\rd", 3);
  g_assert_true(wait_for_text(core, "c^Md"));

  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "^[[200~"));
  g_free(text);

  pt_term_core_free(core);
}

static void test_paste_is_safe(void) {
  g_assert_true(pt_term_core_paste_is_safe("ls", 2));
  g_assert_false(pt_term_core_paste_is_safe("ls\nrm -rf /", 11));
  /* An embedded end sequence is unsafe too, newline or not. */
  g_assert_false(pt_term_core_paste_is_safe("ls\x1b[201~rm -rf /", 16));
  g_assert_true(pt_term_core_paste_is_safe("ls", -1));   /* NUL-terminated */
  /* A bare CR submits the line just as an LF does — the encoder passes it
     through and the tty's icrnl maps it back — so it has to be unsafe. */
  g_assert_false(pt_term_core_paste_is_safe("a\rb", 3));
  g_assert_false(pt_term_core_paste_is_safe("a\r\nb", 4));
  g_assert_false(pt_term_core_paste_is_safe("echo hi\r", 8));   /* trailing */
}

/* ---- scrollbar ---- */

static void test_scrollbar_tracks_the_viewport(void) {
  /* 500 lines into a 24-row pane: the scrollable area is taller than the
     viewport, and the view starts at the bottom of it. */
  const char *argv[] = {"/bin/sh", "-c",
    "seq 1 500; echo bottom-marker; sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "bottom-marker"));

  guint64 total = 0, offset = 0, len = 0;
  g_assert_true(pt_term_core_scrollbar(core, &total, &offset, &len));
  g_assert_cmpuint(len, ==, 24);              /* the visible area is the pane */
  g_assert_cmpuint(total, >, 24);             /* and there is history above it */
  g_assert_cmpuint(offset + len, ==, total);  /* sitting at the bottom */

  /* Ten rows up moves the offset by exactly ten and nothing else: the
     scrollable area does not change shape because the view moved. */
  guint64 total_up = 0, offset_up = 0, len_up = 0;
  pt_term_core_scroll_delta(core, -10);
  g_assert_true(pt_term_core_scrollbar(core, &total_up, &offset_up, &len_up));
  g_assert_cmpuint(offset_up, ==, offset - 10);
  g_assert_cmpuint(total_up, ==, total);
  g_assert_cmpuint(len_up, ==, len);

  guint64 offset_back = 0, len_back = 0, total_back = 0;
  pt_term_core_scroll_bottom(core);
  g_assert_true(pt_term_core_scrollbar(core, &total_back, &offset_back,
                                       &len_back));
  g_assert_cmpuint(offset_back, ==, offset);
  g_assert_cmpuint(offset_back + len_back, ==, total_back);

  pt_term_core_free(core);
}

static void test_scrollbar_without_scrollback(void) {
  /* Nothing has been printed, so there is nothing above the viewport: the
     whole scrollable area is the viewport. This is the case where the bar
     must be hidden, and it is the state every fresh pane starts in. */
  const char *argv[] = {"/bin/sh", "-c", "sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);

  guint64 total = 0, offset = 0, len = 0;
  g_assert_true(pt_term_core_scrollbar(core, &total, &offset, &len));
  g_assert_cmpuint(total, ==, len);
  g_assert_cmpuint(total, ==, 24);
  g_assert_cmpuint(offset, ==, 0);

  pt_term_core_free(core);
}

static void test_scrollbar_hidden_on_the_alt_screen(void) {
  /* The alt screen keeps no scrollback (Terminal.zig:2994 gives it
     max_scrollback 0), so the numbers themselves say there is no bar to draw
     there, whichever screen the primary one is holding. */
  const char *argv[] = {"/bin/sh", "-c",
    "seq 1 500; stty -echo -icanon; printf '\\033[?1049h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(alt_screen_on, core));

  guint64 total = 0, offset = 0, len = 0;
  g_assert_true(pt_term_core_scrollbar(core, &total, &offset, &len));
  g_assert_cmpuint(total, ==, len);
  g_assert_cmpuint(offset, ==, 0);

  pt_term_core_free(core);
}

static void test_scrollbar_read_is_cached(void) {
  /* The library warns this query is expensive, so it is read at most once
     after each thing that can move the numbers, however often it is asked.
     Nothing is pumped between the calls below, so the only thing that can
     dirty the cache is the call under test. */
  const char *argv[] = {"/bin/sh", "-c", "echo cached-marker; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_cmpuint(pt_term_core_scrollbar_reads(core), ==, 0);  /* never eager */
  g_assert_true(wait_for_text(core, "cached-marker"));

  /* Output arrived, so the first ask goes to the library. */
  g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  guint64 reads = pt_term_core_scrollbar_reads(core);
  g_assert_cmpuint(reads, ==, 1);

  /* Asking again with nothing moved is free, however many frames ask. */
  for (int i = 0; i < 10; i++)
    g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  g_assert_cmpuint(pt_term_core_scrollbar_reads(core), ==, reads);

  /* The viewport moving costs exactly one more read, not one per ask. */
  pt_term_core_scroll_delta(core, -5);
  g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  g_assert_cmpuint(pt_term_core_scrollbar_reads(core), ==, reads + 1);

  /* And so does the terminal being written to. `cat` is on the far end, so
     the text coming back is proof the read path ran. */
  pt_term_core_write(core, "probe-echo\n", -1);
  g_assert_true(wait_for_text(core, "probe-echo"));
  g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  g_assert_true(pt_term_core_scrollbar(core, NULL, NULL, NULL));
  g_assert_cmpuint(pt_term_core_scrollbar_reads(core), ==, reads + 2);
  pt_term_core_free(core);
}

/* ---- cursor shape and blink (DECSCUSR, mode 12) ----
 *
 * An escape sequence only means anything on its way *out* of the child, so
 * these drive a bare `cat` with the line discipline out of the way: what the
 * test writes to the pty, cat reads and writes straight back, and it arrives
 * at pt's parser exactly as if the program had printed it. `-echo` stops the
 * tty echoing it a second time and `-icanon` stops it being held until a
 * newline, which an escape sequence never sends.
 *
 * The getters answer as of the last sync, so every predicate syncs first. */
static GhosttyRenderStateCursorVisualStyle want_style;
static gboolean want_blinking;

static gboolean style_is_wanted(PtTermCore *c) {
  pt_term_core_sync(c);
  return pt_term_core_cursor_style(c) == want_style;
}

static gboolean blinking_is_wanted(PtTermCore *c) {
  pt_term_core_sync(c);
  return pt_term_core_cursor_blinking(c) == want_blinking;
}

static gboolean wide_tail_on(PtTermCore *c) {
  pt_term_core_sync(c);
  return pt_term_core_cursor_wide_tail(c);
}

static gboolean wide_tail_off(PtTermCore *c) {
  pt_term_core_sync(c);
  return !pt_term_core_cursor_wide_tail(c);
}

static PtTermCore *cursor_core_new(void) {
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf 'ready-marker\\n'; exec cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  /* One write, so the tty is settled by the time the marker prints. */
  g_assert_true(wait_for_text(core, "ready-marker"));
  return core;
}

/* Send a sequence through the child and wait for the style to become `style`.
   Each assertion below is a real transition away from what the last one left,
   so none of them can pass on a getter that never changes. */
static void expect_style(PtTermCore *core, const char *seq,
                         GhosttyRenderStateCursorVisualStyle style) {
  want_style = style;
  pt_term_core_write(core, seq, -1);
  g_assert_true(wait_until(style_is_wanted, core));
}

static void expect_blinking(PtTermCore *core, const char *seq,
                            gboolean blinking) {
  want_blinking = blinking;
  pt_term_core_write(core, seq, -1);
  g_assert_true(wait_until(blinking_is_wanted, core));
}

static void test_cursor_style_defaults(void) {
  /* What a terminal nobody has configured looks like, which is also what
     `CSI 0 SP q` has to come back to below. libghostty-vt has no user config
     to restore, so its DECSCUSR default is a steady block
     (ghostty src/terminal/stream_terminal.zig:154). */
  const char *argv[] = {"/bin/cat", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  g_assert_nonnull(core);
  pt_term_core_sync(core);
  g_assert_cmpint(pt_term_core_cursor_style(core), ==,
                  GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK);
  g_assert_false(pt_term_core_cursor_blinking(core));
  /* Nothing in pt sets password input yet; the renderer handles it anyway. */
  g_assert_false(pt_term_core_cursor_password_input(core));
  pt_term_core_free(core);
}

static void test_cursor_style_decscusr(void) {
  /* 5 is a bar, 3 an underline, 2 a block — ghostty's numbering, pinned by its
     own parser tests (src/terminal/stream.zig:2833). */
  PtTermCore *core = cursor_core_new();
  expect_style(core, "\033[5 q", GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR);
  expect_style(core, "\033[3 q",
               GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE);
  expect_style(core, "\033[2 q",
               GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK);
  pt_term_core_free(core);
}

static void test_cursor_blink_decscusr(void) {
  /* The odd numbers blink and the even ones are steady, same shape either
     way: 5 and 6 are both bars. */
  PtTermCore *core = cursor_core_new();
  expect_blinking(core, "\033[5 q", TRUE);
  expect_blinking(core, "\033[6 q", FALSE);
  want_style = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR;
  g_assert_true(style_is_wanted(core));
  pt_term_core_free(core);
}

static void test_cursor_style_reset_to_default(void) {
  /* `CSI 0 SP q` puts back the default, so an app that asked for a bar and
     exited cannot leave the next program's cursor looking like an insert
     caret. Blink goes back with it. */
  PtTermCore *core = cursor_core_new();
  expect_style(core, "\033[5 q", GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR);
  g_assert_true(pt_term_core_cursor_blinking(core));
  expect_style(core, "\033[0 q",
               GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK);
  want_blinking = FALSE;
  g_assert_true(wait_until(blinking_is_wanted, core));
  pt_term_core_free(core);
}

static void test_cursor_blink_mode_12(void) {
  /* Mode 12 carries the blink on its own, without touching the shape: a shell
     that only ever sets 12 still gets a blinking cursor. */
  PtTermCore *core = cursor_core_new();
  expect_style(core, "\033[3 q",
               GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE);
  expect_blinking(core, "\033[?12h", TRUE);
  want_style = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE;
  g_assert_true(style_is_wanted(core));
  expect_blinking(core, "\033[?12l", FALSE);
  g_assert_true(style_is_wanted(core));
  pt_term_core_free(core);
}

static void test_cursor_wide_tail(void) {
  /* A wide character owns two cells and the second holds nothing of its own.
     Parking the cursor there is the case the renderer has to back up one
     column for, or half a glyph gets a cursor drawn over it. Row 2 keeps this
     clear of the ready marker on row 1. */
  PtTermCore *core = cursor_core_new();
  /* U+6F22 at row 2, columns 1-2; the cursor lands past it, on column 3. */
  pt_term_core_write(core, "\033[2;1H\xE6\xBC\xA2", -1);
  g_assert_true(wait_for_text(core, "\xE6\xBC\xA2"));
  g_assert_true(wait_until(wide_tail_off, core));

  pt_term_core_write(core, "\033[2;2H", -1);   /* onto the tail */
  g_assert_true(wait_until(wide_tail_on, core));

  pt_term_core_write(core, "\033[2;1H", -1);   /* back onto the head */
  g_assert_true(wait_until(wide_tail_off, core));
  pt_term_core_free(core);
}

/* The blink phase goes back to visible on output, so the consumer has to be
   able to tell output from any other reason to redraw. It cannot do that off
   `draw`: scrolling fires that too, and every keypress snaps the viewport back
   to the bottom, so a shell with echo off would have typing pass for output and
   hold the cursor solid forever. */
static void count_draw(PtTermCore *core, gpointer user) {
  (void)core;
  ((Ctx *)user)->draw_count++;
}

static void count_output(PtTermCore *core, gpointer user) {
  (void)core;
  ((Ctx *)user)->output_count++;
}

static void test_output_callback_is_output_only(void) {
  Ctx ctx = {0};
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf 'ready-marker\\n'; exec cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .draw = count_draw, .output = count_output };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  g_assert_true(wait_for_text(core, "ready-marker"));

  /* Bytes from the child are output, and draw with it. */
  g_assert_cmpint(ctx.output_count, >, 0);
  g_assert_cmpint(ctx.draw_count, >=, ctx.output_count);

  /* Moving pt's own view of the terminal is not. */
  int output_before = ctx.output_count;
  int draw_before = ctx.draw_count;
  pt_term_core_scroll_delta(core, -3);
  pt_term_core_scroll_bottom(core);
  pt_term_core_reset(core);
  g_assert_cmpint(ctx.draw_count, >, draw_before);
  g_assert_cmpint(ctx.output_count, ==, output_before);

  pt_term_core_free(core);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/termcore/output", test_output_reaches_grid);
  g_test_add_func("/termcore/exit", test_exit_status_reported);
  g_test_add_func("/termcore/keys", test_key_send_echoes);
  g_test_add_func("/termcore/long-grapheme", test_long_grapheme_cluster);
  g_test_add_func("/termcore/selection", test_selection);
  g_test_add_func("/termcore/hyperlink-is-safe", test_hyperlink_is_safe);
  g_test_add_func("/termcore/hyperlink-at", test_hyperlink_at);
  g_test_add_func("/termcore/foreground-command", test_foreground_command);
  g_test_add_func("/termcore/running-state-idle", test_running_state_idle);
  g_test_add_func("/termcore/running-state-job",
                  test_running_state_foreground_job);
  g_test_add_func("/termcore/spawn-env", test_spawn_env);
  g_test_add_func("/termcore/exit-marker", test_exit_marker_from_title);
  g_test_add_func("/termcore/mouse-report-sgr", test_mouse_report_sgr);
  g_test_add_func("/termcore/mouse-report-off", test_mouse_report_needs_tracking);
  g_test_add_func("/termcore/focus-report", test_focus_report);
  g_test_add_func("/termcore/focus-report-off", test_focus_report_needs_mode);
  g_test_add_func("/termcore/focus-report-dedupe", test_focus_report_dedupes);
  g_test_add_func("/termcore/focus-report-forced", test_focus_report_forced);
  g_test_add_func("/termcore/focus-report-on-enable",
                  test_focus_report_resent_on_mode_enable);
  g_test_add_func("/termcore/in-band-resize", test_in_band_resize_report);
  g_test_add_func("/termcore/in-band-resize-off",
                  test_in_band_resize_needs_mode);
  g_test_add_func("/termcore/in-band-resize-unchanged",
                  test_in_band_resize_unchanged_is_silent);
  g_test_add_func("/termcore/in-band-resize-on-enable",
                  test_in_band_resize_on_mode_enable);
  g_test_add_func("/termcore/alt-screen-arrows", test_alt_screen_arrows);
  g_test_add_func("/termcore/alt-screen-tracking-wheel",
                  test_alt_screen_tracking_wheel);
  g_test_add_func("/termcore/color-scheme-query-dark",
                  test_color_scheme_query_dark);
  g_test_add_func("/termcore/color-scheme-query-light",
                  test_color_scheme_query_light);
  g_test_add_func("/termcore/color-scheme-notify",
                  test_color_scheme_notifies_on_change);
  g_test_add_func("/termcore/color-scheme-needs-mode",
                  test_color_scheme_needs_mode);
  g_test_add_func("/termcore/scroll-bottom", test_scroll_bottom);
  g_test_add_func("/termcore/scrollbar", test_scrollbar_tracks_the_viewport);
  g_test_add_func("/termcore/scrollbar-empty",
                  test_scrollbar_without_scrollback);
  g_test_add_func("/termcore/scrollbar-alt-screen",
                  test_scrollbar_hidden_on_the_alt_screen);
  g_test_add_func("/termcore/scrollbar-cached", test_scrollbar_read_is_cached);
  g_test_add_func("/termcore/reset-mouse-tracking",
                  test_reset_clears_mouse_tracking);
  g_test_add_func("/termcore/reset-grid", test_reset_clears_grid_and_scrollback);
  g_test_add_func("/termcore/reset-keeps-child", test_reset_keeps_the_child);
  g_test_add_func("/termcore/reset-selection", test_reset_clears_selection);
  g_test_add_func("/termcore/reset-osc-scanner", test_reset_clears_osc_scanner);
  g_test_add_func("/termcore/reset-rearms-mode-edges",
                  test_reset_rearms_mode_edges);
  g_test_add_func("/termcore/reset-rearms-in-band-resize",
                  test_reset_rearms_in_band_resize);
  g_test_add_func("/termcore/reset-keeps-color-scheme",
                  test_reset_keeps_color_scheme);
  g_test_add_func("/termcore/osc-callback", test_osc_reaches_callback);
  g_test_add_func("/termcore/osc-unregister", test_osc_consumer_can_unregister);
  g_test_add_func("/termcore/osc52-decode", test_osc52_decode);
  g_test_add_func("/termcore/osc52-decode-cap", test_osc52_decode_cap);
  g_test_add_func("/termcore/osc52-clipboard", test_osc52_clipboard);
  g_test_add_func("/termcore/osc52-primary", test_osc52_primary);
  g_test_add_func("/termcore/osc52-invalid-base64", test_osc52_invalid_base64);
  g_test_add_func("/termcore/osc52-over-cap", test_osc52_over_cap);
  g_test_add_func("/termcore/osc52-off", test_osc52_off_in_config);
  g_test_add_func("/termcore/osc52-query", test_osc52_query_is_never_answered);
  g_test_add_func("/termcore/paste-bracketed",
                  test_paste_bracketed_strips_end_sequence);
  g_test_add_func("/termcore/paste-unbracketed",
                  test_paste_unbracketed_newline_becomes_cr);
  g_test_add_func("/termcore/paste-is-safe", test_paste_is_safe);
  g_test_add_func("/termcore/cursor-defaults", test_cursor_style_defaults);
  g_test_add_func("/termcore/cursor-style-decscusr", test_cursor_style_decscusr);
  g_test_add_func("/termcore/cursor-blink-decscusr", test_cursor_blink_decscusr);
  g_test_add_func("/termcore/cursor-style-default-restore",
                  test_cursor_style_reset_to_default);
  g_test_add_func("/termcore/cursor-blink-mode-12", test_cursor_blink_mode_12);
  g_test_add_func("/termcore/cursor-wide-tail", test_cursor_wide_tail);
  g_test_add_func("/termcore/output-callback-only-for-output",
                  test_output_callback_is_output_only);
  return g_test_run();
}
