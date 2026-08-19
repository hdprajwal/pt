#include "pt-keymap.h"
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

/* A GhosttyKey names a *physical* key, not the character it produces: the key
 * left of "s" on a US board is GHOSTTY_KEY_A whether the layout prints "a",
 * "q" or "ф" on it. GDK gives us both halves of that, and they answer
 * different questions. The keycode is the hardware scancode and is the honest
 * source for the physical key. The keyval is what the layout, the shift state
 * and any XKB remapping turned that key into, so it is the only place a user's
 * `caps:swapescape` shows up at all.
 *
 * So we keep two tables, exactly as ghostty does. This one is the keycode
 * table, and it decides whenever the keyval maps to nothing or both candidates
 * are keys a layout is expected to move around; `should_be_remappable` below
 * is where that rule lives.
 *
 * Transcribed from ghostty ae52f97, src/input/keycodes.zig, xkb column. That
 * file lists every platform's scancode for each W3C code as
 * `.{ usb, evdev, xkb, win, mac, code }`, and picks the column by target OS at
 * :11-16; Linux takes index 2, the xkb column, because GDK keycodes are xkb
 * keycodes under both X11 and Wayland. Rows whose xkb code is zero, and rows
 * whose W3C code maps to no key in `code_to_key` (:44), are left out so an
 * unknown keycode can never land on a real key. No two remaining rows share an
 * xkb code, so a plain scan gives the same answer ghostty's does. If the
 * ghostty pin ever moves, regenerate this from `raw_entries` in that file
 * rather than editing rows by hand. */
typedef struct {
  guint keycode;
  GhosttyKey key;
} PtKeycodeEntry;

static const PtKeycodeEntry pt_keycode_table[] = {
  {0x009, GHOSTTY_KEY_ESCAPE},
  {0x00A, GHOSTTY_KEY_DIGIT_1},
  {0x00B, GHOSTTY_KEY_DIGIT_2},
  {0x00C, GHOSTTY_KEY_DIGIT_3},
  {0x00D, GHOSTTY_KEY_DIGIT_4},
  {0x00E, GHOSTTY_KEY_DIGIT_5},
  {0x00F, GHOSTTY_KEY_DIGIT_6},
  {0x010, GHOSTTY_KEY_DIGIT_7},
  {0x011, GHOSTTY_KEY_DIGIT_8},
  {0x012, GHOSTTY_KEY_DIGIT_9},
  {0x013, GHOSTTY_KEY_DIGIT_0},
  {0x014, GHOSTTY_KEY_MINUS},
  {0x015, GHOSTTY_KEY_EQUAL},
  {0x016, GHOSTTY_KEY_BACKSPACE},
  {0x017, GHOSTTY_KEY_TAB},
  {0x018, GHOSTTY_KEY_Q},
  {0x019, GHOSTTY_KEY_W},
  {0x01A, GHOSTTY_KEY_E},
  {0x01B, GHOSTTY_KEY_R},
  {0x01C, GHOSTTY_KEY_T},
  {0x01D, GHOSTTY_KEY_Y},
  {0x01E, GHOSTTY_KEY_U},
  {0x01F, GHOSTTY_KEY_I},
  {0x020, GHOSTTY_KEY_O},
  {0x021, GHOSTTY_KEY_P},
  {0x022, GHOSTTY_KEY_BRACKET_LEFT},
  {0x023, GHOSTTY_KEY_BRACKET_RIGHT},
  {0x024, GHOSTTY_KEY_ENTER},
  {0x025, GHOSTTY_KEY_CONTROL_LEFT},
  {0x026, GHOSTTY_KEY_A},
  {0x027, GHOSTTY_KEY_S},
  {0x028, GHOSTTY_KEY_D},
  {0x029, GHOSTTY_KEY_F},
  {0x02A, GHOSTTY_KEY_G},
  {0x02B, GHOSTTY_KEY_H},
  {0x02C, GHOSTTY_KEY_J},
  {0x02D, GHOSTTY_KEY_K},
  {0x02E, GHOSTTY_KEY_L},
  {0x02F, GHOSTTY_KEY_SEMICOLON},
  {0x030, GHOSTTY_KEY_QUOTE},
  {0x031, GHOSTTY_KEY_BACKQUOTE},
  {0x032, GHOSTTY_KEY_SHIFT_LEFT},
  {0x033, GHOSTTY_KEY_BACKSLASH},
  {0x034, GHOSTTY_KEY_Z},
  {0x035, GHOSTTY_KEY_X},
  {0x036, GHOSTTY_KEY_C},
  {0x037, GHOSTTY_KEY_V},
  {0x038, GHOSTTY_KEY_B},
  {0x039, GHOSTTY_KEY_N},
  {0x03A, GHOSTTY_KEY_M},
  {0x03B, GHOSTTY_KEY_COMMA},
  {0x03C, GHOSTTY_KEY_PERIOD},
  {0x03D, GHOSTTY_KEY_SLASH},
  {0x03E, GHOSTTY_KEY_SHIFT_RIGHT},
  {0x03F, GHOSTTY_KEY_NUMPAD_MULTIPLY},
  {0x040, GHOSTTY_KEY_ALT_LEFT},
  {0x041, GHOSTTY_KEY_SPACE},
  {0x042, GHOSTTY_KEY_CAPS_LOCK},
  {0x043, GHOSTTY_KEY_F1},
  {0x044, GHOSTTY_KEY_F2},
  {0x045, GHOSTTY_KEY_F3},
  {0x046, GHOSTTY_KEY_F4},
  {0x047, GHOSTTY_KEY_F5},
  {0x048, GHOSTTY_KEY_F6},
  {0x049, GHOSTTY_KEY_F7},
  {0x04A, GHOSTTY_KEY_F8},
  {0x04B, GHOSTTY_KEY_F9},
  {0x04C, GHOSTTY_KEY_F10},
  {0x04D, GHOSTTY_KEY_NUM_LOCK},
  {0x04E, GHOSTTY_KEY_SCROLL_LOCK},
  {0x04F, GHOSTTY_KEY_NUMPAD_7},
  {0x050, GHOSTTY_KEY_NUMPAD_8},
  {0x051, GHOSTTY_KEY_NUMPAD_9},
  {0x052, GHOSTTY_KEY_NUMPAD_SUBTRACT},
  {0x053, GHOSTTY_KEY_NUMPAD_4},
  {0x054, GHOSTTY_KEY_NUMPAD_5},
  {0x055, GHOSTTY_KEY_NUMPAD_6},
  {0x056, GHOSTTY_KEY_NUMPAD_ADD},
  {0x057, GHOSTTY_KEY_NUMPAD_1},
  {0x058, GHOSTTY_KEY_NUMPAD_2},
  {0x059, GHOSTTY_KEY_NUMPAD_3},
  {0x05A, GHOSTTY_KEY_NUMPAD_0},
  {0x05B, GHOSTTY_KEY_NUMPAD_DECIMAL},
  {0x05F, GHOSTTY_KEY_F11},
  {0x060, GHOSTTY_KEY_F12},
  {0x068, GHOSTTY_KEY_NUMPAD_ENTER},
  {0x069, GHOSTTY_KEY_CONTROL_RIGHT},
  {0x06A, GHOSTTY_KEY_NUMPAD_DIVIDE},
  {0x06B, GHOSTTY_KEY_PRINT_SCREEN},
  {0x06C, GHOSTTY_KEY_ALT_RIGHT},
  {0x06E, GHOSTTY_KEY_HOME},
  {0x06F, GHOSTTY_KEY_ARROW_UP},
  {0x070, GHOSTTY_KEY_PAGE_UP},
  {0x071, GHOSTTY_KEY_ARROW_LEFT},
  {0x072, GHOSTTY_KEY_ARROW_RIGHT},
  {0x073, GHOSTTY_KEY_END},
  {0x074, GHOSTTY_KEY_ARROW_DOWN},
  {0x075, GHOSTTY_KEY_PAGE_DOWN},
  {0x076, GHOSTTY_KEY_INSERT},
  {0x077, GHOSTTY_KEY_DELETE},
  {0x07D, GHOSTTY_KEY_NUMPAD_EQUAL},
  {0x07F, GHOSTTY_KEY_PAUSE},
  {0x085, GHOSTTY_KEY_META_LEFT},
  {0x086, GHOSTTY_KEY_META_RIGHT},
  {0x087, GHOSTTY_KEY_CONTEXT_MENU},
  {0x08D, GHOSTTY_KEY_COPY},
  {0x08F, GHOSTTY_KEY_PASTE},
  {0x091, GHOSTTY_KEY_CUT},
  {0x0BF, GHOSTTY_KEY_F13},
  {0x0C0, GHOSTTY_KEY_F14},
  {0x0C1, GHOSTTY_KEY_F15},
  {0x0C2, GHOSTTY_KEY_F16},
  {0x0C3, GHOSTTY_KEY_F17},
  {0x0C4, GHOSTTY_KEY_F18},
  {0x0C5, GHOSTTY_KEY_F19},
  {0x0C6, GHOSTTY_KEY_F20},
  {0x0C7, GHOSTTY_KEY_F21},
  {0x0C8, GHOSTTY_KEY_F22},
  {0x0C9, GHOSTTY_KEY_F23},
  {0x0CA, GHOSTTY_KEY_F24},
};

GhosttyKey pt_keymap_from_keycode(guint keycode) {
  for (gsize i = 0; i < G_N_ELEMENTS(pt_keycode_table); i++)
    if (pt_keycode_table[i].keycode == keycode) return pt_keycode_table[i].key;
  return GHOSTTY_KEY_UNIDENTIFIED;
}

/* The keyval table, ported from ghostty's GTK apprt (src/apprt/gtk/key.zig:392-534).
 * It matches keyvals exactly and on purpose: GDK_KEY_a is a key, GDK_KEY_A is
 * not, because the shifted keysym tells you about the shift state rather than
 * about which key was struck, and the keycode table already knows which key
 * that was. */
GhosttyKey pt_keymap_from_keyval(guint keyval) {
  switch (keyval) {
  case GDK_KEY_a:            return GHOSTTY_KEY_A;
  case GDK_KEY_b:            return GHOSTTY_KEY_B;
  case GDK_KEY_c:            return GHOSTTY_KEY_C;
  case GDK_KEY_d:            return GHOSTTY_KEY_D;
  case GDK_KEY_e:            return GHOSTTY_KEY_E;
  case GDK_KEY_f:            return GHOSTTY_KEY_F;
  case GDK_KEY_g:            return GHOSTTY_KEY_G;
  case GDK_KEY_h:            return GHOSTTY_KEY_H;
  case GDK_KEY_i:            return GHOSTTY_KEY_I;
  case GDK_KEY_j:            return GHOSTTY_KEY_J;
  case GDK_KEY_k:            return GHOSTTY_KEY_K;
  case GDK_KEY_l:            return GHOSTTY_KEY_L;
  case GDK_KEY_m:            return GHOSTTY_KEY_M;
  case GDK_KEY_n:            return GHOSTTY_KEY_N;
  case GDK_KEY_o:            return GHOSTTY_KEY_O;
  case GDK_KEY_p:            return GHOSTTY_KEY_P;
  case GDK_KEY_q:            return GHOSTTY_KEY_Q;
  case GDK_KEY_r:            return GHOSTTY_KEY_R;
  case GDK_KEY_s:            return GHOSTTY_KEY_S;
  case GDK_KEY_t:            return GHOSTTY_KEY_T;
  case GDK_KEY_u:            return GHOSTTY_KEY_U;
  case GDK_KEY_v:            return GHOSTTY_KEY_V;
  case GDK_KEY_w:            return GHOSTTY_KEY_W;
  case GDK_KEY_x:            return GHOSTTY_KEY_X;
  case GDK_KEY_y:            return GHOSTTY_KEY_Y;
  case GDK_KEY_z:            return GHOSTTY_KEY_Z;

  case GDK_KEY_0:            return GHOSTTY_KEY_DIGIT_0;
  case GDK_KEY_1:            return GHOSTTY_KEY_DIGIT_1;
  case GDK_KEY_2:            return GHOSTTY_KEY_DIGIT_2;
  case GDK_KEY_3:            return GHOSTTY_KEY_DIGIT_3;
  case GDK_KEY_4:            return GHOSTTY_KEY_DIGIT_4;
  case GDK_KEY_5:            return GHOSTTY_KEY_DIGIT_5;
  case GDK_KEY_6:            return GHOSTTY_KEY_DIGIT_6;
  case GDK_KEY_7:            return GHOSTTY_KEY_DIGIT_7;
  case GDK_KEY_8:            return GHOSTTY_KEY_DIGIT_8;
  case GDK_KEY_9:            return GHOSTTY_KEY_DIGIT_9;

  case GDK_KEY_semicolon:    return GHOSTTY_KEY_SEMICOLON;
  case GDK_KEY_space:        return GHOSTTY_KEY_SPACE;
  case GDK_KEY_apostrophe:   return GHOSTTY_KEY_QUOTE;
  case GDK_KEY_comma:        return GHOSTTY_KEY_COMMA;
  case GDK_KEY_grave:        return GHOSTTY_KEY_BACKQUOTE;
  case GDK_KEY_period:       return GHOSTTY_KEY_PERIOD;
  case GDK_KEY_slash:        return GHOSTTY_KEY_SLASH;
  case GDK_KEY_minus:        return GHOSTTY_KEY_MINUS;
  case GDK_KEY_equal:        return GHOSTTY_KEY_EQUAL;
  case GDK_KEY_bracketleft:  return GHOSTTY_KEY_BRACKET_LEFT;
  case GDK_KEY_bracketright: return GHOSTTY_KEY_BRACKET_RIGHT;
  case GDK_KEY_backslash:    return GHOSTTY_KEY_BACKSLASH;

  case GDK_KEY_Up:           return GHOSTTY_KEY_ARROW_UP;
  case GDK_KEY_Down:         return GHOSTTY_KEY_ARROW_DOWN;
  case GDK_KEY_Right:        return GHOSTTY_KEY_ARROW_RIGHT;
  case GDK_KEY_Left:         return GHOSTTY_KEY_ARROW_LEFT;
  case GDK_KEY_Home:         return GHOSTTY_KEY_HOME;
  case GDK_KEY_End:          return GHOSTTY_KEY_END;
  case GDK_KEY_Insert:       return GHOSTTY_KEY_INSERT;
  case GDK_KEY_Delete:       return GHOSTTY_KEY_DELETE;
  case GDK_KEY_Caps_Lock:    return GHOSTTY_KEY_CAPS_LOCK;
  case GDK_KEY_Scroll_Lock:  return GHOSTTY_KEY_SCROLL_LOCK;
  case GDK_KEY_Num_Lock:     return GHOSTTY_KEY_NUM_LOCK;
  case GDK_KEY_Page_Up:      return GHOSTTY_KEY_PAGE_UP;
  case GDK_KEY_Page_Down:    return GHOSTTY_KEY_PAGE_DOWN;
  case GDK_KEY_Escape:       return GHOSTTY_KEY_ESCAPE;
  case GDK_KEY_Return:       return GHOSTTY_KEY_ENTER;
  case GDK_KEY_Tab:          return GHOSTTY_KEY_TAB;
  case GDK_KEY_BackSpace:    return GHOSTTY_KEY_BACKSPACE;
  case GDK_KEY_Print:        return GHOSTTY_KEY_PRINT_SCREEN;
  case GDK_KEY_Pause:        return GHOSTTY_KEY_PAUSE;

  case GDK_KEY_F1:           return GHOSTTY_KEY_F1;
  case GDK_KEY_F2:           return GHOSTTY_KEY_F2;
  case GDK_KEY_F3:           return GHOSTTY_KEY_F3;
  case GDK_KEY_F4:           return GHOSTTY_KEY_F4;
  case GDK_KEY_F5:           return GHOSTTY_KEY_F5;
  case GDK_KEY_F6:           return GHOSTTY_KEY_F6;
  case GDK_KEY_F7:           return GHOSTTY_KEY_F7;
  case GDK_KEY_F8:           return GHOSTTY_KEY_F8;
  case GDK_KEY_F9:           return GHOSTTY_KEY_F9;
  case GDK_KEY_F10:          return GHOSTTY_KEY_F10;
  case GDK_KEY_F11:          return GHOSTTY_KEY_F11;
  case GDK_KEY_F12:          return GHOSTTY_KEY_F12;
  case GDK_KEY_F13:          return GHOSTTY_KEY_F13;
  case GDK_KEY_F14:          return GHOSTTY_KEY_F14;
  case GDK_KEY_F15:          return GHOSTTY_KEY_F15;
  case GDK_KEY_F16:          return GHOSTTY_KEY_F16;
  case GDK_KEY_F17:          return GHOSTTY_KEY_F17;
  case GDK_KEY_F18:          return GHOSTTY_KEY_F18;
  case GDK_KEY_F19:          return GHOSTTY_KEY_F19;
  case GDK_KEY_F20:          return GHOSTTY_KEY_F20;
  case GDK_KEY_F21:          return GHOSTTY_KEY_F21;
  case GDK_KEY_F22:          return GHOSTTY_KEY_F22;
  case GDK_KEY_F23:          return GHOSTTY_KEY_F23;
  case GDK_KEY_F24:          return GHOSTTY_KEY_F24;
  case GDK_KEY_F25:          return GHOSTTY_KEY_F25;

  case GDK_KEY_KP_0:         return GHOSTTY_KEY_NUMPAD_0;
  case GDK_KEY_KP_1:         return GHOSTTY_KEY_NUMPAD_1;
  case GDK_KEY_KP_2:         return GHOSTTY_KEY_NUMPAD_2;
  case GDK_KEY_KP_3:         return GHOSTTY_KEY_NUMPAD_3;
  case GDK_KEY_KP_4:         return GHOSTTY_KEY_NUMPAD_4;
  case GDK_KEY_KP_5:         return GHOSTTY_KEY_NUMPAD_5;
  case GDK_KEY_KP_6:         return GHOSTTY_KEY_NUMPAD_6;
  case GDK_KEY_KP_7:         return GHOSTTY_KEY_NUMPAD_7;
  case GDK_KEY_KP_8:         return GHOSTTY_KEY_NUMPAD_8;
  case GDK_KEY_KP_9:         return GHOSTTY_KEY_NUMPAD_9;
  case GDK_KEY_KP_Decimal:   return GHOSTTY_KEY_NUMPAD_DECIMAL;
  case GDK_KEY_KP_Divide:    return GHOSTTY_KEY_NUMPAD_DIVIDE;
  case GDK_KEY_KP_Multiply:  return GHOSTTY_KEY_NUMPAD_MULTIPLY;
  case GDK_KEY_KP_Subtract:  return GHOSTTY_KEY_NUMPAD_SUBTRACT;
  case GDK_KEY_KP_Add:       return GHOSTTY_KEY_NUMPAD_ADD;
  case GDK_KEY_KP_Enter:     return GHOSTTY_KEY_NUMPAD_ENTER;
  case GDK_KEY_KP_Equal:     return GHOSTTY_KEY_NUMPAD_EQUAL;

  case GDK_KEY_KP_Separator: return GHOSTTY_KEY_NUMPAD_SEPARATOR;
  case GDK_KEY_KP_Left:      return GHOSTTY_KEY_NUMPAD_LEFT;
  case GDK_KEY_KP_Right:     return GHOSTTY_KEY_NUMPAD_RIGHT;
  case GDK_KEY_KP_Up:        return GHOSTTY_KEY_NUMPAD_UP;
  case GDK_KEY_KP_Down:      return GHOSTTY_KEY_NUMPAD_DOWN;
  case GDK_KEY_KP_Page_Up:   return GHOSTTY_KEY_NUMPAD_PAGE_UP;
  case GDK_KEY_KP_Page_Down: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
  case GDK_KEY_KP_Home:      return GHOSTTY_KEY_NUMPAD_HOME;
  case GDK_KEY_KP_End:       return GHOSTTY_KEY_NUMPAD_END;
  case GDK_KEY_KP_Insert:    return GHOSTTY_KEY_NUMPAD_INSERT;
  case GDK_KEY_KP_Delete:    return GHOSTTY_KEY_NUMPAD_DELETE;
  case GDK_KEY_KP_Begin:     return GHOSTTY_KEY_NUMPAD_BEGIN;

  case GDK_KEY_Copy:         return GHOSTTY_KEY_COPY;
  case GDK_KEY_Cut:          return GHOSTTY_KEY_CUT;
  case GDK_KEY_Paste:        return GHOSTTY_KEY_PASTE;

  case GDK_KEY_Shift_L:      return GHOSTTY_KEY_SHIFT_LEFT;
  case GDK_KEY_Control_L:    return GHOSTTY_KEY_CONTROL_LEFT;
  case GDK_KEY_Alt_L:        return GHOSTTY_KEY_ALT_LEFT;
  case GDK_KEY_Super_L:      return GHOSTTY_KEY_META_LEFT;
  case GDK_KEY_Shift_R:      return GHOSTTY_KEY_SHIFT_RIGHT;
  case GDK_KEY_Control_R:    return GHOSTTY_KEY_CONTROL_RIGHT;
  case GDK_KEY_Alt_R:        return GHOSTTY_KEY_ALT_RIGHT;
  case GDK_KEY_Super_R:      return GHOSTTY_KEY_META_RIGHT;
  default:                   return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

/* Ghostty's Key.shouldBeRemappable (src/input/key.zig:457-513), and its
 * reasoning is worth repeating because the rule looks arbitrary otherwise.
 * XKB implements a remap by rewriting the keysym and throwing the original
 * away, so a remapped key and a foreign layout are indistinguishable from
 * here. Trusting the keysym would break Cyrillic and AZERTY; ignoring it would
 * break `caps:swapescape`. The compromise is that the "Writing System Keys" of
 * W3C section 3.1.1, which are the keys a layout is expected to move around,
 * keep their physical identity, and everything else may be remapped. */
static gboolean should_be_remappable(GhosttyKey key) {
  switch (key) {
  /* "Writing System Keys" § 3.1.1 */
  case GHOSTTY_KEY_BACKQUOTE:
  case GHOSTTY_KEY_BACKSLASH:
  case GHOSTTY_KEY_BRACKET_LEFT:
  case GHOSTTY_KEY_BRACKET_RIGHT:
  case GHOSTTY_KEY_COMMA:
  case GHOSTTY_KEY_DIGIT_0:
  case GHOSTTY_KEY_DIGIT_1:
  case GHOSTTY_KEY_DIGIT_2:
  case GHOSTTY_KEY_DIGIT_3:
  case GHOSTTY_KEY_DIGIT_4:
  case GHOSTTY_KEY_DIGIT_5:
  case GHOSTTY_KEY_DIGIT_6:
  case GHOSTTY_KEY_DIGIT_7:
  case GHOSTTY_KEY_DIGIT_8:
  case GHOSTTY_KEY_DIGIT_9:
  case GHOSTTY_KEY_EQUAL:
  case GHOSTTY_KEY_INTL_BACKSLASH:
  case GHOSTTY_KEY_INTL_RO:
  case GHOSTTY_KEY_INTL_YEN:
  case GHOSTTY_KEY_A:
  case GHOSTTY_KEY_B:
  case GHOSTTY_KEY_C:
  case GHOSTTY_KEY_D:
  case GHOSTTY_KEY_E:
  case GHOSTTY_KEY_F:
  case GHOSTTY_KEY_G:
  case GHOSTTY_KEY_H:
  case GHOSTTY_KEY_I:
  case GHOSTTY_KEY_J:
  case GHOSTTY_KEY_K:
  case GHOSTTY_KEY_L:
  case GHOSTTY_KEY_M:
  case GHOSTTY_KEY_N:
  case GHOSTTY_KEY_O:
  case GHOSTTY_KEY_P:
  case GHOSTTY_KEY_Q:
  case GHOSTTY_KEY_R:
  case GHOSTTY_KEY_S:
  case GHOSTTY_KEY_T:
  case GHOSTTY_KEY_U:
  case GHOSTTY_KEY_V:
  case GHOSTTY_KEY_W:
  case GHOSTTY_KEY_X:
  case GHOSTTY_KEY_Y:
  case GHOSTTY_KEY_Z:
  case GHOSTTY_KEY_MINUS:
  case GHOSTTY_KEY_PERIOD:
  case GHOSTTY_KEY_QUOTE:
  case GHOSTTY_KEY_SEMICOLON:
  case GHOSTTY_KEY_SLASH:
    return FALSE;
  default:
    return TRUE;
  }
}

/* Ghostty's physical_key block, src/apprt/gtk/class/surface.zig:1355-1370. */
GhosttyKey pt_keymap_physical_key(guint keycode, guint keyval) {
  GhosttyKey w3c_key = pt_keymap_from_keycode(keycode);
  GhosttyKey remapped = pt_keymap_from_keyval(keyval);
  if (remapped != GHOSTTY_KEY_UNIDENTIFIED &&
      (should_be_remappable(w3c_key) || should_be_remappable(remapped)))
    return remapped;
  return w3c_key;
}

/* Ghostty's keyvalUnicodeUnshifted (src/apprt/gtk/key.zig:96-139). The display
 * holds every keyval the keycode can produce, one per (group, level) pair;
 * the group is the layout the event was typed under and level 0 is the key
 * with no modifiers applied. */
guint32 pt_keymap_unshifted_codepoint(GdkDisplay *display, GdkEvent *event,
                                      guint keycode) {
  guint layout = gdk_key_event_get_layout(event);
  GdkKeymapKey *keys = NULL;
  guint *keyvals = NULL;
  int n_entries = 0;
  if (!gdk_display_map_keycode(display, keycode, &keys, &keyvals, &n_entries))
    return 0;

  guint32 cp = 0;
  for (int i = 0; i < n_entries; i++) {
    if ((guint)keys[i].group == layout && keys[i].level == 0) {
      cp = gdk_keyval_to_unicode(keyvals[i]);
      break;
    }
  }
  g_free(keys);
  g_free(keyvals);
  return cp;
}

/* Ghostty's translateMods (src/apprt/gtk/key.zig:84-93), which it uses for
 * mouse events as well as key events, so the caps lock bit belongs here rather
 * than in the key-only fixups below.
 *
 * Ghostty asks the window protocol for these first and only falls back to this
 * function (winproto.zig:55-63). That matters on X11 alone, where it peeks the
 * next XKB event to learn about a modifier that is being pressed right now
 * (winproto/x11.zig:124-160); the Wayland side returns null and takes this
 * path (winproto/wayland.zig:62-68). pt does not link Xlib and does not want
 * to, so it always takes the Wayland path, and pt_keymap_mods_for_key covers
 * the case the X11 peek exists to cover. */
GhosttyMods pt_keymap_mods(guint state) {
  GhosttyMods mods = 0;
  if (state & GDK_SHIFT_MASK)   mods |= GHOSTTY_MODS_SHIFT;
  if (state & GDK_CONTROL_MASK) mods |= GHOSTTY_MODS_CTRL;
  if (state & GDK_ALT_MASK)     mods |= GHOSTTY_MODS_ALT;
  if (state & GDK_SUPER_MASK)   mods |= GHOSTTY_MODS_SUPER;
  /* GDK reports one lock mask and does not say which lock it is. Ghostty
   * assumes caps, and num lock is read off the device instead. */
  if (state & GDK_LOCK_MASK)    mods |= GHOSTTY_MODS_CAPS_LOCK;
  return mods;
}

/* A side bit set means the right-hand key, clear means the left-hand one, so
 * both halves have to be written on every pass. */
static GhosttyMods sided_mod(GhosttyMods mods, GhosttyMods bit,
                             GhosttyMods side_bit, gboolean right,
                             gboolean down) {
  mods = down ? (GhosttyMods)(mods | bit) : (GhosttyMods)(mods & ~bit);
  return right ? (GhosttyMods)(mods | side_bit)
               : (GhosttyMods)(mods & ~side_bit);
}

/* The switch from ghostty's eventMods (src/apprt/gtk/key.zig:141-206), minus
 * the num lock line, which needs the event's device and so stays with the
 * caller that has one. */
GhosttyMods pt_keymap_mods_for_key(GhosttyMods mods, GhosttyKey key,
                                   GhosttyKeyAction action) {
  gboolean down = action != GHOSTTY_KEY_ACTION_RELEASE;
  switch (key) {
  case GHOSTTY_KEY_SHIFT_LEFT:
    return sided_mod(mods, GHOSTTY_MODS_SHIFT, GHOSTTY_MODS_SHIFT_SIDE, FALSE, down);
  case GHOSTTY_KEY_SHIFT_RIGHT:
    return sided_mod(mods, GHOSTTY_MODS_SHIFT, GHOSTTY_MODS_SHIFT_SIDE, TRUE, down);
  case GHOSTTY_KEY_CONTROL_LEFT:
    return sided_mod(mods, GHOSTTY_MODS_CTRL, GHOSTTY_MODS_CTRL_SIDE, FALSE, down);
  case GHOSTTY_KEY_CONTROL_RIGHT:
    return sided_mod(mods, GHOSTTY_MODS_CTRL, GHOSTTY_MODS_CTRL_SIDE, TRUE, down);
  case GHOSTTY_KEY_ALT_LEFT:
    return sided_mod(mods, GHOSTTY_MODS_ALT, GHOSTTY_MODS_ALT_SIDE, FALSE, down);
  case GHOSTTY_KEY_ALT_RIGHT:
    return sided_mod(mods, GHOSTTY_MODS_ALT, GHOSTTY_MODS_ALT_SIDE, TRUE, down);
  case GHOSTTY_KEY_META_LEFT:
    return sided_mod(mods, GHOSTTY_MODS_SUPER, GHOSTTY_MODS_SUPER_SIDE, FALSE, down);
  case GHOSTTY_KEY_META_RIGHT:
    return sided_mod(mods, GHOSTTY_MODS_SUPER, GHOSTTY_MODS_SUPER_SIDE, TRUE, down);
  default:
    return mods;
  }
}
