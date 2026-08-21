#include "pt-search.h"
#include <string.h>

void pt_search_rows_clear(PtSearchRows *g) {
  if (g == NULL) return;
  if (g->rows != NULL)
    for (int i = 0; i < g->n_rows; i++) g_free(g->rows[i]);
  g_clear_pointer(&g->rows, g_free);
  if (g->maps != NULL)
    for (int i = 0; i < g->n_rows; i++)
      if (g->maps[i] != NULL) g_array_unref(g->maps[i]);
  g_clear_pointer(&g->maps, g_free);
  g->n_rows = 0;
}

/* The byte position one past a match, then the cell columns it covers: the
 * map is consulted at both ends of the matched byte range and the columns
 * come out in the cells' own units, wide characters included. */
static void row_matches(const char *text, const GArray *map,
                        const char *needle, gsize needle_len,
                        int row, GArray *out) {
  const char *p = text;
  while ((p = strstr(p, needle)) != NULL) {
    gsize at = (gsize)(p - text);
    gsize end_byte = at + needle_len - 1;   /* a match spans >= 1 byte */
    /* map->len == strlen(text) is a hard invariant of whoever built these
     * rows: pt_term_core_search_rows appends to text and map together, one
     * map entry per folded byte, and trims trailing blanks off both at once.
     * So both ends of a match have a column behind them, and this asserts it
     * instead of clamping. A silent fall back to column 0 would paint the
     * highlight at the left margin, which reads as a drawing bug and buries
     * the actual one — an extraction that fell out of lockstep. */
    g_assert(end_byte < map->len);
    guint16 first = g_array_index(map, guint16, at);
    guint16 last = g_array_index(map, guint16, end_byte);
    PtSearchMatch m = { .row = row,
                        .start_col = first,
                        .end_col = last + 1 };
    p += needle_len;          /* non-overlapping, left to right */
    /* Case folding can make one cell contribute several bytes — "ß" folds to
     * "ss" — and every one of them maps back to that single column. A needle
     * short enough to sit inside such a cell therefore hits once per byte and
     * comes out on the identical rect each time: "s" in "straße" is found at
     * three byte offsets covering two cells. Painting the same rect twice is
     * invisible, but counting it twice is not — it inflates the bar's N/M and
     * makes Enter step twice through one cell before moving on. Byte order is
     * ascending within a row, so a duplicate is always the match just
     * appended and one look back is enough. */
    if (out->len > 0) {
      const PtSearchMatch *prev =
          &g_array_index(out, PtSearchMatch, out->len - 1);
      if (prev->row == m.row && prev->start_col == m.start_col &&
          prev->end_col == m.end_col)
        continue;
    }
    g_array_append_val(out, m);
  }
}

GArray *pt_search_find(const char *const *rows, const GArray *const *col_maps,
                       int n_rows, const char *needle) {
  GArray *out = g_array_new(FALSE, FALSE, sizeof(PtSearchMatch));
  if (needle == NULL || rows == NULL || col_maps == NULL) return out;

  /* The needle folds whole — it has no column map to stay aligned with —
   * while the rows arrive already folded per cell (see PtSearchRows). An
   * input that folds to nothing ("İ" alone can) matches nothing rather
   * than every byte boundary. */
  gchar *folded = g_utf8_casefold(needle, -1);
  gsize needle_len = strlen(folded);
  if (needle_len == 0) {
    g_free(folded);
    return out;
  }

  for (int i = 0; i < n_rows; i++) {
    if (rows[i] == NULL || col_maps[i] == NULL) continue;
    row_matches(rows[i], col_maps[i], folded, needle_len, i, out);
  }
  g_free(folded);
  return out;
}

int pt_search_step(GArray *matches, int current, int direction,
                   int n_rows, int viewport_top_row) {
  if (matches == NULL || matches->len == 0) return -1;
  int n = (int)matches->len;

  if (direction >= 0) {
    if (current >= 0 && current < n)
      return (current + 1) % n;
    /* No current match: enter at the first hit from the viewport down. */
    int top = CLAMP(viewport_top_row, 0, n_rows > 0 ? n_rows - 1 : 0);
    for (int i = 0; i < n; i++)
      if (g_array_index(matches, PtSearchMatch, i).row >= top) return i;
    return 0;
  }

  if (current >= 0 && current < n)
    return (current - 1 + n) % n;
  int top = CLAMP(viewport_top_row, 0, n_rows > 0 ? n_rows - 1 : 0);
  for (int i = n - 1; i >= 0; i--)
    if (g_array_index(matches, PtSearchMatch, i).row < top) return i;
  return n - 1;
}
