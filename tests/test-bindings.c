#include "pt-bindings.h"
#include <string.h>

/* Index of the one entry parse produced for an accel, or -1. */
static int find_accel(GPtrArray *b, const char *accel) {
  for (guint i = 0; i < b->len; i++) {
    const PtBindingLine *e = g_ptr_array_index(b, i);
    if (strcmp(e->accel, accel) == 0) return (int)i;
  }
  return -1;
}

static void test_valid_lines(void) {
  const char *lines[] = {
    "bind ctrl+shift+t new-tab",
    "unbind alt+1",
    NULL,
  };
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 2);

  const PtBindingLine *bind = g_ptr_array_index(b, 0);
  g_assert_cmpstr(bind->accel, ==, "<Control><Shift>t");
  g_assert_cmpstr(bind->action, ==, "new-tab");
  g_assert_false(bind->is_unbind);
  g_assert_cmpint(bind->line_no, ==, 1);

  const PtBindingLine *unbind = g_ptr_array_index(b, 1);
  g_assert_cmpstr(unbind->accel, ==, "<Alt>1");
  g_assert_null(unbind->action);
  g_assert_true(unbind->is_unbind);
  g_assert_cmpint(unbind->line_no, ==, 2);
  pt_bindings_free(b);
}

static void test_action_lookup(void) {
  PtActionId id;
  int arg;
  g_assert_true(pt_bindings_action_lookup("Pane-Zoom", &id, &arg));
  g_assert_cmpuint(id, ==, PT_ACTION_ZOOM_PANE);
  g_assert_true(pt_bindings_action_lookup("switch-project-9", &id, &arg));
  g_assert_cmpuint(id, ==, PT_ACTION_SWITCH_PROJECT);
  g_assert_cmpint(arg, ==, 8);
  g_assert_true(pt_bindings_action_lookup("split-h", &id, &arg));
  g_assert_cmpuint(id, ==, PT_ACTION_SPLIT);
  g_assert_cmpint(arg, ==, PT_SPLIT_H);
  g_assert_true(pt_bindings_action_lookup("font-zoom-out", &id, &arg));
  g_assert_cmpint(arg, ==, -1);
  g_assert_false(pt_bindings_action_lookup("no-such-action", &id, &arg));
  g_assert_false(pt_bindings_action_lookup("", &id, &arg));
}

static void test_modifiers(void) {
  /* Every modifier alone, then all four stacked in scrambled order: the
   * canonical spelling fixes one order whatever the config wrote. */
  const char *lines[] = {
    "bind ctrl+a paste",
    "bind shift+b copy",
    "bind alt+c paste",
    "bind super+d copy",
    "bind super+alt+shift+ctrl+e paste",
    NULL,
  };
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 5);
  g_assert_cmpint(find_accel(b, "<Control>a"), >=, 0);
  g_assert_cmpint(find_accel(b, "<Shift>b"), >=, 0);
  g_assert_cmpint(find_accel(b, "<Alt>c"), >=, 0);
  g_assert_cmpint(find_accel(b, "<Super>d"), >=, 0);
  g_assert_cmpint(find_accel(b, "<Control><Shift><Alt><Super>e"), >=, 0);
  pt_bindings_free(b);
}

static void test_named_keys(void) {
  /* Punctuation travels by name only, and every named key lands on GTK's
   * spelling of the same key. Letters, digits and the function-key range
   * round it out, including both edges of f1..f24. */
  const struct { const char *key, *gtk; } keys[] = {
    { "a", "a" },                  { "z", "z" },
    { "0", "0" },                  { "9", "9" },
    { "f1", "F1" },                { "f24", "F24" },
    { "enter", "Return" },         { "tab", "Tab" },
    { "escape", "Escape" },        { "space", "space" },
    { "up", "Up" },                { "down", "Down" },
    { "left", "Left" },            { "right", "Right" },
    { "pgup", "Page_Up" },         { "pgdn", "Page_Down" },
    { "home", "Home" },            { "end", "End" },
    { "delete", "Delete" },        { "backspace", "BackSpace" },
    { "equal", "equal" },          { "plus", "plus" },
    { "minus", "minus" },          { "comma", "comma" },
    { "period", "period" },        { "slash", "slash" },
    { "backslash", "backslash" },  { "semicolon", "semicolon" },
    { "quote", "apostrophe" },
    { "bracketleft", "bracketleft" },
    { "bracketright", "bracketright" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
    char *line = g_strdup_printf("bind ctrl+%s paste", keys[i].key);
    const char *one[] = { line, NULL };
    GPtrArray *r = pt_bindings_parse(one, NULL, NULL);
    char *want = g_strdup_printf("<Control>%s", keys[i].gtk);
    if (r->len != 1 ||
        strcmp(((PtBindingLine *)g_ptr_array_index(r, 0))->accel,
               want) != 0)
      g_error("key %s -> %s, wanted %s", keys[i].key,
              r->len == 1
                  ? ((PtBindingLine *)g_ptr_array_index(r, 0))->accel
                  : "(skipped)",
              want);
    pt_bindings_free(r);
    g_free(want);
    g_free(line);
  }
}

static void test_case_insensitive(void) {
  /* Verbs, modifiers, key names and action names all read either case. */
  const char *lines[] = { "BIND CTRL+Z Pane-Zoom", NULL };
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  const PtBindingLine *e = g_ptr_array_index(b, 0);
  g_assert_cmpstr(e->accel, ==, "<Control>z");
  g_assert_cmpstr(e->action, ==, "pane-zoom");
  pt_bindings_free(b);
}

static void test_unknown_action(void) {
  /* A bad action skips its own line only. */
  const char *lines[] = {
    "bind ctrl+z no-such-action",
    "bind ctrl+b toggle-sidebar",
    NULL,
  };
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  g_assert_cmpint(find_accel(b, "<Control>b"), >=, 0);
  g_assert_cmpint(find_accel(b, "<Control>z"), ==, -1);
  pt_bindings_free(b);
}

static void test_bad_accel(void) {
  /* Unknown key names, empty pieces around a literal `+`, a bare modifier,
   * and function keys off both ends of the range are all refused. */
  const char *bad[] = {
    "bind ctrl+foo new-tab",
    "bind zzz new-tab",
    "bind ctrl++z new-tab",
    "bind ctrl+a+ new-tab",
    "bind ctrl new-tab",
    "bind f25 new-tab",
    "bind f0 new-tab",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    const char *one[] = { bad[i], "bind ctrl+b toggle-sidebar", NULL };
    GPtrArray *b = pt_bindings_parse(one, NULL, NULL);
    g_assert_cmpuint(b->len, ==, 1);
    g_assert_cmpint(find_accel(b, "<Control>b"), >=, 0);
    pt_bindings_free(b);
  }
}

static void test_duplicate_accel(void) {
  /* One accelerator, one meaning: the later line replaces the earlier one,
   * whether it binds over a bind or an unbind. */
  const char *rebind[] = {
    "bind ctrl+b toggle-sidebar",
    "bind ctrl+b add-project",
    NULL,
  };
  GPtrArray *b = pt_bindings_parse(rebind, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  g_assert_cmpstr(((PtBindingLine *)g_ptr_array_index(b, 0))->action, ==,
                  "add-project");
  pt_bindings_free(b);

  const char *unbind_over[] = {
    "bind ctrl+b toggle-sidebar",
    "unbind ctrl+b",
    NULL,
  };
  b = pt_bindings_parse(unbind_over, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  g_assert_true(((PtBindingLine *)g_ptr_array_index(b, 0))->is_unbind);
  pt_bindings_free(b);

  const char *bind_over[] = {
    "unbind ctrl+b",
    "bind ctrl+b toggle-sidebar",
    NULL,
  };
  b = pt_bindings_parse(bind_over, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  g_assert_false(((PtBindingLine *)g_ptr_array_index(b, 0))->is_unbind);
  pt_bindings_free(b);
}

static void test_unbind_of_unbound_key(void) {
  /* Nothing bound that accelerator yet: the entry still travels so apply can
   * drop the default row, and parse neither warns fatally nor drops it. */
  const char *lines[] = { "unbind ctrl+q", NULL };
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  const PtBindingLine *e = g_ptr_array_index(b, 0);
  g_assert_true(e->is_unbind);
  g_assert_null(e->action);
  pt_bindings_free(b);
}

static void test_incomplete_lines(void) {
  /* Empty values, missing actions and stray extra fields are all skipped. */
  const char *bad[] = {
    "bind",
    "unbind",
    "bind ctrl+b",
    "bind ctrl+b toggle-sidebar extra",
    "unbind ctrl+b toggle-sidebar",
    "",
    "zoom-pane = something",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    const char *one[] = { bad[i], "bind ctrl+b toggle-sidebar", NULL };
    GPtrArray *b = pt_bindings_parse(one, NULL, NULL);
    g_assert_cmpuint(b->len, ==, 1);
    pt_bindings_free(b);
  }
}

static void test_source_line_numbers(void) {
  /* The caller's real line numbers travel onto every surviving entry. */
  static const int nos[] = { 12, 40 };
  const char *lines[] = { "bind ctrl+b toggle-sidebar", "unbind alt+1", NULL };
  GPtrArray *b = pt_bindings_parse(lines, nos, NULL);
  g_assert_cmpint(((PtBindingLine *)g_ptr_array_index(b, 0))->line_no, ==, 12);
  g_assert_cmpint(((PtBindingLine *)g_ptr_array_index(b, 1))->line_no, ==, 40);
  pt_bindings_free(b);

  /* Without numbers the entries count from one. */
  b = pt_bindings_parse(lines, NULL, NULL);
  g_assert_cmpint(((PtBindingLine *)g_ptr_array_index(b, 0))->line_no, ==, 1);
  g_assert_cmpint(((PtBindingLine *)g_ptr_array_index(b, 1))->line_no, ==, 2);
  pt_bindings_free(b);
}

static void test_null_and_empty_input(void) {
  GPtrArray *b = pt_bindings_parse(NULL, NULL, NULL);
  g_assert_nonnull(b);
  g_assert_cmpuint(b->len, ==, 0);
  pt_bindings_free(b);

  const char *none[] = { NULL };
  b = pt_bindings_parse(none, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 0);
  pt_bindings_free(b);
}

/* ---------- warning capture ----------
 * A few behaviours are a warning plus an outcome rather than an outcome
 * alone; these route warnings into a buffer so the tests can read them. */
static GString *log_buf = NULL;
static guint log_id = 0;
static char *log_msg = NULL;

static void log_capture(const gchar *domain, GLogLevelFlags level,
                        const gchar *msg, gpointer user) {
  (void)domain; (void)level; (void)user;
  g_string_append_printf(log_buf, "%s\n", msg);
}

static void log_begin(void) {
  g_free(log_msg);
  log_msg = NULL;
  log_buf = g_string_new(NULL);
  log_id = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING, log_capture, NULL);
}

static void log_end(void) {
  g_log_remove_handler(NULL, log_id);
  log_msg = g_string_free(log_buf, FALSE);
  log_buf = NULL;
}

/* How many times needle appears in the captured warnings. */
static guint log_count(const char *needle) {
  guint n = 0;
  const char *p = log_msg;
  if (p == NULL) return 0;
  size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) { n++; p += len; }
  return n;
}

static void test_no_modifier_refused(void) {
  /* A modifier-less accelerator would ride the capture-phase controller and
   * swallow the bare key everywhere, so parse refuses it and says which
   * config line did it. */
  const char *bad[] = { "bind t new-tab", "bind enter paste", "bind f5 copy" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    log_begin();
    const char *one[] = { bad[i], NULL };
    GPtrArray *b = pt_bindings_parse(one, NULL, NULL);
    log_end();
    g_assert_cmpuint(b->len, ==, 0);
    pt_bindings_free(b);
    g_assert_nonnull(strstr(log_msg, "no modifier"));
    g_assert_nonnull(strstr(log_msg, "line 1"));
  }

  /* With a modifier the same key is fine. */
  const char *ok[] = { "bind ctrl+t new-tab", NULL };
  GPtrArray *b = pt_bindings_parse(ok, NULL, NULL);
  g_assert_cmpuint(b->len, ==, 1);
  pt_bindings_free(b);
}

static void test_ctrl_letter_warns_but_applies(void) {
  /* Deliberate choice: ctrl+<letter> still applies even though programs in
   * the pane also see it (SIGINT on ctrl+c). Parse warns once and binds. */
  const char *lines[] = { "bind ctrl+c copy", "bind ctrl+shift+c copy",
                          "bind alt+c paste", NULL };
  log_begin();
  GPtrArray *b = pt_bindings_parse(lines, NULL, NULL);
  log_end();
  g_assert_cmpuint(b->len, ==, 3);
  g_assert_cmpint(find_accel(b, "<Control>c"), >=, 0);
  /* Exactly one warning: only plain ctrl+letter carries the risk. */
  g_assert_cmpuint(log_count("ctrl+c is also seen"), ==, 1);
  g_assert_null(strstr(log_msg, "ctrl+s"));   /* shift variant not warned */
  g_assert_null(strstr(log_msg, "alt+c"));
  pt_bindings_free(b);
}

int main(void) {
  test_valid_lines();
  test_action_lookup();
  test_modifiers();
  test_named_keys();
  test_case_insensitive();
  test_unknown_action();
  test_bad_accel();
  test_duplicate_accel();
  test_unbind_of_unbound_key();
  test_incomplete_lines();
  test_source_line_numbers();
  test_null_and_empty_input();
  test_no_modifier_refused();
  test_ctrl_letter_warns_but_applies();
  g_print("test-bindings: OK\n");
  return 0;
}
