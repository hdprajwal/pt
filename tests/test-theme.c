#include "pt-theme.h"

static void test_color_parse(void) {
  PtColor c;
  g_assert_true(pt_color_parse("#6ee7a0", &c));
  g_assert_cmpint(c.r, ==, 0x6e);
  g_assert_cmpint(c.g, ==, 0xe7);
  g_assert_cmpint(c.b, ==, 0xa0);
  g_assert_cmpfloat(c.a, ==, 1.0);
  g_assert_true(pt_color_parse("rgba(255, 20,0, 0.5)", &c));
  g_assert_cmpint(c.r, ==, 255);
  g_assert_cmpint(c.g, ==, 20);
  g_assert_cmpfloat(c.a, ==, 0.5);
  g_assert_false(pt_color_parse("#12", &c));
  g_assert_false(pt_color_parse("blue", &c));
  char *css = pt_color_to_css(&(PtColor){0x0e, 0x10, 0x13, 1.0});
  g_assert_cmpstr(css, ==, "#0e1013");
  g_free(css);
}

static void test_pt_dark_identity(void) {
  /* The builtin theme resolved with no overrides must reproduce the shipped
   * design exactly — these hexes are the current style.css values. */
  PtTheme *t = pt_theme_parse(pt_theme_builtin_pt_dark());
  PtResolvedTheme rt;
  pt_theme_resolve(t, NULL, &rt);
  struct { PtTokenId id; const char *css; } expect[] = {
    { PT_TOK_BACKGROUND, "#0e1013" },
    { PT_TOK_SURFACE, "#12151a" },
    { PT_TOK_SURFACE_ALT, "#101317" },
    { PT_TOK_PANEL, "#15181d" },
    { PT_TOK_TERMINAL_CLEAR, "#0b0d10" },
    { PT_TOK_TEXT, "#e6e8ea" },
    { PT_TOK_TEXT_MUTED, "#8a9199" },
    { PT_TOK_TEXT_MID, "#727a85" },
    { PT_TOK_TEXT_FAINT, "#5c646f" },
    { PT_TOK_TEXT_DIM, "#4e5661" },
    { PT_TOK_TEXT_GHOST, "#3f4650" },
    { PT_TOK_OK, "#6ee7a0" },
    { PT_TOK_OK_MUTED, "#6b747f" },
    { PT_TOK_ERR, "#f2777a" },
    { PT_TOK_WARN, "#f2b25c" },
    { PT_TOK_FOCUS_RING, "#2f4f3a" },
    { PT_TOK_ACCENT_0, "#6ee7a0" },
    { PT_TOK_ACCENT_1, "#8ab4f8" },
    { PT_TOK_ACCENT_2, "#f2b25c" },
    { PT_TOK_ACCENT_3, "#c99bf0" },
    { PT_TOK_ACCENT_4, "#5ed3c4" },
    { PT_TOK_ACCENT_5, "#e0849b" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(expect); i++) {
    char *css = pt_color_to_css(&rt.tokens[expect[i].id]);
    if (g_strcmp0(css, expect[i].css) != 0)
      g_error("token %s: got %s want %s",
              pt_theme_token_name(expect[i].id), css, expect[i].css);
    g_free(css);
  }
  /* alpha tokens */
  g_assert_cmpfloat(rt.tokens[PT_TOK_BORDER].a, ==, 0.06);
  g_assert_cmpint(rt.tokens[PT_TOK_BORDER].r, ==, 255);
  g_assert_cmpfloat(rt.tokens[PT_TOK_SCRIM].a, ==, 0.62);
  /* terminal side */
  char *sel = pt_color_to_css(&rt.term.selection_bg);
  g_assert_cmpstr(sel, ==, "#264f38");
  g_free(sel);
  g_assert_true(rt.term.palette_set[2]);
  g_assert_cmpint(rt.term.palette[2].g, ==, 0xe7);
  pt_theme_free(t);
}

static void test_derivation_dark(void) {
  /* A dark theme with no app-* keys gets derived chrome: background lighter
   * than the terminal bg, text from fg, accents from ANSI slots. */
  PtTheme *t = pt_theme_parse(
      "background = #1d2021\n"
      "foreground = #ebdbb2\n"
      "selection-background = #504945\n"
      "palette = 1=#cc241d\n"
      "palette = 2=#98971a\n"
      "palette = 3=#d79921\n"
      "palette = 4=#458588\n"
      "palette = 5=#b16286\n"
      "palette = 6=#689d6a\n");
  PtResolvedTheme rt;
  pt_theme_resolve(t, NULL, &rt);
  /* chrome background strictly lighter than terminal bg (dark theme) */
  int bg_sum = rt.tokens[PT_TOK_BACKGROUND].r + rt.tokens[PT_TOK_BACKGROUND].g
             + rt.tokens[PT_TOK_BACKGROUND].b;
  g_assert_cmpint(bg_sum, >, 0x1d + 0x20 + 0x21);
  int surf_sum = rt.tokens[PT_TOK_SURFACE].r + rt.tokens[PT_TOK_SURFACE].g
               + rt.tokens[PT_TOK_SURFACE].b;
  g_assert_cmpint(surf_sum, >, bg_sum);
  /* text is exactly the fg; accents come from the palette */
  g_assert_cmpint(rt.tokens[PT_TOK_TEXT].r, ==, 0xeb);
  g_assert_cmpint(rt.tokens[PT_TOK_ACCENT_0].r, ==, 0x98); /* ANSI 2 */
  g_assert_cmpint(rt.tokens[PT_TOK_ACCENT_1].r, ==, 0x45); /* ANSI 4 */
  g_assert_cmpint(rt.tokens[PT_TOK_ERR].r, ==, 0xcc);      /* ANSI 1 */
  /* muted text sits between fg and bg */
  g_assert_cmpint(rt.tokens[PT_TOK_TEXT_MUTED].r, <, 0xeb);
  g_assert_cmpint(rt.tokens[PT_TOK_TEXT_MUTED].r, >, 0x1d);
  pt_theme_free(t);
}

static void test_derivation_light(void) {
  /* Light terminal bg: chrome derives DARKER, never clips to white. */
  PtTheme *t = pt_theme_parse(
      "background = #fbf1c7\nforeground = #3c3836\n");
  PtResolvedTheme rt;
  pt_theme_resolve(t, NULL, &rt);
  int bg_sum = rt.tokens[PT_TOK_BACKGROUND].r + rt.tokens[PT_TOK_BACKGROUND].g
             + rt.tokens[PT_TOK_BACKGROUND].b;
  g_assert_cmpint(bg_sum, <, 0xfb + 0xf1 + 0xc7);
  pt_theme_free(t);
}

static void test_precedence(void) {
  /* theme app-* beats derivation; config app-* beats the theme. */
  PtTheme *t = pt_theme_parse(
      "background = #000000\nforeground = #ffffff\n"
      "app-surface = #111111\napp-panel = #222222\n");
  GHashTable *cfg = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
  g_hash_table_insert(cfg, g_strdup("panel"), g_strdup("#333333"));
  PtResolvedTheme rt;
  pt_theme_resolve(t, cfg, &rt);
  char *surface = pt_color_to_css(&rt.tokens[PT_TOK_SURFACE]);
  char *panel = pt_color_to_css(&rt.tokens[PT_TOK_PANEL]);
  g_assert_cmpstr(surface, ==, "#111111");
  g_assert_cmpstr(panel, ==, "#333333");
  g_free(surface);
  g_free(panel);
  g_hash_table_unref(cfg);
  pt_theme_free(t);
}

static void test_missing_keys_fall_back(void) {
  /* An empty theme is exactly pt-dark. */
  PtTheme *empty = pt_theme_parse("");
  PtTheme *dark = pt_theme_parse(pt_theme_builtin_pt_dark());
  PtResolvedTheme a, b;
  pt_theme_resolve(empty, NULL, &a);
  pt_theme_resolve(dark, NULL, &b);
  for (int i = 0; i < PT_TOK_COUNT; i++) {
    char *ca = pt_color_to_css(&a.tokens[i]);
    char *cb = pt_color_to_css(&b.tokens[i]);
    g_assert_cmpstr(ca, ==, cb);
    g_free(ca);
    g_free(cb);
  }
  pt_theme_free(empty);
  pt_theme_free(dark);
}

int main(void) {
  test_color_parse();
  test_pt_dark_identity();
  test_derivation_dark();
  test_derivation_light();
  test_precedence();
  test_missing_keys_fall_back();
  g_print("test-theme: OK\n");
  return 0;
}
