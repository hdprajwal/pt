#include "pt-term-core.h"
#include "pt-term-core-internal.h"   /* pt_osc52_decode, the OSC 52 caps */
#include "pt-config.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

typedef struct { GMainLoop *loop; PtTermCore *core;
                 gboolean found; int exit_status; gboolean exited;
                 char comm[64]; char title[128]; int title_count;
                 gboolean title_from_prompt;
                 int osc_code; char osc_payload[128]; int osc_count;
                 char clip[256]; gsize clip_len; gboolean clip_primary;
                 int clip_count;
                 int draw_count, output_count;
                 char notif_title[128]; char notif_body[512];
                 int notif_count; } Ctx;

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

/* The shell name is derived from what the spawn execs, never read back from
 * /proc/<child>/comm — a comm read can race the child's exec and see the
 * parent's own name. Being spawn-derived it is exact and available
 * immediately, even before the exec has happened. */
static void test_shell_name(void) {
  const char *argv[] = {"/bin/sh", "-c", "exit 0", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(pt_term_core_shell_name(core), ==, "sh");
  pt_term_core_free(core);

  /* NULL argv spawns the default shell; the name is $SHELL's basename. */
  char *old = g_strdup(g_getenv("SHELL"));
  g_setenv("SHELL", "/bin/sh", TRUE);
  core = pt_term_core_new("/tmp", NULL, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(pt_term_core_shell_name(core), ==, "sh");
  pt_term_core_free(core);   /* kills+reaps the interactive shell */

  /* A SHELL override in env_pairs reaches the child's environment before the
   * shell is resolved, so it decides what gets exec'd — and the cached name
   * must follow the override, not the parent's own $SHELL (here a decoy that
   * would betray a parent-environment resolution as "fakesh"). */
  g_setenv("SHELL", "/bin/fakesh", TRUE);
  const char *envp[] = { "SHELL=/bin/sh", NULL };
  core = pt_term_core_new("/tmp", NULL, envp, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(pt_term_core_shell_name(core), ==, "sh");
  pt_term_core_free(core);

  if (old != NULL) g_setenv("SHELL", old, TRUE);
  else g_unsetenv("SHELL");
  g_free(old);
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

static gboolean core_running(PtTermCore *c) {
  return pt_term_core_running(c);
}

static gboolean core_not_running(PtTermCore *c) {
  return !pt_term_core_running(c);
}

static gboolean core_exited(PtTermCore *c) {
  return pt_term_core_exited(c, NULL);
}

static void test_running_state_idle(void) {
  /* Negative case: the spawned program is itself the pty's foreground
     process-group leader (forkpty/login_tty), so nothing is "running" on top
     of it. Wait for the tty to actually settle on the child first, otherwise
     the assertion would only exercise the `fg > 0` guard. The answer comes
     from the 700ms foreground poll, so it is waited for, not read once. */
  const char *argv[] = {"/bin/cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_nonnull(core);
  g_assert_true(wait_until(fg_is_child, core));       /* tty settled on cat */
  g_assert_true(wait_until(core_not_running, core));
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
  g_assert_true(wait_until(core_running, core));
  pt_term_core_free(core);
}

static void test_running_state_cleared_on_exit(void) {
  /* The moment the child is gone nothing is running, however recently the
     foreground poll saw a job on the tty — a tab must not keep its spinner
     for up to a poll interval after the shell died. */
  const char *argv[] = {"/bin/sh", "-mc", "sleep 30; true", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_nonnull(core);
  g_assert_true(wait_until(fg_is_not_child, core));   /* sleep owns the tty */
  g_assert_true(wait_until(core_running, core));

  kill(pt_term_core_shell_pid(core), SIGKILL);
  g_assert_true(wait_until(core_exited, core));
  g_assert_false(pt_term_core_running(core));
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
static void on_title_cb(PtTermCore *core, const char *title,
                        gboolean from_prompt, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  ctx->title_count++;
  ctx->title_from_prompt = from_prompt;
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
  g_assert_true(ctx.title_from_prompt);            /* the shell set this one */
  pt_term_core_free(core);
  g_main_loop_unref(ctx.loop);
}

/* A title with no marker came from the program in the pane, not from the
   prompt. The braille frame is what Claude Code and Codex actually animate
   while they work, and it has to arrive untouched: pt shows titles verbatim. */
static void test_program_title_not_from_prompt(void) {
  Ctx ctx = {0};
  ctx.loop = g_main_loop_new(NULL, FALSE);
  const char *argv[] = {"/bin/sh", "-c",
    "printf '\\033]0;\\342\\240\\202 Claude Code\\007'; printf 'done-marker\\n'; "
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
  g_assert_false(ctx.title_from_prompt);
  g_assert_cmpstr(ctx.title, ==, "⠂ Claude Code");   /* verbatim, glyph and all */
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

static void test_wheel_report_batches_notches(void) {
  /* A wheel event can carry several notches; the widget hands all of them to
     the core at once and the core makes one write of it. The bytes have to be
     exactly what one report per notch produced — three identical button-4
     presses, back to back, nothing merged and nothing dropped. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf '\\033[?1000h\\033[?1006h'; cat -v", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_until(tracking_on, core));

  g_assert_true(pt_term_core_wheel_report(core, GHOSTTY_MOUSE_BUTTON_FOUR, 0,
                                          21.0, 20.0, 3));
  g_assert_true(wait_for_text(core, "^[[<64;1;1M^[[<64;1;1M^[[<64;1;1M"));

  /* Nothing to report is not an error, and must not write a stray byte. */
  g_assert_false(pt_term_core_wheel_report(core, GHOSTTY_MOUSE_BUTTON_FIVE, 0,
                                           21.0, 20.0, 0));

  pt_term_core_sync(core);
  char *text = pt_term_core_grid_text(core);
  g_assert_nonnull(text);
  g_assert_null(strstr(text, "^[[<65;"));
  g_free(text);

  pt_term_core_free(core);
}

static void test_wheel_report_needs_tracking(void) {
  /* Same rule as a click: with no mouse mode set the wheel writes nothing. */
  const char *argv[] = {"/bin/cat", NULL};
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, NULL);
  g_assert_nonnull(core);
  g_assert_false(pt_term_core_wheel_report(core, GHOSTTY_MOUSE_BUTTON_FOUR, 0,
                                           21.0, 20.0, 3));
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

/* ---- title dedupe ---- */

static void test_title_dedupes(void) {
  /* Shells re-emit the same OSC 2 title on every prompt; only a real change
     should reach the consumer, which invalidates a tab label off it. Plain
     `cat` echoes the escapes raw, so the parser sees them; the probes prove
     each batch had its turn (the pty is FIFO). */
  Ctx ctx = {0};
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready-marker; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .title = on_title_cb };
  pt_term_core_set_callbacks(core, &cbs, &ctx);
  g_assert_true(wait_for_text(core, "ready-marker"));

  pt_term_core_write(core,
      "\033]2;same-title\007\033]2;same-title\007PROBE-1\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-1"));
  g_assert_cmpint(ctx.title_count, ==, 1);
  g_assert_cmpstr(ctx.title, ==, "same-title");

  /* A different title still comes through: the dedupe compares, not counts. */
  pt_term_core_write(core, "\033]2;other-title\007PROBE-2\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-2"));
  g_assert_cmpint(ctx.title_count, ==, 2);
  g_assert_cmpstr(ctx.title, ==, "other-title");

  pt_term_core_free(core);
}

/* ---- last non-empty row ---- */

static void test_last_nonempty_row(void) {
  /* `cat` echoes what the test writes back through the parser; the newlines
     after "world" leave blank rows below it, so "world" is the last row with
     anything on it and the trailing blanks of its own row are stripped. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf 'ready-marker\\n'; exec cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready-marker"));

  pt_term_core_write(core, "hello\nworld\n\n\n", -1);
  g_assert_true(wait_for_text(core, "world"));
  pt_term_core_sync(core);

  char buf[64];
  g_assert_true(pt_term_core_last_nonempty_row(core, buf, sizeof buf));
  g_assert_cmpstr(buf, ==, "world");

  /* Truncation at cap is NUL-terminated: 4 bytes hold "wor" and the NUL. */
  char small[4];
  memset(small, 'X', sizeof small);
  g_assert_true(pt_term_core_last_nonempty_row(core, small, sizeof small));
  g_assert_cmpuint(strlen(small), <, sizeof small);
  g_assert_cmpstr(small, ==, "wor");

  pt_term_core_free(core);
}

static void test_last_nonempty_row_empty_grid(void) {
  /* A fresh grid nothing has printed to: no row qualifies. */
  const char *argv[] = {"/bin/sh", "-c", "sleep 30", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  pt_term_core_sync(core);
  char buf[64];
  g_assert_false(pt_term_core_last_nonempty_row(core, buf, sizeof buf));
  pt_term_core_free(core);
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
 * pt_term_core_cursor_info answers as of the last sync, so every predicate
 * syncs first. */
static GhosttyRenderStateCursorVisualStyle want_style;
static gboolean want_blinking;
static int want_cx, want_cy, want_cw;

static gboolean style_is_wanted(PtTermCore *c) {
  pt_term_core_sync(c);
  PtCursorInfo info;
  pt_term_core_cursor_info(c, &info);
  return info.style == (int)want_style;
}

static gboolean blinking_is_wanted(PtTermCore *c) {
  pt_term_core_sync(c);
  PtCursorInfo info;
  pt_term_core_cursor_info(c, &info);
  return info.blinking == want_blinking;
}

static gboolean cursor_info_is_wanted(PtTermCore *c) {
  pt_term_core_sync(c);
  PtCursorInfo info;
  if (!pt_term_core_cursor_info(c, &info)) return FALSE;
  return info.x == want_cx && info.y == want_cy && info.width == want_cw;
}

static gboolean cursor_info_hidden(PtTermCore *c) {
  pt_term_core_sync(c);
  PtCursorInfo info;
  pt_term_core_cursor_info(c, &info);
  return !info.visible;
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
  PtCursorInfo info;
  g_assert_true(pt_term_core_cursor_info(core, &info));
  g_assert_cmpint(info.style, ==,
                  GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK);
  g_assert_false(info.blinking);
  /* Nothing in pt sets password input yet; the renderer handles it anyway. */
  g_assert_false(info.password);
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
  want_blinking = TRUE;
  g_assert_true(blinking_is_wanted(core));
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

static void test_cursor_wide_cells(void) {
  /* A wide character owns two cells and the second holds nothing of its own.
     Parking the cursor there is the case cursor_info has to back up one
     column for, or half a glyph gets a cursor drawn over it. Row 2 keeps this
     clear of the ready marker on row 1. */
  PtTermCore *core = cursor_core_new();
  /* U+6F22 at row 2, columns 1-2; the cursor lands past it, on column 3,
     which is narrow. */
  want_cx = 2; want_cy = 1; want_cw = 1;
  pt_term_core_write(core, "\033[2;1H\xE6\xBC\xA2", -1);
  g_assert_true(wait_for_text(core, "\xE6\xBC\xA2"));
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* Onto the spacer tail: x is backed up onto the head and the cursor covers
     both cells. A renderer that widened in place would cover the wrong two. */
  want_cx = 0; want_cy = 1; want_cw = 2;
  pt_term_core_write(core, "\033[2;2H", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* Onto the head: same two cells, no backing up. This is a real transition —
     the raw cursor column moved from 2 to 1 — even though info.x stays 0, so
     the wait below is against the narrow probe that follows. */
  pt_term_core_write(core, "\033[2;1H", -1);
  want_cx = 4; want_cy = 1; want_cw = 1;
  pt_term_core_write(core, "\033[2;5H", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));
  want_cx = 0; want_cy = 1; want_cw = 2;
  pt_term_core_write(core, "\033[2;1H", -1);   /* back onto the head */
  g_assert_true(wait_until(cursor_info_is_wanted, core));
  pt_term_core_free(core);
}

/* ---- flat cell rows ---- */

static void test_row_cells_text_style_colors(void) {
  /* Truecolor SGRs pin exact byte values, so nothing here depends on
     libghostty's stock palette. Row 2 keeps clear of the ready marker. */
  PtTermCore *core = cursor_core_new();
  pt_term_core_write(core,
      "\033[2;1H\033[1;3;4;9;38;2;200;10;20m\033[48;2;1;2;3mA"
      "\033[0mB\033[2;7mC\033[0m", -1);
  g_assert_true(wait_for_text(core, "ABC"));
  pt_term_core_sync(core);

  PtCell cells[80];
  int n = pt_term_core_row_cells(core, 1, cells, 80);
  g_assert_cmpint(n, ==, 80);

  g_assert_cmpstr(cells[0].text, ==, "A");
  g_assert_cmpuint(cells[0].width, ==, 1);
  g_assert_cmpuint(cells[0].style, ==,
                   PT_CELL_STYLE_BOLD | PT_CELL_STYLE_ITALIC |
                   PT_CELL_STYLE_UNDERLINE | PT_CELL_STYLE_STRIKE);
  g_assert_cmpuint(cells[0].fg.r, ==, 200);
  g_assert_cmpuint(cells[0].fg.g, ==, 10);
  g_assert_cmpuint(cells[0].fg.b, ==, 20);
  g_assert_true(cells[0].has_bg);
  g_assert_cmpuint(cells[0].bg.r, ==, 1);
  g_assert_cmpuint(cells[0].bg.g, ==, 2);
  g_assert_cmpuint(cells[0].bg.b, ==, 3);
  g_assert_false(cells[0].selected);

  g_assert_cmpstr(cells[1].text, ==, "B");
  g_assert_cmpuint(cells[1].style, ==, 0);
  g_assert_false(cells[1].has_bg);

  /* Inverse is reported, never applied: the cell keeps its own colors. */
  g_assert_cmpstr(cells[2].text, ==, "C");
  g_assert_cmpuint(cells[2].style, ==,
                   PT_CELL_STYLE_FAINT | PT_CELL_STYLE_INVERSE);
  g_assert_false(cells[2].has_bg);

  /* Past the text: blank cells, narrow. */
  g_assert_cmpstr(cells[3].text, ==, "");
  g_assert_cmpuint(cells[3].width, ==, 1);

  /* max clamps the fill; a row outside the viewport fills nothing. */
  g_assert_cmpint(pt_term_core_row_cells(core, 1, cells, 1), ==, 1);
  g_assert_cmpint(pt_term_core_row_cells(core, 24, cells, 80), ==, 0);
  g_assert_cmpint(pt_term_core_row_cells(core, -1, cells, 80), ==, 0);

  pt_term_core_free(core);
}

static void test_row_cells_wide(void) {
  PtTermCore *core = cursor_core_new();
  /* U+6F22 owns columns 1-2 of row 3; "x" follows on column 3. */
  pt_term_core_write(core, "\033[3;1H\xE6\xBC\xA2x", -1);
  g_assert_true(wait_for_text(core, "\xE6\xBC\xA2"));
  pt_term_core_sync(core);
  PtCell cells[8];
  g_assert_cmpint(pt_term_core_row_cells(core, 2, cells, 8), ==, 8);
  g_assert_cmpstr(cells[0].text, ==, "\xE6\xBC\xA2");
  g_assert_cmpuint(cells[0].width, ==, 2);
  /* The spacer tail holds nothing of its own. */
  g_assert_cmpuint(cells[1].width, ==, 0);
  g_assert_cmpstr(cells[1].text, ==, "");
  g_assert_cmpstr(cells[2].text, ==, "x");
  g_assert_cmpuint(cells[2].width, ==, 1);
  pt_term_core_free(core);
}

static void test_row_cells_selected(void) {
  PtTermCore *core = cursor_core_new();
  pt_term_core_write(core, "\033[2;1HSELECTME", -1);
  g_assert_true(wait_for_text(core, "SELECTME"));
  /* Cells are 8x16 inset by 20/18, so row 1 spans pixels [34, 50); drag
     columns 0..2 of it. */
  pt_term_core_selection_press(core, 21.0, 40.0, 1000000000ULL);
  pt_term_core_selection_drag(core, 37.0, 40.0);
  pt_term_core_selection_release(core, 37.0, 40.0);
  pt_term_core_sync(core);
  PtCell cells[8];
  g_assert_cmpint(pt_term_core_row_cells(core, 1, cells, 8), ==, 8);
  g_assert_true(cells[0].selected);
  g_assert_true(cells[2].selected);
  g_assert_false(cells[3].selected);
  pt_term_core_free(core);
}

static void test_set_colors_reach_cells(void) {
  PtTermCore *core = cursor_core_new();
  PtTermColors colors = {
    .bg = { 5, 6, 7, 1.0 },
    .fg = { 10, 20, 30, 1.0 },
    .cursor = { 40, 50, 60, 1.0 },
  };
  colors.palette[1] = (PtColor){ 170, 16, 32, 1.0 };   /* pin ANSI red */
  pt_term_core_set_colors(core, &colors);
  /* A plain cell and an SGR-31 cell, printed after the colors landed. */
  pt_term_core_write(core, "\033[2;1HX\033[31mR\033[0m", -1);
  g_assert_true(wait_for_text(core, "XR"));
  pt_term_core_sync(core);
  PtCell cells[4];
  g_assert_cmpint(pt_term_core_row_cells(core, 1, cells, 4), ==, 4);
  /* The plain cell reports the theme's default foreground... */
  g_assert_cmpuint(cells[0].fg.r, ==, 10);
  g_assert_cmpuint(cells[0].fg.g, ==, 20);
  g_assert_cmpuint(cells[0].fg.b, ==, 30);
  g_assert_false(cells[0].has_bg);
  /* ...and SGR 31 resolves through the pinned palette slot. */
  g_assert_cmpuint(cells[1].fg.r, ==, 170);
  g_assert_cmpuint(cells[1].fg.g, ==, 16);
  g_assert_cmpuint(cells[1].fg.b, ==, 32);

  /* The effective defaults come back out for the frame's background fill... */
  PtColor bg = {0}, fg = {0};
  pt_term_core_default_colors(core, &bg, &fg);
  g_assert_cmpuint(bg.r, ==, 5);
  g_assert_cmpuint(bg.g, ==, 6);
  g_assert_cmpuint(bg.b, ==, 7);
  g_assert_cmpuint(fg.r, ==, 10);
  g_assert_cmpuint(fg.g, ==, 20);
  g_assert_cmpuint(fg.b, ==, 30);

  /* ...and the cursor color rides on cursor_info, already resolved. */
  PtCursorInfo info;
  pt_term_core_cursor_info(core, &info);
  g_assert_cmpuint(info.color.r, ==, 40);
  g_assert_cmpuint(info.color.g, ==, 50);
  g_assert_cmpuint(info.color.b, ==, 60);
  pt_term_core_free(core);
}

/* ---- sequential row walk ---- */

static void test_rows_walk_matches_row_cells(void) {
  /* The one-pass reader must agree with the random-access read cell for cell:
     styles, colors, a wide char and a link all in play. fill_cell zeroes each
     cell before filling, so memcmp covers padding and text tails too. */
  PtTermCore *core = cursor_core_new();
  pt_term_core_write(core,
      "\033[2;1H\033[1;31mred\033[0m \xE6\xBC\xA2 "
      "\033]8;;https://example.com/x\033\\link\033]8;;\033\\", -1);
  g_assert_true(wait_for_text(core, "link"));
  pt_term_core_sync(core);

  PtRowReader *r = pt_term_core_rows_begin(core);
  g_assert_nonnull(r);
  const PtCell *walked;
  PtCell sought[80];
  int rows = 0;
  int n;
  while ((n = pt_term_core_rows_next(r, &walked)) >= 0) {
    g_assert_cmpint(pt_term_core_row_cells(core, rows, sought, 80), ==, n);
    g_assert_cmpint(memcmp(walked, sought, (size_t)n * sizeof(PtCell)), ==, 0);
    rows++;
  }
  /* Past the last row the walk stays done. */
  g_assert_cmpint(pt_term_core_rows_next(r, &walked), ==, -1);
  pt_term_core_rows_end(r);
  g_assert_cmpint(rows, ==, 24);
  pt_term_core_free(core);
}

static void test_rows_walk_wide_pane(void) {
  /* A pane wider than any fixed row buffer: 600 columns. Nothing may be
     truncated — the reader grows its buffer to the row, and text parked past
     column 512 comes back through both read paths. */
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf 'ready-marker\\n'; exec cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 600, 24, 8, 16, &err);
  g_assert_no_error(err);
  g_assert_true(wait_for_text(core, "ready-marker"));
  /* Row 2, columns 591-594 (1-based): all beyond the old 512-cell clip. */
  pt_term_core_write(core, "\033[2;591HWIDE", -1);
  g_assert_true(wait_for_text(core, "WIDE"));
  pt_term_core_sync(core);

  static PtCell sought[600];   /* static: too big for the test's stack */
  g_assert_cmpint(pt_term_core_row_cells(core, 1, sought, 600), ==, 600);
  g_assert_cmpstr(sought[590].text, ==, "W");
  g_assert_cmpstr(sought[593].text, ==, "E");

  PtRowReader *r = pt_term_core_rows_begin(core);
  g_assert_nonnull(r);
  const PtCell *walked;
  g_assert_cmpint(pt_term_core_rows_next(r, &walked), ==, 600);  /* row 0 */
  g_assert_cmpint(pt_term_core_rows_next(r, &walked), ==, 600);  /* row 1 */
  g_assert_cmpstr(walked[590].text, ==, "W");
  g_assert_cmpstr(walked[591].text, ==, "I");
  g_assert_cmpstr(walked[592].text, ==, "D");
  g_assert_cmpstr(walked[593].text, ==, "E");
  g_assert_cmpint(memcmp(walked, sought, sizeof sought), ==, 0);
  pt_term_core_rows_end(r);
  pt_term_core_free(core);
}

/* ---- row/cell link helpers ---- */

static void test_row_link_helpers(void) {
  /* Same two links as test_hyperlink_at: an https one pt opens on row 1 (row
     2 keeps clear of the ready marker) and a javascript: one it must not on
     row 2 — written through cat so the OSC 8 pairs arrive off the pty. */
  PtTermCore *core = cursor_core_new();
  pt_term_core_write(core,
      "\033[2;1H\033]8;;https://example.com/x\033\\GOOD\033]8;;\033\\"
      "\033[3;1H\033]8;;javascript:alert(1)\033\\EVIL\033]8;;\033\\", -1);
  g_assert_true(wait_for_text(core, "EVIL"));
  pt_term_core_sync(core);

  /* The row flag: linked rows answer yes — the unopenable link too, because
     the flag feeds the underline and the underline must stay honest. */
  g_assert_true(pt_term_core_row_has_link(core, 1));
  g_assert_true(pt_term_core_row_has_link(core, 2));
  g_assert_false(pt_term_core_row_has_link(core, 3));
  g_assert_false(pt_term_core_row_has_link(core, -1));
  g_assert_false(pt_term_core_row_has_link(core, 24));

  /* The per-cell flag the underline pass draws from, on the flat rows. */
  PtCell cells[8];
  g_assert_cmpint(pt_term_core_row_cells(core, 1, cells, 8), ==, 8);
  g_assert_true(cells[0].has_link);
  g_assert_true(cells[3].has_link);
  g_assert_false(cells[4].has_link);
  g_assert_cmpint(pt_term_core_row_cells(core, 2, cells, 8), ==, 8);
  g_assert_true(cells[0].has_link);          /* unsafe scheme still underlines */

  /* The URI itself, by cell: hyperlink_at's rules, addressed by (row, col). */
  char *uri = pt_term_core_link_at_cell(core, 1, 0);
  g_assert_cmpstr(uri, ==, "https://example.com/x");
  g_free(uri);
  uri = pt_term_core_link_at_cell(core, 1, 3);
  g_assert_cmpstr(uri, ==, "https://example.com/x");
  g_free(uri);
  g_assert_null(pt_term_core_link_at_cell(core, 1, 4));   /* past the text */
  g_assert_null(pt_term_core_link_at_cell(core, 2, 0));   /* linked, unopenable */
  g_assert_null(pt_term_core_link_at_cell(core, -1, 0));
  g_assert_null(pt_term_core_link_at_cell(core, 1, -1));
  g_assert_null(pt_term_core_link_at_cell(core, 24, 0));
  g_assert_null(pt_term_core_link_at_cell(core, 1, 80));
  pt_term_core_free(core);
}

/* ---- logical lines ----

   What the bare-URL matcher runs against: a row as one string, plus the cell
   each byte came from. Rows a program wrapped are one line here, because a URL
   that ran off the right edge is still one URL. */

/* The byte offset of `needle` in the line, asserted to exist. */
static gsize line_off(const PtLine *l, const char *needle) {
  const char *at = strstr(l->text, needle);
  g_assert_nonnull(at);
  return (gsize)(at - l->text);
}

static void test_line_at_plain(void) {
  PtTermCore *core = cursor_core_new();
  /* Column 5 of row 1, so the map has a non-zero column to report. */
  pt_term_core_write(core, "\033[2;6Hgo http://localhost:5173/ now", -1);
  g_assert_true(wait_for_text(core, "5173"));
  pt_term_core_sync(core);

  PtLine line;
  g_assert_true(pt_term_core_line_at(core, 1, &line));
  /* The row is rendered whole: blanks are spaces, so offsets are columns. */
  g_assert_cmpuint(line.len, ==, 80);
  gsize off = line_off(&line, "http://localhost:5173/");
  g_assert_cmpuint(off, ==, 8);              /* 5 blank + "go " */
  g_assert_cmpint(line.at[off].row, ==, 1);
  g_assert_cmpint(line.at[off].col, ==, 8);
  /* Every byte maps to the cell that drew it, so a span maps back to cells. */
  g_assert_cmpint(line.at[off + 4].col, ==, 12);
  g_assert_cmpint(line.at[line.len - 1].col, ==, 79);
  pt_term_core_line_clear(&line);

  /* A row outside the viewport has no line. */
  g_assert_false(pt_term_core_line_at(core, 24, &line));
  g_assert_false(pt_term_core_line_at(core, -1, &line));
  pt_term_core_free(core);
}

/* A URL that ran off the right edge is one link, so the two rows the program
   wrapped are one line — and either row answers with it. */
static void test_line_at_wrapped(void) {
  PtTermCore *core = cursor_core_new();
  /* 70 columns of padding, then a URL long enough to cross the edge at 80. */
  pt_term_core_write(core,
      "\033[2;1H" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaa" "https://example.com/wrapped/path", -1);
  g_assert_true(wait_for_text(core, "wrapped"));
  pt_term_core_sync(core);

  PtLine line;
  g_assert_true(pt_term_core_line_at(core, 1, &line));
  g_assert_cmpuint(line.len, ==, 160);       /* two rows joined, no seam */
  gsize off = line_off(&line, "https://example.com/wrapped/path");
  g_assert_cmpuint(off, ==, 70);
  g_assert_cmpint(line.at[off].row, ==, 1);
  g_assert_cmpint(line.at[off].col, ==, 70);
  /* Past the wrap the map moves to the next row, column 0. */
  g_assert_cmpint(line.at[80].row, ==, 2);
  g_assert_cmpint(line.at[80].col, ==, 0);
  pt_term_core_line_clear(&line);

  /* The continuation row answers with the same line, not with its half. */
  PtLine tail;
  g_assert_true(pt_term_core_line_at(core, 2, &tail));
  g_assert_cmpuint(tail.len, ==, 160);
  g_assert_cmpuint(line_off(&tail, "https://example.com/wrapped/path"), ==, 70);
  pt_term_core_line_clear(&tail);
  pt_term_core_free(core);
}

/* ---- cursor info ---- */

static void test_cursor_info(void) {
  PtTermCore *core = cursor_core_new();
  PtCursorInfo info;
  /* cursor_info answers as of the last sync, like the flat rows. After the
     ready marker the cursor sits at row 1, column 0. */
  pt_term_core_sync(core);
  g_assert_true(pt_term_core_cursor_info(core, &info));
  g_assert_cmpint(info.x, ==, 0);
  g_assert_cmpint(info.y, ==, 1);
  g_assert_cmpint(info.width, ==, 1);
  g_assert_cmpint(info.style, ==,
                  GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK);
  g_assert_true(info.visible);
  g_assert_false(info.blinking);
  /* Nothing in pt can set password input yet (see the header). */
  g_assert_false(info.password);

  /* Printing advances x. */
  want_cx = 2; want_cy = 1; want_cw = 1;
  pt_term_core_write(core, "ab", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* On the spacer tail of a wide char, x is already backed up onto the head
     and the cursor covers both cells. */
  want_cx = 0; want_cy = 2; want_cw = 2;
  pt_term_core_write(core, "\033[3;1H\xE6\xBC\xA2\033[3;2H", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* On the head: same two cells, no backing up. */
  want_cx = 0; want_cy = 2; want_cw = 2;
  pt_term_core_write(core, "\033[3;1H", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* Past the wide char the cursor is narrow again. */
  want_cx = 3; want_cy = 2; want_cw = 1;
  pt_term_core_write(core, "\033[3;4H", -1);
  g_assert_true(wait_until(cursor_info_is_wanted, core));

  /* DECTCEM hides it; position stays valid. */
  pt_term_core_write(core, "\033[?25l", -1);
  g_assert_true(wait_until(cursor_info_hidden, core));
  pt_term_core_sync(core);
  g_assert_true(pt_term_core_cursor_info(core, &info));

  pt_term_core_free(core);
}

/* ---- render dirty ---- */

/* Let any in-flight pty reads land so a take-and-clear assertion cannot race
   a byte that was already on its way. */
static void drain_reads(void) {
  for (int i = 0; i < 20; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(2000);
  }
}

static void test_take_render_dirty(void) {
  PtTermCore *core = cursor_core_new();   /* output has arrived already */
  drain_reads();
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));

  /* Bytes from the child set it again — exactly once. */
  pt_term_core_write(core, "probe\n", -1);
  g_assert_true(wait_for_text(core, "probe"));
  drain_reads();
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));

  /* Moving the viewport is a render change with no output. */
  pt_term_core_scroll_delta(core, -1);
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));

  /* So are a resize and a theme's colors landing. */
  pt_term_core_resize(core, 81, 24, 8, 16);
  g_assert_true(pt_term_core_take_render_dirty(core));
  PtTermColors colors = { .fg = { 1, 2, 3, 1.0 } };
  pt_term_core_set_colors(core, &colors);
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));

  /* And so is the selection changing. */
  pt_term_core_selection_press(core, 21.0, 20.0, 1000000000ULL);
  pt_term_core_selection_drag(core, 60.0, 20.0);
  pt_term_core_selection_release(core, 60.0, 20.0);
  g_assert_true(pt_term_core_take_render_dirty(core));
  pt_term_core_selection_clear(core);
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_false(pt_term_core_take_render_dirty(core));

  pt_term_core_free(core);
}

static void test_content_serial(void) {
  PtTermCore *core = cursor_core_new();
  drain_reads();
  guint s0 = pt_term_core_content_serial(core);
  /* Readers move nothing: not this getter, and not the take. */
  g_assert_cmpuint(pt_term_core_content_serial(core), ==, s0);
  g_assert_true(pt_term_core_take_render_dirty(core));
  g_assert_cmpuint(pt_term_core_content_serial(core), ==, s0);

  /* Bytes from the child move it. */
  pt_term_core_write(core, "serial-probe\n", -1);
  g_assert_true(wait_for_text(core, "serial-probe"));
  drain_reads();
  guint s1 = pt_term_core_content_serial(core);
  g_assert_cmpuint(s1, >, s0);
  g_assert_true(pt_term_core_take_render_dirty(core));

  /* So does a pure viewport move, with no output at all. */
  pt_term_core_scroll_delta(core, -1);
  g_assert_cmpuint(pt_term_core_content_serial(core), >, s1);
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

/* ---- desktop notifications (OSC 9, OSC 777) ----
 *
 * How each payload is classified is pinned in test-osc-scan against ghostty's
 * own tests. What is proved here is the rest of the path: that the scanner is
 * wired to the notification callback on a live pty, that a focused pane is
 * silent, that the rate limit holds, and that a long body is cut rather than
 * dropped. */

static void on_notification(PtTermCore *core, const char *title,
                            const char *body, gpointer user) {
  (void)core;
  Ctx *ctx = user;
  ctx->notif_count++;
  g_strlcpy(ctx->notif_title, title, sizeof ctx->notif_title);
  g_strlcpy(ctx->notif_body, body, sizeof ctx->notif_body);
}

/* A shell that echoes whatever is written to it back down the pty, raw, so a
 * test can post arbitrary sequences with pt_term_core_write and know the
 * scanner saw exactly those bytes. `cat` and not `cat -v`: the escapes have to
 * come back as escapes. */
static PtTermCore *notify_core(Ctx *ctx) {
  /* The rate limit is process-wide (as ghostty's is), so back-to-back tests in
     one binary would rate-limit each other. */
  pt_notify_gate_reset();
  const char *argv[] = {"/bin/sh", "-c",
    "stty -echo -icanon; printf ready-marker; cat", NULL};
  GError *err = NULL;
  PtTermCore *core = pt_term_core_new("/tmp", argv, NULL, 80, 24, 8, 16, &err);
  g_assert_no_error(err);
  PtTermCoreCallbacks cbs = { .notification = on_notification };
  pt_term_core_set_callbacks(core, &cbs, ctx);
  g_assert_true(wait_for_text(core, "ready-marker"));
  return core;
}

static void test_notification_osc9(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  pt_term_core_write(core, "\033]9;build done\007PROBE-9\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-9"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_cmpstr(ctx.notif_body, ==, "build done");
  /* OSC 9 carries no title; naming it is the consumer's job. */
  g_assert_cmpstr(ctx.notif_title, ==, "");
  pt_term_core_free(core);
}

static void test_notification_osc777(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  pt_term_core_write(core, "\033]777;notify;Build;done\007PROBE-777\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-777"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_cmpstr(ctx.notif_title, ==, "Build");
  g_assert_cmpstr(ctx.notif_body, ==, "done");
  pt_term_core_free(core);
}

/* The two payloads that must reach the callback as nothing at all: an OSC 777
   extension pt does not implement, and the ConEmu progress report that shares
   OSC 9 with the notification (issue #17 owns that one). */
static void test_notification_ignores_the_rest(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  pt_term_core_write(core,
      "\033]777;something-else;a;b\007"
      "\033]9;4;1;40\007"
      "\033]9;9;/home/me\007"
      "PROBE-NONE\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-NONE"));
  g_assert_cmpint(ctx.notif_count, ==, 0);
  pt_term_core_free(core);
}

/* A pane the user is reading says what it has to say on screen; the desktop
   does not need telling. c->focused is what the widget reported, and GTK takes
   focus off a pane when its window goes inactive, so this covers "the focused
   pane of the focused window" and nothing wider. */
static void test_notification_silent_while_focused(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  pt_term_core_focus_report(core, TRUE, FALSE);
  pt_term_core_write(core, "\033]9;while focused\007PROBE-FOCUSED\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-FOCUSED"));
  g_assert_cmpint(ctx.notif_count, ==, 0);

  /* And the same pane notifies again the moment focus leaves it — the
     suppressed one must not have spent the rate limit on the way past. */
  pt_term_core_focus_report(core, FALSE, FALSE);
  pt_term_core_write(core, "\033]9;while away\007PROBE-AWAY\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-AWAY"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_cmpstr(ctx.notif_body, ==, "while away");
  pt_term_core_free(core);
}

/* A program in a loop must not be able to queue thousands of notifications at
   the desktop. Bodies are all different so it is the one-per-second limit
   being measured and not the identical-text suppressor. */
static void test_notification_rate_limited(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  GString *burst = g_string_new(NULL);
  for (int i = 0; i < 100; i++)
    g_string_append_printf(burst, "\033]9;notification %d\007", i);
  g_string_append(burst, "PROBE-BURST\n");
  gint64 started = g_get_monotonic_time();
  pt_term_core_write(core, burst->str, (gssize)burst->len);
  g_string_free(burst, TRUE);
  g_assert_true(wait_for_text(core, "PROBE-BURST"));
  /* One per second, so a burst that took under a second to arrive can only
     have produced one — two if the pty split it either side of a tick. */
  g_assert_cmpint(ctx.notif_count, >=, 1);
  g_assert_cmpint(ctx.notif_count, <=, 2);
  g_assert_cmpint(g_get_monotonic_time() - started, <, 2 * G_USEC_PER_SEC);
  pt_term_core_free(core);
}

/* Truncated, not dropped: a build that ends by printing a long line should
   still notify, even if the notification only carries the front of it. */
static void test_notification_body_is_capped(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  GString *big = g_string_new("\033]9;");
  for (int i = 0; i < PT_NOTIFY_BODY_MAX + 200; i++)
    g_string_append_c(big, 'x');
  g_string_append(big, "\007PROBE-CAP\n");
  pt_term_core_write(core, big->str, (gssize)big->len);
  g_string_free(big, TRUE);
  g_assert_true(wait_for_text(core, "PROBE-CAP"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_cmpuint(strlen(ctx.notif_body), ==, PT_NOTIFY_BODY_MAX);
  for (gsize i = 0; i < PT_NOTIFY_BODY_MAX; i++)
    g_assert_cmpint(ctx.notif_body[i], ==, 'x');
  pt_term_core_free(core);
}

/* The cap lands on a character boundary. A body of three-byte characters is
   the awkward case: 255 is not a multiple of 3, so cutting at the byte cap
   would hand the session bus half a codepoint. */
static void test_notification_cap_keeps_utf8_whole(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  GString *big = g_string_new("\033]9;");
  for (int i = 0; i < 200; i++) g_string_append(big, "\344\270\200");  /* U+4E00 */
  g_string_append(big, "\007PROBE-UTF8\n");
  pt_term_core_write(core, big->str, (gssize)big->len);
  g_string_free(big, TRUE);
  g_assert_true(wait_for_text(core, "PROBE-UTF8"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_true(g_utf8_validate(ctx.notif_body, -1, NULL));
  /* 85 whole characters is as many as fit in 255 bytes, so the cut is at 255
     here; 254 would mean a character was dropped that fitted. */
  g_assert_cmpuint(strlen(ctx.notif_body), ==, 255);
  pt_term_core_free(core);
}

/* A notification body is offered to the desktop as UTF-8, and the payload came
   from whatever was writing to the pty. Bytes that are not text are refused
   outright rather than sent on as something the session bus will reject —
   the same rule pt already applies to OSC 52 clipboard writes. */
static void test_notification_rejects_non_utf8(void) {
  Ctx ctx = {0};
  PtTermCore *core = notify_core(&ctx);
  pt_term_core_write(core, "\033]9;bad \377\376 bytes\007PROBE-BYTES\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-BYTES"));
  g_assert_cmpint(ctx.notif_count, ==, 0);

  /* And a well-formed one right behind it still gets through, so the refusal
     costs the next notification nothing. */
  pt_term_core_write(core, "\033]9;good\007PROBE-GOOD\n", -1);
  g_assert_true(wait_for_text(core, "PROBE-GOOD"));
  g_assert_cmpint(ctx.notif_count, ==, 1);
  g_assert_cmpstr(ctx.notif_body, ==, "good");
  pt_term_core_free(core);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/termcore/output", test_output_reaches_grid);
  g_test_add_func("/termcore/exit", test_exit_status_reported);
  g_test_add_func("/termcore/shell-name", test_shell_name);
  g_test_add_func("/termcore/keys", test_key_send_echoes);
  g_test_add_func("/termcore/long-grapheme", test_long_grapheme_cluster);
  g_test_add_func("/termcore/selection", test_selection);
  g_test_add_func("/termcore/hyperlink-is-safe", test_hyperlink_is_safe);
  g_test_add_func("/termcore/hyperlink-at", test_hyperlink_at);
  g_test_add_func("/termcore/foreground-command", test_foreground_command);
  g_test_add_func("/termcore/running-state-idle", test_running_state_idle);
  g_test_add_func("/termcore/running-state-job",
                  test_running_state_foreground_job);
  g_test_add_func("/termcore/running-state-exit",
                  test_running_state_cleared_on_exit);
  g_test_add_func("/termcore/spawn-env", test_spawn_env);
  g_test_add_func("/termcore/exit-marker", test_exit_marker_from_title);
  g_test_add_func("/termcore/program-title-not-from-prompt",
                  test_program_title_not_from_prompt);
  g_test_add_func("/termcore/title-dedupe", test_title_dedupes);
  g_test_add_func("/termcore/mouse-report-sgr", test_mouse_report_sgr);
  g_test_add_func("/termcore/wheel-report-batch",
                  test_wheel_report_batches_notches);
  g_test_add_func("/termcore/wheel-report-off",
                  test_wheel_report_needs_tracking);
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
  g_test_add_func("/termcore/last-nonempty-row", test_last_nonempty_row);
  g_test_add_func("/termcore/last-nonempty-row-empty",
                  test_last_nonempty_row_empty_grid);
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
  g_test_add_func("/termcore/notification-osc9", test_notification_osc9);
  g_test_add_func("/termcore/notification-osc777", test_notification_osc777);
  g_test_add_func("/termcore/notification-ignores-the-rest",
                  test_notification_ignores_the_rest);
  g_test_add_func("/termcore/notification-focused",
                  test_notification_silent_while_focused);
  g_test_add_func("/termcore/notification-rate-limit",
                  test_notification_rate_limited);
  g_test_add_func("/termcore/notification-body-cap",
                  test_notification_body_is_capped);
  g_test_add_func("/termcore/notification-cap-utf8",
                  test_notification_cap_keeps_utf8_whole);
  g_test_add_func("/termcore/notification-non-utf8",
                  test_notification_rejects_non_utf8);
  g_test_add_func("/termcore/cursor-defaults", test_cursor_style_defaults);
  g_test_add_func("/termcore/cursor-style-decscusr", test_cursor_style_decscusr);
  g_test_add_func("/termcore/cursor-blink-decscusr", test_cursor_blink_decscusr);
  g_test_add_func("/termcore/cursor-style-default-restore",
                  test_cursor_style_reset_to_default);
  g_test_add_func("/termcore/cursor-blink-mode-12", test_cursor_blink_mode_12);
  g_test_add_func("/termcore/cursor-wide-cells", test_cursor_wide_cells);
  g_test_add_func("/termcore/row-cells", test_row_cells_text_style_colors);
  g_test_add_func("/termcore/row-cells-wide", test_row_cells_wide);
  g_test_add_func("/termcore/row-cells-selected", test_row_cells_selected);
  g_test_add_func("/termcore/rows-walk", test_rows_walk_matches_row_cells);
  g_test_add_func("/termcore/rows-walk-wide", test_rows_walk_wide_pane);
  g_test_add_func("/termcore/row-link-helpers", test_row_link_helpers);
  g_test_add_func("/termcore/line-at-plain", test_line_at_plain);
  g_test_add_func("/termcore/line-at-wrapped", test_line_at_wrapped);
  g_test_add_func("/termcore/set-colors", test_set_colors_reach_cells);
  g_test_add_func("/termcore/cursor-info", test_cursor_info);
  g_test_add_func("/termcore/render-dirty", test_take_render_dirty);
  g_test_add_func("/termcore/content-serial", test_content_serial);
  g_test_add_func("/termcore/output-callback-only-for-output",
                  test_output_callback_is_output_only);
  return g_test_run();
}
