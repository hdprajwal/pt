#pragma once
#include <ghostty/vt.h>
#include <glib.h>
/* The physical key for a GDK key event, which is what the kitty keyboard
 * protocol reports. Prefer this over the two lookups below: a GhosttyKey is a
 * layout-independent physical code, and a keyval is neither, because the
 * layout and the shift state have already been applied to it by the time GDK
 * hands it over. Shift+3 arrives as `numbersign` and Q on AZERTY arrives as
 * `a`, and only the keycode still knows which key the finger landed on. */
GhosttyKey pt_keymap_physical_key(guint keycode, guint keyval);
/* Map a hardware keycode to a GhosttyKey; GHOSTTY_KEY_UNIDENTIFIED if unmapped. */
GhosttyKey pt_keymap_from_keycode(guint keycode);
/* Map a GDK keyval to a GhosttyKey; GHOSTTY_KEY_UNIDENTIFIED if unmapped. This
 * is how a user's XKB remapping, such as caps lock sending escape, reaches us
 * at all, so it is consulted for the keys where a remap is more likely than a
 * foreign layout. */
GhosttyKey pt_keymap_from_keyval(guint keyval);
/* Unshifted codepoint for the kitty protocol ('a' for A, '1' for !, 0 if none). */
guint32 pt_keymap_unshifted_codepoint(guint keyval);
/* GDK modifier state → GhosttyMods. */
GhosttyMods pt_keymap_mods(guint state); /* pass GdkModifierType */
