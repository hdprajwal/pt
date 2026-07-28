#include "pt-term-core.h"
#include <stdio.h>
#include <string.h>

typedef struct { GMainLoop *loop; PtTermCore *core;
                 gboolean found; int exit_status; gboolean exited;
                 char comm[64]; char title[128];
                 int osc_code; char osc_payload[128]; int osc_count; } Ctx;

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
  g_test_add_func("/termcore/alt-screen-arrows", test_alt_screen_arrows);
  g_test_add_func("/termcore/scroll-bottom", test_scroll_bottom);
  g_test_add_func("/termcore/osc-callback", test_osc_reaches_callback);
  g_test_add_func("/termcore/osc-unregister", test_osc_consumer_can_unregister);
  g_test_add_func("/termcore/paste-bracketed",
                  test_paste_bracketed_strips_end_sequence);
  g_test_add_func("/termcore/paste-unbracketed",
                  test_paste_unbracketed_newline_becomes_cr);
  g_test_add_func("/termcore/paste-is-safe", test_paste_is_safe);
  return g_test_run();
}
