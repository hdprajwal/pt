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
