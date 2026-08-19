#pragma once
#include <gdk/gdk.h>
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
/* The codepoint the key produces with no modifiers at all, which is what the
 * kitty protocol reports and what fixterms compares the shifted character
 * against: 'a' for A, '1' for !. Zero when the key produces no text.
 *
 * It has to come from the layout rather than from the keyval, because
 * lowercasing the keyval answers a different question: Shift+2 lowercases to
 * `@` where the protocol wants `2`. So the keycode is looked up in the
 * display's own table and the entry for the event's layout at level 0 wins.
 * That needs a live GdkDisplay and a real GdkKeyEvent, which a headless test
 * has neither of, so this one function is checked by the GUI pass and not by
 * ctest. Zero when the display knows no mapping for the keycode. */
guint32 pt_keymap_unshifted_codepoint(GdkDisplay *display, GdkEvent *event,
                                      guint keycode);
/* GDK modifier state → GhosttyMods, including caps lock: GDK reports one lock
 * mask without saying which lock set it, and ghostty reads that as caps. Num
 * lock is not in here, because it comes off the event's device rather than the
 * state, so the caller with a device adds it. */
GhosttyMods pt_keymap_mods(guint state); /* pass GdkModifierType */
/* The fixups a key event needs on top of the live modifier state. GTK reports
 * the modifiers as they were *before* the key was struck, so pressing shift
 * alone arrives with no shift bit and an app in kitty's report-all-keys mode
 * would be told a modifier was pressed while being handed mods that say it was
 * not. The sided bits have no other source either: only the physical key says
 * which shift it was. Caller passes the action so that a release clears the
 * bit the matching press set. */
GhosttyMods pt_keymap_mods_for_key(GhosttyMods mods, GhosttyKey key,
                                   GhosttyKeyAction action);
/* Is this one of the eight modifier keys? Ghostty asks the same question to
 * decide what does not count as typing: a bare shift must not clear a
 * selection, even in a mode where bytes are encoded for it. */
gboolean pt_keymap_is_modifier(GhosttyKey key);
