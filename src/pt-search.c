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
    /* A match cannot be shorter than its needle, so the last byte is
     * always inside the map. */
    guint16 first = 0, last = 0;
    if (at < map->len) first = g_array_index(map, guint16, at);
    gsize end_byte = at + needle_len - 1;
    if (end_byte < map->len) last = g_array_index(map, guint16, end_byte);
    PtSearchMatch m = { .row = row,
                        .start_col = first,
                        .end_col = last + 1 };
    g_array_append_val(out, m);
    p += needle_len;          /* non-overlapping, left to right */
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
