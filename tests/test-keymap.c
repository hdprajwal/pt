#include "pt-keymap.h"
#include <gdk/gdkkeysyms.h>
#include <gdk/gdk.h>

static void test_letters(void) {
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_a), ==, GHOSTTY_KEY_A);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Z), ==, GHOSTTY_KEY_Z);
  g_assert_cmpint(pt_keymap_unshifted_codepoint(GDK_KEY_A), ==, 'a');
}

static void test_digits_and_specials(void) {
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_0), ==, GHOSTTY_KEY_DIGIT_0);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Return), ==, GHOSTTY_KEY_ENTER);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_BackSpace), ==, GHOSTTY_KEY_BACKSPACE);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Escape), ==, GHOSTTY_KEY_ESCAPE);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_Up), ==, GHOSTTY_KEY_ARROW_UP);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_F5), ==, GHOSTTY_KEY_F5);
  g_assert_cmpint(pt_keymap_from_keyval(GDK_KEY_VoidSymbol), ==, GHOSTTY_KEY_UNIDENTIFIED);
}

static void test_mods(void) {
  g_assert_cmpint(pt_keymap_mods(GDK_CONTROL_MASK | GDK_SHIFT_MASK), ==,
                  GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT);
  g_assert_cmpint(pt_keymap_mods(GDK_ALT_MASK), ==, GHOSTTY_MODS_ALT);
  g_assert_cmpint(pt_keymap_mods(GDK_SUPER_MASK), ==, GHOSTTY_MODS_SUPER);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/keymap/letters", test_letters);
  g_test_add_func("/keymap/specials", test_digits_and_specials);
  g_test_add_func("/keymap/mods", test_mods);
  return g_test_run();
}
