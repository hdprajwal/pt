#include "pt-style-css.h"

#include <locale.h>
#include <string.h>

/* The shipped theme resolved with no overrides, plus a default config: what
 * every pt window starts from, and the only input the CSS depends on. */
static char *css_for(PtConfig *cfg) {
  PtTheme *t = pt_theme_parse(pt_theme_builtin_pt_dark());
  PtResolvedTheme rt;
  pt_theme_resolve(t, NULL, &rt);
  char *css = pt_style_css(&rt, cfg);
  pt_theme_free(t);
  g_assert_nonnull(css);
  return css;
}

static char *default_css(void) {
  PtConfig *c = pt_config_new();
  char *css = css_for(c);
  pt_config_free(c);
  return css;
}

/* Every chrome token reaches the stylesheet under its own name. src/style.css
 * reads these with var(--pt-…), so a token that is not emitted is a rule that
 * silently does nothing. */
static void test_every_token_emitted(void) {
  char *css = default_css();
  g_assert_cmpint(PT_TOK_COUNT, ==, 33);
  for (int i = 0; i < PT_TOK_COUNT; i++) {
    char *decl = g_strdup_printf("\n  --pt-%s: ",
                                 pt_theme_token_name((PtTokenId)i));
    if (strstr(css, decl) == NULL)
      g_error("missing declaration for token %d (%s)", i,
              pt_theme_token_name((PtTokenId)i));
    g_free(decl);
  }
  g_assert_true(g_str_has_prefix(css, ":root {\n"));
  g_assert_true(g_str_has_suffix(css, "}\n"));
  g_free(css);
}

/* The derived values the tokens alone do not carry: per-accent glow and chip
 * washes, the ok glow, and the two font families. */
static void test_derived_values(void) {
  char *css = default_css();
  for (int a = 0; a < 6; a++) {
    char *glow = g_strdup_printf("  --pt-accent-%d-glow: rgba(", a);
    char *chip = g_strdup_printf("  --pt-accent-%d-chip: rgba(", a);
    g_assert_nonnull(strstr(css, glow));
    g_assert_nonnull(strstr(css, chip));
    g_free(glow);
    g_free(chip);
  }
  g_assert_nonnull(strstr(css, "  --pt-ok-glow: rgba("));
  g_assert_nonnull(strstr(css, ",0.13);"));   /* dot ring */
  g_assert_nonnull(strstr(css, ",0.10);"));   /* chip background */
  g_assert_nonnull(strstr(css, ",0.18);"));   /* ok glow */
  g_assert_nonnull(strstr(css,
      "  --pt-font-sans: \"IBM Plex Sans\", sans-serif;\n"));
  g_assert_nonnull(strstr(css,
      "  --pt-font-mono: \"JetBrains Mono\", monospace;\n"));
  g_free(css);
}

/* The nine size roles are ratios of ui-font-size, emitted at 0.1px. */
static void test_font_size_roles(void) {
  char *css = default_css();
  static const char *const at_12_5[] = {
    "  --pt-fs-lg: 13.0px;\n",   "  --pt-fs-base: 12.5px;\n",
    "  --pt-fs-md: 12.0px;\n",   "  --pt-fs-sm2: 11.5px;\n",
    "  --pt-fs-sm: 11.0px;\n",   "  --pt-fs-xs: 10.5px;\n",
    "  --pt-fs-2xs: 9.5px;\n",   "  --pt-fs-icon: 16.0px;\n",
    "  --pt-fs-plus: 14.0px;\n",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(at_12_5); i++)
    if (strstr(css, at_12_5[i]) == NULL)
      g_error("missing font size role: %s", at_12_5[i]);
  g_free(css);

  /* The roles scale with the setting, and every one of them moves. */
  PtConfig *c = pt_config_new();
  c->ui_font_size = 25.0;
  char *big = css_for(c);
  pt_config_free(c);
  g_assert_nonnull(strstr(big, "  --pt-fs-base: 25.0px;\n"));
  g_assert_nonnull(strstr(big, "  --pt-fs-lg: 26.0px;\n"));
  g_assert_nonnull(strstr(big, "  --pt-fs-icon: 32.0px;\n"));
  g_free(big);
}

/* printf's decimal separator follows LC_NUMERIC: under a comma locale a "%.1f"
 * would emit "12,5px" and make every font-size a CSS parse error. The numbers
 * are formatted with g_ascii_formatd, so they do not move with the locale. */
static void test_locale_independent_numbers(void) {
  /* Whichever comma-decimal locale this machine happens to have; a machine
   * with none skips the assert rather than failing for its locale set. */
  static const char *const comma[] = {
    "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "nl_NL.UTF-8",
  };
  char *saved = g_strdup(setlocale(LC_NUMERIC, NULL));
  gboolean set = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS(comma) && !set; i++)
    set = setlocale(LC_NUMERIC, comma[i]) != NULL;
  if (!set) {
    g_test_skip("no comma-decimal locale installed (de_DE.UTF-8 et al.)");
  } else {
    char *css = default_css();
    g_assert_nonnull(strstr(css, "  --pt-fs-base: 12.5px;\n"));
    g_assert_null(strstr(css, "12,5"));
    g_assert_null(strstr(css, ",5px"));
    g_free(css);
  }
  setlocale(LC_NUMERIC, saved != NULL ? saved : "C");
  g_free(saved);
}

/* pt_style_apply reloads the provider only when the text changed, comparing
 * with strcmp — so the same inputs have to give the same bytes, and different
 * ones different bytes. */
static void test_deterministic(void) {
  char *a = default_css();
  char *b = default_css();
  g_assert_cmpstr(a, ==, b);
  PtConfig *c = pt_config_new();
  g_free(c->ui_font_family);
  c->ui_font_family = g_strdup("Cantarell");
  char *other = css_for(c);
  pt_config_free(c);
  g_assert_cmpstr(a, !=, other);
  g_free(a);
  g_free(b);
  g_free(other);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/style/every-token", test_every_token_emitted);
  g_test_add_func("/style/derived", test_derived_values);
  g_test_add_func("/style/font-sizes", test_font_size_roles);
  g_test_add_func("/style/locale", test_locale_independent_numbers);
  g_test_add_func("/style/deterministic", test_deterministic);
  return g_test_run();
}
