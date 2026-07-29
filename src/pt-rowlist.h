/* pt-rowlist.h */
#pragma once
#include <gtk/gtk.h>

/* A box of rows rebuilt from a block of items, with the two things every one of
 * pt's row lists needs written once:
 *
 * - the dedupe. Callers refresh on every output change, which for a chatty pane
 *   means a couple of times a second while the underlying data sits still. A
 *   rebuild then would destroy a button between its press and its release — a
 *   widget destroyed in between never emits "clicked", so the click is silently
 *   swallowed — and would reset nothing useful. An unchanged set is a no-op.
 * - the row index. Every row carries the index of the item it came from, so a
 *   click reports the item and not the position on screen.
 *
 * The list owns the item block between rebuilds (that is what it compares the
 * next one against), so it needs a way to free it. */
#define PT_TYPE_ROWLIST (pt_rowlist_get_type())
G_DECLARE_FINAL_TYPE(PtRowList, pt_rowlist, PT, ROWLIST, GObject)

/* `host` holds the rows and nothing else: any chrome around them (a trailing +
 * button, a footer) belongs in a parent box, or a rebuild would destroy it. */
PtRowList *pt_rowlist_new(GtkBox *host);

/* Render `n_items` rows from `items`.
 *
 * `build_row(items, idx, u)` gets the whole block and an index — the block is
 * opaque here, so only the caller knows the element size. Returning NULL leaves
 * that item unrendered (a filter) without disturbing anyone else's index.
 *
 * `items_equal(a, na, b, nb, u)` compares the rendered block against the new
 * one; TRUE keeps the rows exactly as they are. NULL means "always rebuild".
 * Either side may be NULL when its count is 0.
 *
 * `items` is handed over on every call, the block already held included: a
 * refcounted one therefore wants a fresh reference taken before each call, and
 * the list gives its own back. `items_free` runs on the outgoing block when it
 * is replaced (after the rows built from it are gone, so a row may read its item
 * as it is destroyed), when the list is disposed, and immediately when
 * `items_equal` said the rows need no rebuild. `items_free` NULL borrows
 * instead — then the caller must clear the rows (a set with 0 items) before it
 * frees the block itself. */
void pt_rowlist_set(PtRowList *rl, gpointer items, guint n_items,
                    GtkWidget *(*build_row)(gpointer item, guint idx,
                                            gpointer u),
                    gboolean (*items_equal)(gpointer a, guint na, gpointer b,
                                            guint nb, gpointer u),
                    gpointer u, GDestroyNotify items_free);

/* The index of the item a row was built from. Rows built by this list only. */
int pt_rowlist_row_index(GtkWidget *row);

/* Move the "selected" class to the `sel`-th child of `row_host` (none when
 * negative). Highlighting this way instead of rebuilding is what lets arrow
 * keys run over a row a click gesture is sitting on. Takes the host box rather
 * than the list so hand-built rows can use it too. */
void pt_rowlist_mark_selected(GtkWidget *row_host, int sel);

/* Signals:
 * - "row-activated" (int index): a press landed on a row. Connect it and every
 *   row gets a click gesture; leave it alone and the rows stay inert (file rows
 *   are not clickable) or carry whatever controllers build_row put on them (a
 *   drag source, a hit-test that dodges a nested button). */
