#include "pt-keymap.h"
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>

GhosttyKey pt_keymap_from_keyval(guint keyval) {
  guint lower = gdk_keyval_to_lower(keyval);
  if (lower >= GDK_KEY_a && lower <= GDK_KEY_z)
    return GHOSTTY_KEY_A + (lower - GDK_KEY_a);
  if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
    return GHOSTTY_KEY_DIGIT_0 + (keyval - GDK_KEY_0);
  if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
    return GHOSTTY_KEY_F1 + (keyval - GDK_KEY_F1);
  switch (keyval) {
  case GDK_KEY_space:        return GHOSTTY_KEY_SPACE;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:     return GHOSTTY_KEY_ENTER;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab: return GHOSTTY_KEY_TAB;
  case GDK_KEY_BackSpace:    return GHOSTTY_KEY_BACKSPACE;
  case GDK_KEY_Delete:       return GHOSTTY_KEY_DELETE;
  case GDK_KEY_Escape:       return GHOSTTY_KEY_ESCAPE;
  case GDK_KEY_Up:           return GHOSTTY_KEY_ARROW_UP;
  case GDK_KEY_Down:         return GHOSTTY_KEY_ARROW_DOWN;
  case GDK_KEY_Left:         return GHOSTTY_KEY_ARROW_LEFT;
  case GDK_KEY_Right:        return GHOSTTY_KEY_ARROW_RIGHT;
  case GDK_KEY_Home:         return GHOSTTY_KEY_HOME;
  case GDK_KEY_End:          return GHOSTTY_KEY_END;
  case GDK_KEY_Page_Up:      return GHOSTTY_KEY_PAGE_UP;
  case GDK_KEY_Page_Down:    return GHOSTTY_KEY_PAGE_DOWN;
  case GDK_KEY_Insert:       return GHOSTTY_KEY_INSERT;
  case GDK_KEY_minus:        return GHOSTTY_KEY_MINUS;
  case GDK_KEY_equal:        return GHOSTTY_KEY_EQUAL;
  case GDK_KEY_bracketleft:  return GHOSTTY_KEY_BRACKET_LEFT;
  case GDK_KEY_bracketright: return GHOSTTY_KEY_BRACKET_RIGHT;
  case GDK_KEY_backslash:    return GHOSTTY_KEY_BACKSLASH;
  case GDK_KEY_semicolon:    return GHOSTTY_KEY_SEMICOLON;
  case GDK_KEY_apostrophe:   return GHOSTTY_KEY_QUOTE;
  case GDK_KEY_comma:        return GHOSTTY_KEY_COMMA;
  case GDK_KEY_period:       return GHOSTTY_KEY_PERIOD;
  case GDK_KEY_slash:        return GHOSTTY_KEY_SLASH;
  case GDK_KEY_grave:        return GHOSTTY_KEY_BACKQUOTE;
  default:                   return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

guint32 pt_keymap_unshifted_codepoint(guint keyval) {
  guint lower = gdk_keyval_to_lower(keyval);
  guint32 cp = gdk_keyval_to_unicode(lower);
  return cp;
}

GhosttyMods pt_keymap_mods(guint state) {
  GhosttyMods mods = 0;
  if (state & GDK_SHIFT_MASK)   mods |= GHOSTTY_MODS_SHIFT;
  if (state & GDK_CONTROL_MASK) mods |= GHOSTTY_MODS_CTRL;
  if (state & GDK_ALT_MASK)     mods |= GHOSTTY_MODS_ALT;
  if (state & GDK_SUPER_MASK)   mods |= GHOSTTY_MODS_SUPER;
  return mods;
}
