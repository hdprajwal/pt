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
  test_parse_out_of_range_font_size();
  test_copy_equal();
  test_rewrite_preserves();
  test_rewrite_roundtrip();
  test_load_save();
  g_print("test-config: OK\n");
  return 0;
}
