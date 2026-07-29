#pragma once
#include <glib.h>

/* Case-insensitive subsequence match. Returns a score >= 1 when every
 * character of needle appears in order in haystack, 0 otherwise.
 * Higher is better: consecutive matches and matches at the start of
 * haystack or after a separator (/ - _ . space) score extra.
 * Empty needle matches everything with score 1.
 * Case folding is ASCII-only; non-ASCII bytes are compared verbatim, so
 * UTF-8 text still matches itself but not across case. NULL on either
 * side scores 0. */
int pt_fuzzy_score(const char *needle, const char *haystack);

/* Stable top-`max_out` selection over scores the caller worked out: writes the
 * indices of the best-scoring items into `out_idx`, score descending, and
 * returns how many it wrote (never more than max_out). `scores[i]` is item i's
 * score; items scoring 0 or less are left out entirely.
 * Equal scores keep input order — a tie loses to the earlier item, which is
 * what stops a list re-ranked with an unchanged query from reshuffling rows
 * under the user's cursor. Nothing is allocated and nothing outside the kept
 * range of `out_idx` is written.
 * Split from pt_fuzzy_rank because a caller's score per item need not come
 * from one string: the command palette takes the better of an item's name and
 * its detail. */
int pt_fuzzy_rank_scored(const int *scores, int n, int *out_idx, int max_out);

/* pt_fuzzy_rank_scored over pt_fuzzy_score(query, labels[i]): the best
 * max_out labels for `query`, in the same stable order. An empty query scores
 * every label 1, so it degenerates to "the first max_out labels, in order"; a
 * NULL label (or a NULL query) scores 0 and is left out. */
int pt_fuzzy_rank(const char *const *labels, int n, const char *query,
                  int *out_idx, int max_out);
