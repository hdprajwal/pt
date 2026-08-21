#pragma once
#include <glib.h>

/* ---- find in scrollback ----
 *
 * The pure half of scrollback search: what a match is, what a search reads,
 * and how the current match moves. Everything here is stateless and takes
 * what it needs as arguments, so tests drive it without a terminal; the
 * extraction that feeds pt_search_find lives in pt-term-core (grid access),
 * and the drawing and the bar live in pt-terminal / pt-search-bar.
 *
 * One coordinate rule holds end to end: a match names CELL columns, and the
 * row strings it was found in carry a parallel byte -> column map. Byte
 * offsets over a row's UTF-8 are not columns — one cell can hold several
 * bytes, and the spacer half of a wide character holds none — so nothing in
 * the chain ever converts between the two by arithmetic. */

/* A match inside one row of the scrollable area. `row` is a SCREEN row:
 * 0 is the oldest row libghostty still retains and higher rows are newer,
 * which is the coordinate ghostty_terminal_grid_ref answers in and the one
 * pt_term_core_scrollbar()'s `offset` is counted from. `start_col` is the
 * first matched cell, `end_col` the cell one past the last matched byte's
 * cell — a half-open range ready for a pixel rect of
 * (end_col - start_col) cells. */
typedef struct {
  int row;
  int start_col;
  int end_col;
} PtSearchMatch;

/* Every row of one extraction, in screen order (row 0 first). `rows[i]` is
 * the row's text as UTF-8, blanks as spaces and trailing blanks trimmed;
 * `maps[i]` holds one guint16 per BYTE of `rows[i]`, saying which cell
 * column that byte was drawn by — several entries can name the same column
 * (a multi-byte cluster) and a wide character's spacer tail has none.
 *
 * The texts are CASE-FOLDED, each cell folded on its own before its bytes
 * are appended, so folding cannot move a byte out from under its map entry
 * the way folding a whole row would (ß becomes "ss" and grows two bytes):
 * the map is built from the folded bytes themselves. A caller matches a
 * similarly case-folded needle against these strings with plain byte
 * comparison and still lands on the right cells. */
typedef struct {
  char **rows;
  GArray **maps;
  int n_rows;
} PtSearchRows;

/* Frees everything PtSearchRows owns and zeroes it. NULL-safe. */
void pt_search_rows_clear(PtSearchRows *g);

/* Find every occurrence of `needle` in the given prepared rows. `rows` and
 * `col_maps` are parallel arrays of `n_rows` entries in the shape
 * PtSearchRows documents above — case-folded text, per-byte column maps —
 * and `needle` is case-folded by this call, so the caller hands raw user
 * input. Rows are searched in order and a row's matches left to right,
 * non-overlapping; the result is sorted by row. Empty (or absent) needle,
 * or no hit anywhere, is an empty array rather than NULL. Caller frees with
 * g_array_unref. */
GArray *pt_search_find(const char *const *rows, const GArray *const *col_maps,
                       int n_rows, const char *needle);

/* Move the current match. Returns the index of the match to show, or -1
 * when `matches` holds nothing. With `current` already on a match,
 * `direction` +1 / -1 steps to the next / previous one, wrapping past the
 * ends. With no current match (-1) the walk starts near the viewport rather
 * than at an end: downward takes the first match at or below the viewport's
 * top row (`viewport_top_row`, a screen row — the same axis the matches
 * carry), upward the last one above it, and when the viewport sits beyond
 * every match the walk wraps the way a repeat search would: downward to
 * the first match overall, upward to the last. */
int pt_search_step(GArray *matches, int current, int direction,
                   int n_rows, int viewport_top_row);
