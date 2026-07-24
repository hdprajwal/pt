#include "pt-term-core.h"
#include <string.h>

typedef struct { GMainLoop *loop; PtTermCore *core;
                 gboolean found; int exit_status; gboolean exited; } Ctx;

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
  PtTermCore *core = pt_term_core_new("/tmp", argv, 80, 24, 8, 16, &err);
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
  PtTermCore *core = pt_term_core_new("/tmp", argv, 80, 24, 8, 16, NULL);
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
  PtTermCore *core = pt_term_core_new("/tmp", argv, 80, 24, 8, 16, NULL);
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
  PtTermCore *core = pt_term_core_new("/tmp", argv, 80, 24, 8, 16, &err);
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

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/termcore/output", test_output_reaches_grid);
  g_test_add_func("/termcore/exit", test_exit_status_reported);
  g_test_add_func("/termcore/keys", test_key_send_echoes);
  g_test_add_func("/termcore/long-grapheme", test_long_grapheme_cluster);
  return g_test_run();
}
