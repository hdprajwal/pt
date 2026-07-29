#include "pt-config.h"
#include <string.h>

static void test_defaults(void) {
  PtConfig *c = pt_config_new();
  g_assert_cmpstr(c->theme, ==, "pt-dark");
  g_assert_cmpint(c->font_size, ==, 9);
  g_assert_cmpstr(c->font_family, ==, "JetBrains Mono");
  g_assert_cmpfloat(c->ui_font_size, ==, 12.5);
  g_assert_cmpstr(c->ui_font_family, ==, "IBM Plex Sans");
  g_assert_cmpuint(g_hash_table_size(c->app_overrides), ==, 0);
  /* Apps that ask for the mouse get it, as everywhere else; shift takes it
   * back for a gesture when you want pt's own selection. */
  g_assert_true(c->mouse_reporting);
  /* Clipboard writes from programs ship on: a yank on the far end of an ssh
   * session is meant to land on the local clipboard without setup. */
  g_assert_cmpint(c->osc52, ==, PT_OSC52_WRITE);
  pt_config_free(c);
}

static void test_parse_mouse_reporting(void) {
  const char *on[] = { "mouse-reporting = true\n", "mouse-reporting = yes\n",
                       "mouse-reporting = on\n",   "mouse-reporting = 1\n",
                       "mouse-reporting =  TRUE \n" };
  for (gsize i = 0; i < G_N_ELEMENTS(on); i++) {
    PtConfig *c = pt_config_parse(on[i]);
    g_assert_true(c->mouse_reporting);
    pt_config_free(c);
  }
  const char *off[] = { "mouse-reporting = false\n", "mouse-reporting = no\n",
                        "mouse-reporting = off\n",   "mouse-reporting = 0\n" };
  for (gsize i = 0; i < G_N_ELEMENTS(off); i++) {
    PtConfig *c = pt_config_parse(off[i]);
    g_assert_false(c->mouse_reporting);
    pt_config_free(c);
  }
  /* Junk keeps the default, like every other key. */
  PtConfig *bad = pt_config_parse("mouse-reporting = sometimes\n");
  g_assert_true(bad->mouse_reporting);
  pt_config_free(bad);

  /* It takes part in copy/equal like the rest. */
  PtConfig *a = pt_config_parse("mouse-reporting = true\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_true(b->mouse_reporting);
  g_assert_true(pt_config_equal(a, b));
  b->mouse_reporting = FALSE;
  g_assert_false(pt_config_equal(a, b));
  pt_config_free(a);
  pt_config_free(b);
}

static void test_rewrite_mouse_reporting(void) {
  /* Absent from the old text: appended, and round-trips back to the same
   * value in both directions. */
  PtConfig *c = pt_config_new();
  c->mouse_reporting = TRUE;
  char *out = pt_config_rewrite("theme = pt-dark\n", c);
  g_assert_nonnull(strstr(out, "mouse-reporting = true\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_true(back->mouse_reporting);
  g_assert_true(pt_config_equal(c, back));
  g_free(out);
  pt_config_free(back);

  c->mouse_reporting = FALSE;
  out = pt_config_rewrite("mouse-reporting = true\n# tail\n", c);
  g_assert_nonnull(strstr(out, "mouse-reporting = false\n"));
  g_assert_null(strstr(out, "mouse-reporting = true\n"));
  g_assert_nonnull(strstr(out, "# tail\n"));
  g_free(out);
  pt_config_free(c);
}

static void test_parse_osc52(void) {
  const struct { const char *text; PtOsc52Mode want; } ok[] = {
    { "osc52 = off\n",   PT_OSC52_OFF },
    { "osc52 = write\n", PT_OSC52_WRITE },
    { "osc52 = ask\n",   PT_OSC52_ASK },
    { "osc52 =  ASK \n", PT_OSC52_ASK },   /* trimmed and case-insensitive */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(ok); i++) {
    PtConfig *c = pt_config_parse(ok[i].text);
    g_assert_cmpint(c->osc52, ==, ok[i].want);
    pt_config_free(c);
  }
  /* Junk keeps the default. `true` is junk here: this key is a mode, and a
   * typo must not be read as "turn something off" either. */
  const char *bad[] = { "osc52 = sometimes\n", "osc52 = true\n",
                        "osc52 = \n" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *c = pt_config_parse(bad[i]);
    g_assert_cmpint(c->osc52, ==, PT_OSC52_WRITE);
    pt_config_free(c);
  }

  /* It takes part in copy/equal like the rest. */
  PtConfig *a = pt_config_parse("osc52 = ask\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_cmpint(b->osc52, ==, PT_OSC52_ASK);
  g_assert_true(pt_config_equal(a, b));
  b->osc52 = PT_OSC52_OFF;
  g_assert_false(pt_config_equal(a, b));
  pt_config_free(a);
  pt_config_free(b);
}

static void test_rewrite_osc52(void) {
  /* Absent from the old text: appended, and round-trips. */
  PtConfig *c = pt_config_new();
  c->osc52 = PT_OSC52_ASK;
  char *out = pt_config_rewrite("theme = pt-dark\n", c);
  g_assert_nonnull(strstr(out, "osc52 = ask\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_cmpint(back->osc52, ==, PT_OSC52_ASK);
  g_assert_true(pt_config_equal(c, back));
  g_free(out);
  pt_config_free(back);

  c->osc52 = PT_OSC52_OFF;
  out = pt_config_rewrite("osc52 = ask\n# tail\n", c);
  g_assert_nonnull(strstr(out, "osc52 = off\n"));
  g_assert_null(strstr(out, "osc52 = ask\n"));
  g_assert_nonnull(strstr(out, "# tail\n"));
  g_free(out);
  pt_config_free(c);
}

static void test_parse(void) {
  PtConfig *c = pt_config_parse(
      "# a comment\n"
      "theme = gruvbox\n"
      "  font-size =14  \n"
      "font-family = Fira Code\n"
      "ui-font-size = 13.5\n"
      "ui-font-family = Inter\n"
      "app-background = #101010\n"
      "app-border = rgba(255,255,255,0.10)\n"
      "not-a-known-key = whatever\n"
      "malformed line without equals\n"
      "\n");
  g_assert_cmpstr(c->theme, ==, "gruvbox");
  g_assert_cmpint(c->font_size, ==, 14);
  g_assert_cmpstr(c->font_family, ==, "Fira Code");
  g_assert_cmpfloat(c->ui_font_size, ==, 13.5);
  g_assert_cmpstr(c->ui_font_family, ==, "Inter");
  g_assert_cmpstr(g_hash_table_lookup(c->app_overrides, "background"),
                  ==, "#101010");
  g_assert_cmpstr(g_hash_table_lookup(c->app_overrides, "border"),
                  ==, "rgba(255,255,255,0.10)");
  pt_config_free(c);
}

static void test_parse_bad_values(void) {
  /* Junk numbers fall back to defaults; parser never crashes. */
  PtConfig *c = pt_config_parse("font-size = huge\nui-font-size = \n");
  g_assert_cmpint(c->font_size, ==, 9);
  g_assert_cmpfloat(c->ui_font_size, ==, 12.5);
  pt_config_free(c);
}

static void test_parse_out_of_range_font_size(void) {
  /* Out-of-range font sizes are rejected, not silently accepted or wrapped. */
  const char *bad[] = {
    "font-size = 0\n",
    "font-size = -5\n",
    "font-size = 99999999999999\n",   /* overflows int when narrowed */
    "font-size = 257\n",              /* just past the accepted range */
    "font-size = 999999999999999999999999\n", /* overflows long too */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *c = pt_config_parse(bad[i]);
    g_assert_cmpint(c->font_size, ==, 9);
    pt_config_free(c);
  }
  /* The edges of the accepted range still parse. */
  PtConfig *lo = pt_config_parse("font-size = 1\n");
  g_assert_cmpint(lo->font_size, ==, 1);
  pt_config_free(lo);
  PtConfig *hi = pt_config_parse("font-size = 256\n");
  g_assert_cmpint(hi->font_size, ==, 256);
  pt_config_free(hi);
}

/* ui-font-size lands in a CSS length, so "not a number" has to be caught here:
 * strtod spells nan and inf as numbers, and NaN compares false against any
 * range, which is how "nanpx" reaches the stylesheet. */
static void test_parse_ui_font_size_not_a_number(void) {
  const char *bad[] = {
    "ui-font-size = nan\n",  "ui-font-size = NaN\n",
    "ui-font-size = -nan\n", "ui-font-size = inf\n",
    "ui-font-size = -inf\n", "ui-font-size = infinity\n",
    "ui-font-size = 0\n",    "ui-font-size = -3.5\n",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *c = pt_config_parse(bad[i]);
    g_assert_cmpfloat(c->ui_font_size, ==, 12.5);
    pt_config_free(c);
  }
  /* Absurdly small is still positive and still finite: accepted, the same as
   * before there was a lower bound at all. Nobody can read it; that is the
   * UI's business, not the parser's. */
  PtConfig *tiny = pt_config_parse("ui-font-size = 1e-320\n");
  g_assert_cmpfloat(tiny->ui_font_size, ==, 1e-320);
  pt_config_free(tiny);
  /* And a large one round-trips through the rewrite unharmed. */
  PtConfig *big = pt_config_parse("ui-font-size = 1e300\n");
  g_assert_cmpfloat(big->ui_font_size, ==, 1e300);
  char *out = pt_config_rewrite("", big);
  PtConfig *back = pt_config_parse(out);
  g_assert_true(pt_config_equal(big, back));
  g_free(out);
  pt_config_free(big);
  pt_config_free(back);
}

static void test_copy_equal(void) {
  PtConfig *a = pt_config_parse("theme = x\napp-ok = #00ff00\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_true(pt_config_equal(a, b));
  g_free(b->theme);
  b->theme = g_strdup("y");
  g_assert_false(pt_config_equal(a, b));
  pt_config_free(a);
  pt_config_free(b);
}

static void test_rewrite_preserves(void) {
  const char *old =
      "# my config\n"
      "theme = pt-dark\n"
      "\n"
      "# fonts\n"
      "font-size = 11\n"
      "custom-future-key = kept\n";
  PtConfig *c = pt_config_parse(old);
  g_free(c->theme);
  c->theme = g_strdup("nord");
  c->font_size = 13;
  char *out = pt_config_rewrite(old, c);
  g_assert_nonnull(strstr(out, "# my config\n"));
  g_assert_nonnull(strstr(out, "# fonts\n"));
  g_assert_nonnull(strstr(out, "theme = nord\n"));
  g_assert_nonnull(strstr(out, "font-size = 13\n"));
  g_assert_nonnull(strstr(out, "custom-future-key = kept\n"));
  g_assert_null(strstr(out, "pt-dark"));
  /* keys absent from the old text are appended */
  g_assert_nonnull(strstr(out, "ui-font-size = 12.5\n"));
  g_free(out);
  pt_config_free(c);
}

static void test_rewrite_roundtrip(void) {
  PtConfig *c = pt_config_new();
  c->font_size = 9;
  char *out = pt_config_rewrite("", c);
  PtConfig *back = pt_config_parse(out);
  g_assert_true(pt_config_equal(c, back));
  g_free(out);
  pt_config_free(c);
  pt_config_free(back);
}

static void test_load_save(void) {
  char *dir = g_dir_make_tmp("pt-config-XXXXXX", NULL);
  char *path = g_build_filename(dir, "sub", "config", NULL);
  /* missing file -> defaults */
  PtConfig *c = pt_config_load(path);
  g_assert_cmpstr(c->theme, ==, "pt-dark");
  c->font_size = 15;
  GError *err = NULL;
  g_assert_true(pt_config_save(c, path, &err));  /* creates sub/ */
  g_assert_no_error(err);
  PtConfig *back = pt_config_load(path);
  g_assert_cmpint(back->font_size, ==, 15);
  pt_config_free(c);
  pt_config_free(back);
  g_free(path);
  g_free(dir);
}

int main(void) {
  test_defaults();
  test_parse();
  test_parse_bad_values();
  test_parse_mouse_reporting();
  test_rewrite_mouse_reporting();
  test_parse_osc52();
  test_rewrite_osc52();
  test_parse_out_of_range_font_size();
  test_parse_ui_font_size_not_a_number();
  test_copy_equal();
  test_rewrite_preserves();
  test_rewrite_roundtrip();
  test_load_save();
  g_print("test-config: OK\n");
  return 0;
}
