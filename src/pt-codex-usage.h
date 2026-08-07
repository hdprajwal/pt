/* pt-codex-usage.h — Codex's usage, read out of its own session log.
 *
 * Codex writes a `token_count` event into the rollout log after every turn,
 * and each one carries the account's limits at that moment. So this reader is
 * a file read: no network, no credentials, nothing to authorize, and nothing
 * that can be rate limited. It is the cheapest of the three by a wide margin,
 * which is why it is the one that works with no setup. */
#pragma once
#include <glib.h>
#include "pt-usage.h"

/* $CODEX_HOME, or ~/.codex. Caller frees. */
char *pt_codex_home(void);

/* Fill `out` from a chunk of rollout jsonl — the tail of a log, or a whole
 * one. Reads the LAST `token_count` event in the chunk, since every turn
 * writes a fresh one and only the newest describes the account now. Lines that
 * are not JSON, or are cut in half by the tail read, are skipped.
 *
 * `now` is passed in rather than read so the mapping from a relative reset
 * ("resets in 4000 seconds", which older Codex wrote) to an absolute one is
 * testable. It does not set `fetched_at` — that is the caller's, and says when
 * the read happened, not when the log was written.
 *
 * `want_context` FALSE fills the limit windows and leaves the context bar
 * empty: the limits are account-wide so any recent log reports them, but the
 * context fill describes one session, and showing another pane's session
 * context here would be a lie.
 *
 * FALSE when the chunk holds no usable `token_count` at all. */
gboolean pt_codex_usage_parse(const char *jsonl, gint64 now,
                              gboolean want_context, PtUsage *out);

/* The `cwd` a rollout log's header line records, or NULL. Takes the front of
 * the file rather than a whole line — the header runs to some 22K of inlined
 * system prompt and the cwd is in the first few hundred bytes of it. Caller
 * frees. */
char *pt_codex_log_cwd(const char *header);

/* The real thing: find the newest session log under `codex_home` and fill
 * `out` from it. A log whose cwd is `cwd` is preferred, and only such a log
 * contributes the context bar; failing that the newest log of any directory
 * still reports the account-wide limits.
 *
 * Bounded on purpose — it walks only the newest few day directories, considers
 * a fixed number of files, and reads only the tail of each — because this runs
 * on a timer against a directory that grows without limit.
 *
 * FALSE when there is nothing to show, leaving `out` untouched. */
gboolean pt_codex_usage_read(const char *codex_home, const char *cwd,
                             gint64 now, PtUsage *out);
