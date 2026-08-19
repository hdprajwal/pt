#include "pt-keymap.h"
#include <gdk/gdkkeysyms.h>
#include <gdk/gdk.h>

static void test_letters(void) {
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_a), ==, GHOSTTY_KEY_A);
  /* The keyval table matches exactly, as ghostty's does, so the shifted keysym
   * is not a key. Shift+z resolves through the keycode instead. */
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Z), ==, GHOSTTY_KEY_UNIDENTIFIED);
}

static void test_digits_and_specials(void) {
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_0), ==, GHOSTTY_KEY_DIGIT_0);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Return), ==, GHOSTTY_KEY_ENTER);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_BackSpace), ==, GHOSTTY_KEY_BACKSPACE);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Escape), ==, GHOSTTY_KEY_ESCAPE);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Up), ==, GHOSTTY_KEY_ARROW_UP);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_F5), ==, GHOSTTY_KEY_F5);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_VoidSymbol), ==, GHOSTTY_KEY_UNIDENTIFIED);

  /* Both of these read as mistakes and are not. The keypad enter is its own
   * key in the kitty protocol and has to stay distinct from the enter above
   * it, and the shifted tab keysym says shift was held rather than which key
   * was struck, so ghostty's table leaves it out and the keycode answers for
   * it. Ghostty has both this way (apprt/gtk/key.zig:392-534). */
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_KP_Enter), ==, GHOSTTY_KEY_NUMPAD_ENTER);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_ISO_Left_Tab), ==, GHOSTTY_KEY_UNIDENTIFIED);
}

static void test_mods(void) {
  g_assert_cmpint(pt_keymap_mods(GDK_CONTROL_MASK | GDK_SHIFT_MASK), ==,
                  GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT);
  g_assert_cmpint(pt_keymap_mods(GDK_ALT_MASK), ==, GHOSTTY_MODS_ALT);
  g_assert_cmpint(pt_keymap_mods(GDK_SUPER_MASK), ==, GHOSTTY_MODS_SUPER);
  /* GDK has one lock mask and will not say which lock set it; ghostty reads it
   * as caps and pt follows (apprt/gtk/key.zig:84-93). */
  g_assert_cmpint(pt_keymap_mods(GDK_LOCK_MASK), ==, GHOSTTY_MODS_CAPS_LOCK);
}

/* GTK reports the modifier state from before the key was struck, so the press
 * of a modifier arrives with its own bit clear and its release arrives with it
 * still set. Both have to be corrected, and the side has to be filled in from
 * the physical key, because nothing else knows it. */
typedef struct {
  const char *name;
  GhosttyKey key;
  GhosttyKeyAction action;
  GhosttyMods in;
  GhosttyMods expected;
} ModsForKeyCase;

static const ModsForKeyCase mods_for_key_cases[] = {
    {"press left shift", GHOSTTY_KEY_SHIFT_LEFT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_SHIFT},
    {"press right shift", GHOSTTY_KEY_SHIFT_RIGHT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_SHIFT_SIDE},
    {"press left control", GHOSTTY_KEY_CONTROL_LEFT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_CTRL},
    {"press right control", GHOSTTY_KEY_CONTROL_RIGHT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_CTRL | GHOSTTY_MODS_CTRL_SIDE},
    {"press left alt", GHOSTTY_KEY_ALT_LEFT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_ALT},
    {"press right alt", GHOSTTY_KEY_ALT_RIGHT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_ALT | GHOSTTY_MODS_ALT_SIDE},
    {"press left meta", GHOSTTY_KEY_META_LEFT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_SUPER},
    {"press right meta", GHOSTTY_KEY_META_RIGHT, GHOSTTY_KEY_ACTION_PRESS, 0,
     GHOSTTY_MODS_SUPER | GHOSTTY_MODS_SUPER_SIDE},

    /* The release of the last modifier held: GTK still reports it, and the bit
     * has to go, or an app in report-all-keys mode is told the key went up
     * while being handed mods saying it is still down. The side bit is written
     * on a release all the same, as it is in ghostty, and means nothing once
     * the modifier bit beside it is clear. */
    {"release left shift", GHOSTTY_KEY_SHIFT_LEFT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_SHIFT, 0},
    {"release right shift", GHOSTTY_KEY_SHIFT_RIGHT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_SHIFT, GHOSTTY_MODS_SHIFT_SIDE},
    {"release left control", GHOSTTY_KEY_CONTROL_LEFT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_CTRL, 0},
    {"release right control", GHOSTTY_KEY_CONTROL_RIGHT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_CTRL, GHOSTTY_MODS_CTRL_SIDE},
    {"release left alt", GHOSTTY_KEY_ALT_LEFT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_ALT, 0},
    {"release right alt", GHOSTTY_KEY_ALT_RIGHT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_ALT, GHOSTTY_MODS_ALT_SIDE},
    {"release left meta", GHOSTTY_KEY_META_LEFT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_SUPER, 0},
    {"release right meta", GHOSTTY_KEY_META_RIGHT, GHOSTTY_KEY_ACTION_RELEASE,
     GHOSTTY_MODS_SUPER, GHOSTTY_MODS_SUPER_SIDE},

    /* Repeat counts as held, and the other modifiers in the state are left
     * exactly as they came in. */
    {"repeat left control", GHOSTTY_KEY_CONTROL_LEFT, GHOSTTY_KEY_ACTION_REPEAT,
     GHOSTTY_MODS_ALT, GHOSTTY_MODS_ALT | GHOSTTY_MODS_CTRL},
    /* Ctrl+A: the modifier is already in the state and the key is not a
     * modifier, so there is nothing to fix and nothing to guess a side from. */
    {"ctrl and a letter", GHOSTTY_KEY_A, GHOSTTY_KEY_ACTION_PRESS,
     GHOSTTY_MODS_CTRL, GHOSTTY_MODS_CTRL},
};

static void test_mods_for_key(void) {
  for (gsize i = 0; i < G_N_ELEMENTS(mods_for_key_cases); i++) {
    const ModsForKeyCase *c = &mods_for_key_cases[i];
    g_test_message("case: %s", c->name);
    g_assert_cmpint(pt_keymap_mods_for_key(c->in, c->key, c->action), ==,
                    c->expected);
  }
}

/* The keycodes below are xkb keycodes taken from the table in pt-keymap.c, not
 * from memory, and GDK reports xkb keycodes on both X11 and Wayland. A row
 * with GDK_KEY_VoidSymbol as its keyval is a key whose keysym we do not care
 * about, which pins the answer to the keycode table alone. */
typedef struct {
  const char *name;
  guint keycode;
  guint keyval;
  GhosttyKey expected;
} PhysicalKeyCase;

static const PhysicalKeyCase physical_key_cases[] = {
    {"a", 0x026, GDK_KEY_a, GHOSTTY_KEY_A},
    /* Shift changes the keysym and nothing else, so every shifted row here has
     * to come back with the same key its unshifted twin does. */
    {"shift+a", 0x026, GDK_KEY_A, GHOSTTY_KEY_A},
    {"shift+3", 0x00C, GDK_KEY_numbersign, GHOSTTY_KEY_DIGIT_3},
    {"shift+semicolon", 0x02F, GDK_KEY_colon, GHOSTTY_KEY_SEMICOLON},
    /* AZERTY puts q where a US board puts a. Both are writing system keys, so
     * neither may be remapped and the physical key wins. */
    {"azerty q", 0x026, GDK_KEY_q, GHOSTTY_KEY_A},
    /* caps:swapescape, which is the case the keyval table exists to serve.
     * Caps lock is not a writing system key, so the remap is honoured. */
    {"caps as escape", 0x042, GDK_KEY_Escape, GHOSTTY_KEY_ESCAPE},
    /* The other half of the rule: the key struck is a writing system key, so
     * the first test says keep the physical key, but escape is not one, so the
     * remap is honoured anyway. Someone who binds escape onto a letter key
     * wants escape. */
    {"letter as escape", 0x026, GDK_KEY_Escape, GHOSTTY_KEY_ESCAPE},
    /* No keycode at all, as with a synthetic event, falls back to the keyval. */
    {"no keycode", 0x000, GDK_KEY_Escape, GHOSTTY_KEY_ESCAPE},

    {"f13", 0x0BF, GDK_KEY_VoidSymbol, GHOSTTY_KEY_F13},
    {"numpad 5", 0x054, GDK_KEY_VoidSymbol, GHOSTTY_KEY_NUMPAD_5},
    {"print screen", 0x06B, GDK_KEY_VoidSymbol, GHOSTTY_KEY_PRINT_SCREEN},
    {"scroll lock", 0x04E, GDK_KEY_VoidSymbol, GHOSTTY_KEY_SCROLL_LOCK},
    {"pause", 0x07F, GDK_KEY_VoidSymbol, GHOSTTY_KEY_PAUSE},
    {"context menu", 0x087, GDK_KEY_VoidSymbol, GHOSTTY_KEY_CONTEXT_MENU},

    /* Without these eight, kitty's report-all-keys mode can never tell an app
     * that a modifier went down. */
    {"left shift", 0x032, GDK_KEY_Shift_L, GHOSTTY_KEY_SHIFT_LEFT},
    {"right shift", 0x03E, GDK_KEY_Shift_R, GHOSTTY_KEY_SHIFT_RIGHT},
    {"left control", 0x025, GDK_KEY_Control_L, GHOSTTY_KEY_CONTROL_LEFT},
    {"right control", 0x069, GDK_KEY_Control_R, GHOSTTY_KEY_CONTROL_RIGHT},
    {"left alt", 0x040, GDK_KEY_Alt_L, GHOSTTY_KEY_ALT_LEFT},
    {"right alt", 0x06C, GDK_KEY_Alt_R, GHOSTTY_KEY_ALT_RIGHT},
    {"left meta", 0x085, GDK_KEY_Super_L, GHOSTTY_KEY_META_LEFT},
    {"right meta", 0x086, GDK_KEY_Super_R, GHOSTTY_KEY_META_RIGHT},
};

static void test_physical_key(void) {
  for (gsize i = 0; i < G_N_ELEMENTS(physical_key_cases); i++) {
    const PhysicalKeyCase *c = &physical_key_cases[i];
    g_test_message("case: %s", c->name);
    g_assert_cmpint(pt_keymap_physical_key(c->keycode, c->keyval), ==, c->expected);
  }
}

static void test_unknown_keycode(void) {
  g_assert_cmpint(pt_keymap_from_keycode(0), ==, GHOSTTY_KEY_UNIDENTIFIED);
  g_assert_cmpint(pt_keymap_from_keycode(0xFFFF), ==, GHOSTTY_KEY_UNIDENTIFIED);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/keymap/letters", test_letters);
  g_test_add_func("/keymap/specials", test_digits_and_specials);
  g_test_add_func("/keymap/mods", test_mods);
  g_test_add_func("/keymap/mods-for-key", test_mods_for_key);
  g_test_add_func("/keymap/physical-key", test_physical_key);
  g_test_add_func("/keymap/unknown-keycode", test_unknown_keycode);
  return g_test_run();
}
