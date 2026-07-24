#pragma once
#include <ghostty/vt.h>
#include <glib.h>
/* Map a GDK keyval to a GhosttyKey; GHOSTTY_KEY_UNIDENTIFIED if unmapped. */
GhosttyKey pt_keymap_from_keyval(guint keyval);
/* Unshifted codepoint for the kitty protocol ('a' for A, '1' for !, 0 if none). */
guint32 pt_keymap_unshifted_codepoint(guint keyval);
/* GDK modifier state → GhosttyMods. */
GhosttyMods pt_keymap_mods(guint state); /* pass GdkModifierType */
