#include "pt-theme.h"
#include <glib/gstdio.h>   /* g_remove / g_rmdir in the discovery tests */
#include <string.h>

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
  /* strtol() takes a sign and leading space; a hex color must not.
   * "#-00001" is the 7-char case that used to sign-extend into pure white. */
  g_assert_false(pt_color_parse("#-000001", &c));
  g_assert_false(pt_color_parse("#-00001", &c));
  g_assert_false(pt_color_parse("#+12345", &c));
  g_assert_false(pt_color_parse("# 12345", &c));
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
  g_assert_cmpfloat(rt.tokens[PT_TOK_BORDER].a, ==, 0.1);
  g_assert_cmpint(rt.tokens[PT_TOK_BORDER].r, ==, 255);
  g_assert_cmpfloat(rt.tokens[PT_TOK_FIELD_BG].a, ==, 0.04);
  g_assert_cmpint(rt.tokens[PT_TOK_FIELD_BG].r, ==, 255);
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

static void test_palette_index(void) {
  /* A malformed slot index must warn and pin nothing. atoi() would read "x"
   * and "" as 0 and silently repaint ANSI 0, and "1x" as slot 1. */
  PtTheme *t = pt_theme_parse(
      "palette = x=#ff0000\n"
      "palette = =#ff0000\n"
      "palette = 1x=#ff0000\n");
  g_assert_false(t->palette_set[0]);
  g_assert_false(t->palette_set[1]);
  /* a well-formed entry still lands */
  PtTheme *ok = pt_theme_parse("palette = 1=#ff0000\n");
  g_assert_true(ok->palette_set[1]);
  g_assert_cmpint(ok->palette[1].r, ==, 0xff);
  pt_theme_free(ok);
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
  /* Anything missing falls back to pt-dark's value for terminal keys, and to
   * derivation for chrome keys. An empty theme is pt-dark's terminal colors
   * with derived chrome — NOT pt-dark's pinned chrome. */
  PtTheme *t = pt_theme_parse("");
  PtResolvedTheme rt;
  pt_theme_resolve(t, NULL, &rt);
  /* terminal keys <- pt-dark */
  char *bg = pt_color_to_css(&rt.term.background);
  char *fg = pt_color_to_css(&rt.term.foreground);
  char *sel = pt_color_to_css(&rt.term.selection_bg);
  g_assert_cmpstr(bg, ==, "#0b0d10");
  g_assert_cmpstr(fg, ==, "#d6dae0");
  g_assert_cmpstr(sel, ==, "#264f38");
  g_free(bg);
  g_free(fg);
  g_free(sel);
  /* chrome keys <- derivation: text is the fallback foreground, not the
   * builtin's pinned app-text (#e6e8ea). */
  char *text = pt_color_to_css(&rt.tokens[PT_TOK_TEXT]);
  g_assert_cmpstr(text, ==, "#d6dae0");
  g_free(text);
  int bg_sum = rt.tokens[PT_TOK_BACKGROUND].r + rt.tokens[PT_TOK_BACKGROUND].g
             + rt.tokens[PT_TOK_BACKGROUND].b;
  g_assert_cmpint(bg_sum, >, 0x0b + 0x0d + 0x10);
  /* no palette slot 4, so accent-1 is the derivation default */
  char *accent1 = pt_color_to_css(&rt.tokens[PT_TOK_ACCENT_1]);
  g_assert_cmpstr(accent1, ==, "#8ab4f8");
  g_free(accent1);
  pt_theme_free(t);
}

static void test_discovery(void) {
  char *dir = g_dir_make_tmp("pt-themes-XXXXXX", NULL);
  char *f = g_build_filename(dir, "mytheme", NULL);
  g_file_set_contents(f, "background = #123456\n", -1, NULL);
  char **names = pt_theme_list_names(dir);
  gboolean saw_builtin = FALSE, saw_mine = FALSE;
  for (int i = 0; names[i] != NULL; i++) {
    if (g_strcmp0(names[i], "pt-dark") == 0) saw_builtin = TRUE;
    if (g_strcmp0(names[i], "mytheme") == 0) saw_mine = TRUE;
  }
  g_assert_true(saw_builtin);
  g_assert_true(saw_mine);
  g_strfreev(names);
  char *text = pt_theme_load_text(dir, "mytheme");
  g_assert_nonnull(strstr(text, "#123456"));
  g_free(text);
  text = pt_theme_load_text(dir, "pt-dark");
  g_assert_nonnull(strstr(text, "app-background"));
  g_free(text);
  g_assert_null(pt_theme_load_text(dir, "no-such-theme"));
  g_free(f);
  g_free(dir);
}

/* The light/dark classification programs are told about (CSI ? 996 n and mode
 * 2031 both report it). Driven off theme text rather than off whatever themes
 * happen to be installed. */
static void test_dark_classification(void) {
  struct { const char *bg; gboolean dark; } cases[] = {
    { "#0b0d10", TRUE },        /* pt-dark's background */
    { "#ffffff", FALSE },       /* a plain light theme */
    { "#f5f2ef", FALSE },       /* an off-white one */
    { "#1c1c1c", TRUE },
    /* Luminance, not the raw channels: saturated green is bright enough to
     * count as light (0.587 * 1.0), pure blue is not (0.114). */
    { "#00ff00", FALSE },
    { "#0000ff", TRUE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    char *text = g_strdup_printf("background = %s\n", cases[i].bg);
    PtTheme *t = pt_theme_parse(text);
    PtResolvedTheme rt;
    pt_theme_resolve(t, NULL, &rt);
    if (rt.dark != cases[i].dark)
      g_error("theme bg %s: dark=%d, expected %d", cases[i].bg, rt.dark,
              cases[i].dark);
    pt_theme_free(t);
    g_free(text);
  }
  /* An empty theme inherits pt-dark's background, so it is dark. */
  PtTheme *empty = pt_theme_parse("");
  PtResolvedTheme rt;
  pt_theme_resolve(empty, NULL, &rt);
  g_assert_true(rt.dark);
  pt_theme_free(empty);
}

static void test_is_dark_by_name(void) {
  /* What a caller classifying every installed theme uses: name in, answer out,
   * nothing applied. Same lookup order as pt_theme_load_text. */
  char *dir = g_dir_make_tmp("pt-themes-XXXXXX", NULL);
  char *lightf = g_build_filename(dir, "sunny", NULL);
  char *darkf = g_build_filename(dir, "midnight", NULL);
  g_file_set_contents(lightf, "background = #fafafa\nforeground = #202020\n",
                      -1, NULL);
  g_file_set_contents(darkf, "background = #101418\n", -1, NULL);

  g_assert_false(pt_theme_is_dark(dir, "sunny"));
  g_assert_true(pt_theme_is_dark(dir, "midnight"));
  g_assert_true(pt_theme_is_dark(dir, "pt-dark"));      /* the builtin */
  g_assert_true(pt_theme_is_dark(dir, "no-such-theme")); /* unknown -> dark */

  /* A file shadowing the builtin wins here too, exactly as it does on apply. */
  char *shadow = g_build_filename(dir, "pt-dark", NULL);
  g_file_set_contents(shadow, "background = #ffffff\n", -1, NULL);
  g_assert_false(pt_theme_is_dark(dir, "pt-dark"));

  /* Asked again with nothing changed — now served from the mtime-keyed
   * cache — the answers hold. */
  g_assert_false(pt_theme_is_dark(dir, "sunny"));
  g_assert_true(pt_theme_is_dark(dir, "midnight"));

  /* Rewriting a file re-classifies it: the cache must never serve a stale
   * answer for a changed file. The two bodies differ in length so the
   * fingerprint moves even on a filesystem with coarse mtimes. */
  g_file_set_contents(lightf, "background = #0d0f12\n", -1, NULL);
  g_assert_true(pt_theme_is_dark(dir, "sunny"));

  /* Deleting the shadow falls back to the builtin, not the cached file. */
  g_remove(shadow);
  g_assert_true(pt_theme_is_dark(dir, "pt-dark"));
  g_remove(lightf);
  g_remove(darkf);
  g_rmdir(dir);
  g_free(shadow);
  g_free(lightf);
  g_free(darkf);
  g_free(dir);
}

/* A classifier that reads no files: the filtering has to be right on its own,
 * whatever is (or is not) installed. Names ending in "-dark" are dark. */
static gboolean classify_by_suffix(const char *name, gpointer user) {
  int *calls = user;
  if (calls != NULL) (*calls)++;
  return g_str_has_suffix(name, "-dark");
}

/* What the dialog passes: the shipped classifier, bound to a theme dir. */
static gboolean classify_on_disk(const char *name, gpointer user) {
  return pt_theme_is_dark((const char *)user, name);
}

static void assert_names(char **got, const char *const *want) {
  guint n = 0;
  while (want[n] != NULL) n++;
  g_assert_cmpuint(g_strv_length(got), ==, n);
  for (guint i = 0; i < n; i++) g_assert_cmpstr(got[i], ==, want[i]);
}

/* What the settings dialog splits its theme list with. */
static void test_filter_appearance(void) {
  const char *const names[] = { "ayu", "one-dark", "github-light", "pt-dark",
                                "vercel", NULL };
  int calls = 0;
  char **dark = NULL, **light = NULL;
  pt_theme_filter_appearance(names, classify_by_suffix, &calls, &dark, &light);
  /* Input order survives, and the two subsets partition the list. */
  assert_names(dark, (const char *const[]){ "one-dark", "pt-dark", NULL });
  assert_names(light,
               (const char *const[]){ "ayu", "github-light", "vercel", NULL });
  /* Exactly one classify call per name for both sides together — the whole
   * point of partitioning in one walk. A second pass would read every theme
   * file twice per dialog open. */
  g_assert_cmpint(calls, ==, 5);
  g_strfreev(dark);
  g_strfreev(light);

  /* An appearance nothing matches is an empty vector, never NULL: the caller
   * tests it to know the appearance is not worth offering. */
  const char *const only_dark[] = { "pt-dark", NULL };
  pt_theme_filter_appearance(only_dark, classify_by_suffix, NULL,
                             &dark, &light);
  g_assert_nonnull(light);
  g_assert_null(light[0]);
  g_assert_cmpstr(dark[0], ==, "pt-dark");
  g_strfreev(dark);
  g_strfreev(light);

  /* Empty and NULL lists behave the same way. */
  const char *const empty[] = { NULL };
  char **empty_dark = NULL, **empty_light = NULL;
  char **null_dark = NULL, **null_light = NULL;
  pt_theme_filter_appearance(empty, classify_by_suffix, NULL,
                             &empty_dark, &empty_light);
  pt_theme_filter_appearance(NULL, classify_by_suffix, NULL,
                             &null_dark, &null_light);
  g_assert_null(empty_dark[0]);
  g_assert_null(empty_light[0]);
  g_assert_null(null_dark[0]);
  g_assert_null(null_light[0]);
  g_strfreev(empty_dark);
  g_strfreev(empty_light);
  g_strfreev(null_dark);
  g_strfreev(null_light);
}

/* The same split, driven by the real classifier over real theme files: a light
 * background lands in the light list and a dark one in the dark list. */
static void test_filter_appearance_on_disk(void) {
  char *dir = g_dir_make_tmp("pt-themes-XXXXXX", NULL);
  char *lightf = g_build_filename(dir, "sunny", NULL);
  char *darkf = g_build_filename(dir, "midnight", NULL);
  g_file_set_contents(lightf, "background = #f4f4f4\n", -1, NULL);
  g_file_set_contents(darkf, "background = #15141b\n", -1, NULL);

  char **names = pt_theme_list_names(dir);   /* midnight, pt-dark, sunny */
  char **dark = NULL, **light = NULL;
  pt_theme_filter_appearance((const char *const *)names, classify_on_disk, dir,
                             &dark, &light);
  assert_names(dark, (const char *const[]){ "midnight", "pt-dark", NULL });
  assert_names(light, (const char *const[]){ "sunny", NULL });

  g_strfreev(dark);
  g_strfreev(light);
  g_strfreev(names);
  g_remove(lightf);
  g_remove(darkf);
  g_rmdir(dir);
  g_free(lightf);
  g_free(darkf);
  g_free(dir);
}

int main(void) {
  test_color_parse();
  test_pt_dark_identity();
  test_derivation_dark();
  test_palette_index();
  test_derivation_light();
  test_precedence();
  test_missing_keys_fall_back();
  test_discovery();
  test_dark_classification();
  test_is_dark_by_name();
  test_filter_appearance();
  test_filter_appearance_on_disk();
  g_print("test-theme: OK\n");
  return 0;
}
