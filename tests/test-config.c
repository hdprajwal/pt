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
  /* The attention dot, nothing louder: a bell you can see is the default,
   * a beep is one you have to ask for. */
  g_assert_cmpint(c->bell, ==, PT_BELL_VISUAL);
  /* Bytes of history per pane, ghostty's 10MB — a pane that keeps a session's
   * worth of output, not the ~10KB the old hardcoded number bought. */
  g_assert_cmpint(c->scrollback_limit, ==, 10000000);
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

static void test_parse_resume_agents(void) {
  /* On by default: saving an agent's session id is only worth anything if the
   * restored pane picks that session back up. */
  PtConfig *c = pt_config_parse("");
  g_assert_true(c->resume_agents);
  pt_config_free(c);

  c = pt_config_parse("resume-agents = false\n");
  g_assert_false(c->resume_agents);
  pt_config_free(c);

  /* Junk keeps the default, like every other key. */
  c = pt_config_parse("resume-agents = nonsense\n");
  g_assert_true(c->resume_agents);
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

static void test_parse_bell(void) {
  const struct { const char *text; PtBellMode want; } ok[] = {
    { "bell = visual\n",  PT_BELL_VISUAL },
    { "bell = audible\n", PT_BELL_AUDIBLE },
    { "bell = both\n",    PT_BELL_BOTH },
    { "bell = off\n",     PT_BELL_OFF },
    { "bell =  BOTH \n",  PT_BELL_BOTH },   /* trimmed and case-insensitive */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(ok); i++) {
    PtConfig *c = pt_config_parse(ok[i].text);
    g_assert_cmpint(c->bell, ==, ok[i].want);
    pt_config_free(c);
  }
  /* Junk keeps the default. `true` is junk here: this key is a mode, and a
   * typo must not be read as "turn something off" either. */
  const char *bad[] = { "bell = sometimes\n", "bell = true\n",
                        "bell = beep\n",      "bell = \n" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *c = pt_config_parse(bad[i]);
    g_assert_cmpint(c->bell, ==, PT_BELL_VISUAL);
    pt_config_free(c);
  }

  /* It takes part in copy/equal like the rest. */
  PtConfig *a = pt_config_parse("bell = both\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_cmpint(b->bell, ==, PT_BELL_BOTH);
  g_assert_true(pt_config_equal(a, b));
  b->bell = PT_BELL_OFF;
  g_assert_false(pt_config_equal(a, b));
  pt_config_free(a);
  pt_config_free(b);
}

static void test_rewrite_bell(void) {
  /* Absent from the old text: appended, and round-trips. */
  PtConfig *c = pt_config_new();
  c->bell = PT_BELL_AUDIBLE;
  char *out = pt_config_rewrite("theme = pt-dark\n", c);
  g_assert_nonnull(strstr(out, "bell = audible\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_cmpint(back->bell, ==, PT_BELL_AUDIBLE);
  g_assert_true(pt_config_equal(c, back));
  g_free(out);
  pt_config_free(back);

  /* An existing line is rewritten in place, comments around it kept. */
  c->bell = PT_BELL_OFF;
  out = pt_config_rewrite("bell = both\n# tail\n", c);
  g_assert_nonnull(strstr(out, "bell = off\n"));
  g_assert_null(strstr(out, "bell = both\n"));
  g_assert_nonnull(strstr(out, "# tail\n"));
  g_free(out);
  pt_config_free(c);
}

static void test_bell_halves(void) {
  /* The two questions everything downstream asks, pinned per mode so a new
   * mode has to answer them deliberately rather than fall out of a default. */
  const struct { PtBellMode m; gboolean visual, audio; } cases[] = {
    { PT_BELL_VISUAL,  TRUE,  FALSE },
    { PT_BELL_AUDIBLE, FALSE, TRUE  },
    { PT_BELL_BOTH,    TRUE,  TRUE  },
    { PT_BELL_OFF,     FALSE, FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_assert_cmpint(pt_bell_visual(cases[i].m), ==, cases[i].visual);
    g_assert_cmpint(pt_bell_audio(cases[i].m), ==, cases[i].audio);
  }
}

static void test_bell_attention(void) {
  /* The pane's bell_pending rule, pure part: a bell raises the tab's
   * attention flag only when the visual half is on and the pane is not the
   * one the user is reading. */
  const struct { PtBellMode m; gboolean focused; gboolean want; } cases[] = {
    { PT_BELL_VISUAL,  FALSE, TRUE  },
    { PT_BELL_VISUAL,  TRUE,  FALSE },
    { PT_BELL_AUDIBLE, FALSE, FALSE },   /* no dot under this setting at all */
    { PT_BELL_AUDIBLE, TRUE,  FALSE },
    { PT_BELL_BOTH,    FALSE, TRUE  },
    { PT_BELL_BOTH,    TRUE,  FALSE },
    { PT_BELL_OFF,     FALSE, FALSE },   /* off never gets this far anyway */
    { PT_BELL_OFF,     TRUE,  FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++)
    g_assert_cmpint(pt_bell_attention(cases[i].focused, cases[i].m), ==,
                    cases[i].want);
  /* The clear side of the lifecycle — pt_terminal_set_pane_bell dropping a
   * pending flag when the visual half goes away, and show_active_grid
   * answering a whole tab's panes at once — lives in the widgets and needs a
   * display, so it is not pinned here. pt_bell_visual is the predicate both
   * of them ask, and test_bell_halves already covers it. */
}

static void test_bell_audio_rate_limit(void) {
  gint64 last = 0;
  /* First ring after forever: heard. */
  gint64 t0 = 1000 * G_USEC_PER_SEC;
  g_assert_true(pt_bell_audio_take(&last, t0));
  g_assert_cmpint(last, ==, t0);
  /* Half a second later: suppressed — and crucially the stamp did not move,
   * or a program ringing twice a second would never be heard at all. */
  g_assert_false(pt_bell_audio_take(&last, t0 + G_USEC_PER_SEC / 2));
  g_assert_cmpint(last, ==, t0);
  /* One second on from the *heard* ring: allowed again. */
  g_assert_true(pt_bell_audio_take(&last, t0 + G_USEC_PER_SEC));
  g_assert_cmpint(last, ==, t0 + G_USEC_PER_SEC);
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

static void test_parse_term(void) {
  /* Absent: ghostty's name, which is what pt ships an entry for. */
  PtConfig *c = pt_config_parse("");
  g_assert_cmpstr(c->term, ==, PT_CONFIG_TERM_DEFAULT);
  g_assert_cmpstr(c->term, ==, "xterm-ghostty");
  pt_config_free(c);

  /* The whole point of the key: the way out for a user whose TERMINFO_DIRS
     does not survive to the program that needs it. */
  c = pt_config_parse("term = xterm-256color\n");
  g_assert_cmpstr(c->term, ==, "xterm-256color");
  pt_config_free(c);

  /* An empty value reads as "leave this alone", like every other string key
     here, rather than as a child with no $TERM at all. */
  c = pt_config_parse("term =\n");
  g_assert_cmpstr(c->term, ==, PT_CONFIG_TERM_DEFAULT);
  pt_config_free(c);

  /* Any name parses. Whether an entry exists behind it is answered at spawn,
     by the terminfo guard, not here. */
  c = pt_config_parse("term = pt-no-such-terminal\n");
  g_assert_cmpstr(c->term, ==, "pt-no-such-terminal");
  pt_config_free(c);

  /* It takes part in copy/equal and the rewrite like the rest. */
  PtConfig *a = pt_config_parse("term = xterm-256color\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_cmpstr(b->term, ==, "xterm-256color");
  g_assert_true(pt_config_equal(a, b));
  g_free(b->term);
  b->term = g_strdup("xterm-ghostty");
  g_assert_false(pt_config_equal(a, b));
  char *out = pt_config_rewrite("theme = pt-dark\n", a);
  g_assert_nonnull(strstr(out, "term = xterm-256color\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_true(pt_config_equal(a, back));
  g_free(out);
  pt_config_free(a);
  pt_config_free(b);
  pt_config_free(back);
}

static void test_parse_scrollback_limit(void) {
  /* Absent: ghostty's 10MB, in bytes. */
  PtConfig *c = pt_config_parse("");
  g_assert_cmpint(c->scrollback_limit, ==, PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT);
  pt_config_free(c);

  c = pt_config_parse("scrollback-limit = 123456\n");
  g_assert_cmpint(c->scrollback_limit, ==, 123456);
  pt_config_free(c);

  /* Zero is a real setting — a pane that keeps no history at all — so it has
   * to survive the parse rather than read as "unset". */
  c = pt_config_parse("scrollback-limit = 0\n");
  g_assert_cmpint(c->scrollback_limit, ==, 0);
  pt_config_free(c);

  /* Junk and out-of-range keep the default, like every other number here. */
  const char *bad[] = {
    "scrollback-limit = junk\n",
    "scrollback-limit = -5\n",
    "scrollback-limit = 10MB\n",
    "scrollback-limit = 99999999999999\n",   /* overflows int when narrowed */
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *b = pt_config_parse(bad[i]);
    g_assert_cmpint(b->scrollback_limit, ==,
                    PT_CONFIG_SCROLLBACK_LIMIT_DEFAULT);
    pt_config_free(b);
  }

  /* It takes part in copy/equal and the rewrite like the rest. */
  PtConfig *a = pt_config_parse("scrollback-limit = 2000000\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_cmpint(b->scrollback_limit, ==, 2000000);
  g_assert_true(pt_config_equal(a, b));
  b->scrollback_limit = 3000000;
  g_assert_false(pt_config_equal(a, b));
  char *out = pt_config_rewrite("theme = pt-dark\n", a);
  g_assert_nonnull(strstr(out, "scrollback-limit = 2000000\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_true(pt_config_equal(a, back));
  g_free(out);
  pt_config_free(a);
  pt_config_free(b);
  pt_config_free(back);
}

static void test_parse_window_padding(void) {
  /* Absent: the inset pt has always drawn. */
  PtConfig *c = pt_config_parse("");
  g_assert_cmpint(c->window_padding_x, ==, PT_CONFIG_WINDOW_PADDING_X_DEFAULT);
  g_assert_cmpint(c->window_padding_y, ==, PT_CONFIG_WINDOW_PADDING_Y_DEFAULT);
  pt_config_free(c);

  c = pt_config_parse("window-padding-x = 4\nwindow-padding-y = 6\n");
  g_assert_cmpint(c->window_padding_x, ==, 4);
  g_assert_cmpint(c->window_padding_y, ==, 6);
  pt_config_free(c);

  /* One axis set leaves the other on its own default: the two are separate
     keys, as in ghostty. */
  c = pt_config_parse("window-padding-x = 0\n");
  g_assert_cmpint(c->window_padding_x, ==, 0);
  g_assert_cmpint(c->window_padding_y, ==, PT_CONFIG_WINDOW_PADDING_Y_DEFAULT);
  pt_config_free(c);

  /* Junk and out-of-range keep the default. */
  const char *bad[] = {
    "window-padding-x = junk\n",
    "window-padding-x = -1\n",
    "window-padding-x = 201\n",
    "window-padding-x = 8px\n",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    PtConfig *b = pt_config_parse(bad[i]);
    g_assert_cmpint(b->window_padding_x, ==,
                    PT_CONFIG_WINDOW_PADDING_X_DEFAULT);
    pt_config_free(b);
  }

  /* Copy, equality and the rewrite carry both axes. */
  PtConfig *a = pt_config_parse("window-padding-x = 12\nwindow-padding-y = 3\n");
  PtConfig *b = pt_config_copy(a);
  g_assert_cmpint(b->window_padding_x, ==, 12);
  g_assert_cmpint(b->window_padding_y, ==, 3);
  g_assert_true(pt_config_equal(a, b));
  b->window_padding_y = 4;
  g_assert_false(pt_config_equal(a, b));
  char *out = pt_config_rewrite("theme = pt-dark\n", a);
  g_assert_nonnull(strstr(out, "window-padding-x = 12\n"));
  g_assert_nonnull(strstr(out, "window-padding-y = 3\n"));
  PtConfig *back = pt_config_parse(out);
  g_assert_true(pt_config_equal(a, back));
  g_free(out);
  pt_config_free(a);
  pt_config_free(b);
  pt_config_free(back);
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

static void test_binding_lines_collected(void) {
  /* Bind and unbind lines are gathered raw, in file order, wherever they sit
   * among the other keys; comments leave no residue. */
  const char *text =
      "# my config\n"
      "theme = gruvbox\n"
      "bind ctrl+shift+z pane-zoom\n"
      "\n"
      "unbind ctrl+b\n"
      "# bind ctrl+9 switch-project-9\n"
      "font-size = 11\n";
  PtConfig *c = pt_config_parse(text);
  g_assert_cmpuint(pt_config_n_binding_lines(c), ==, 2);
  g_assert_cmpstr(pt_config_binding_line(c, 0), ==,
                  "bind ctrl+shift+z pane-zoom");
  g_assert_cmpint(pt_config_binding_line_no(c, 0), ==, 3);
  g_assert_cmpstr(pt_config_binding_line(c, 1), ==, "unbind ctrl+b");
  g_assert_cmpint(pt_config_binding_line_no(c, 1), ==, 5);
  pt_config_free(c);

  PtConfig *plain = pt_config_parse("theme = pt-dark\nfont-size = 9\n");
  g_assert_cmpuint(pt_config_n_binding_lines(plain), ==, 0);
  pt_config_free(plain);
}

static void test_binding_lines_malformed(void) {
  /* No '=' at all: the generic malformed-line warning covers it and nothing
   * is collected. An empty value has nothing to bind, so it warns too. */
  const char *bad[] = { "bind\n", "unbind\n", "bind =\n", "unbind =\n" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    const char *one[] = { bad[i], "bind ctrl+b toggle-sidebar", NULL };
    GString *text = g_string_new(NULL);
    for (gsize j = 0; j < G_N_ELEMENTS(one) - 1; j++)
      g_string_append(text, one[j]);
    PtConfig *c = pt_config_parse(text->str);
    g_string_free(text, TRUE);
    g_assert_cmpuint(pt_config_n_binding_lines(c), ==, 1);
    g_assert_cmpstr(pt_config_binding_line(c, 0), ==,
                    "bind ctrl+b toggle-sidebar");
    pt_config_free(c);
  }
}

/* Counts warnings whose text contains `needle`, for the rules whose whole
 * product is a warning: the line is dropped either way, so only the message
 * separates "refused out loud" from "vanished". */
typedef struct { const char *needle; int hits; } WarnCatch;

static void warn_catch(const char *domain, GLogLevelFlags level,
                       const char *message, gpointer user) {
  (void)domain; (void)level;
  WarnCatch *w = user;
  if (strstr(message, w->needle) != NULL) w->hits++;
}

static void test_binding_lines_equals_in_accel(void) {
  /* An accelerator written with a literal '=' splits on it up in pt_kv_parse,
   * so the line can never be collected whole. Dropping it is not the point —
   * that already happened — being told why is: the grammar spells punctuation
   * by name, and `equal` is the name. Everything else in the file applies. */
  WarnCatch w = { .needle = "'=' in a bind line", .hits = 0 };
  guint id = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING, warn_catch, &w);
  PtConfig *c = pt_config_parse("bind ctrl+= font-zoom-in\n"
                                "theme = gruvbox\n"
                                "bind ctrl+equal font-zoom-in\n");
  g_log_remove_handler(NULL, id);
  g_assert_cmpint(w.hits, ==, 1);
  g_assert_cmpuint(pt_config_n_binding_lines(c), ==, 1);
  g_assert_cmpstr(pt_config_binding_line(c, 0), ==,
                  "bind ctrl+equal font-zoom-in");
  g_assert_cmpstr(c->theme, ==, "gruvbox");
  pt_config_free(c);

  /* An ordinary unknown key with an '=' is not a bind line and must stay
   * silent on this rule, or every `app-*` override would trip it. */
  w.hits = 0;
  id = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING, warn_catch, &w);
  PtConfig *q = pt_config_parse("app-background = #101010\nsome-key = 1\n");
  g_log_remove_handler(NULL, id);
  g_assert_cmpint(w.hits, ==, 0);
  pt_config_free(q);
}

static void test_binding_lines_tab_separated(void) {
  /* A bind line is words; whether a tab or a space parts them is not worth
   * refusing over. The verb's own separator is consumed either way, and what
   * follows travels verbatim for pt-bindings to split. */
  PtConfig *c = pt_config_parse("bind\tctrl+shift+t\tnew-tab\n"
                                "unbind \t alt+1\n"
                                "bind   ctrl+b   toggle-sidebar\n");
  g_assert_cmpuint(pt_config_n_binding_lines(c), ==, 3);
  g_assert_cmpstr(pt_config_binding_line(c, 0), ==,
                  "bind ctrl+shift+t\tnew-tab");
  g_assert_cmpstr(pt_config_binding_line(c, 1), ==, "unbind alt+1");
  g_assert_cmpstr(pt_config_binding_line(c, 2), ==,
                  "bind ctrl+b   toggle-sidebar");
  pt_config_free(c);
}

static void test_binding_lines_copy_equal_rewrite(void) {
  const char *old =
      "bind ctrl+b toggle-sidebar\n"
      "theme = pt-dark\n"
      "unbind alt+1\n";
  PtConfig *a = pt_config_parse(old);
  PtConfig *b = pt_config_copy(a);
  g_assert_true(pt_config_equal(a, b));
  g_assert_cmpuint(pt_config_n_binding_lines(b), ==, 2);
  g_assert_cmpstr(pt_config_binding_line(b, 1), ==, "unbind alt+1");
  /* A different line breaks equality. */
  g_ptr_array_remove_index(b->binding_lines, b->binding_lines->len - 1);
  g_assert_false(pt_config_equal(a, b));
  pt_config_free(a);
  pt_config_free(b);

  /* The rewrite is untouched by them: not managed keys, they pass through
   * verbatim like any comment or unknown line. */
  PtConfig *c = pt_config_parse(old);
  c->font_size = 12;
  char *out = pt_config_rewrite(old, c);
  g_assert_nonnull(strstr(out, "bind ctrl+b toggle-sidebar\n"));
  g_assert_nonnull(strstr(out, "unbind alt+1\n"));
  g_free(out);
  pt_config_free(c);
}

int main(void) {
  test_defaults();
  test_parse();
  test_parse_bad_values();
  test_parse_mouse_reporting();
  test_rewrite_mouse_reporting();
  test_parse_resume_agents();
  test_parse_osc52();
  test_rewrite_osc52();
  test_parse_bell();
  test_rewrite_bell();
  test_bell_halves();
  test_bell_attention();
  test_bell_audio_rate_limit();
  test_parse_term();
  test_parse_scrollback_limit();
  test_parse_window_padding();
  test_parse_out_of_range_font_size();
  test_parse_ui_font_size_not_a_number();
  test_copy_equal();
  test_rewrite_preserves();
  test_rewrite_roundtrip();
  test_load_save();
  test_binding_lines_collected();
  test_binding_lines_malformed();
  test_binding_lines_equals_in_accel();
  test_binding_lines_tab_separated();
  test_binding_lines_copy_equal_rewrite();
  g_print("test-config: OK\n");
  return 0;
}
