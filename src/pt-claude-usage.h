/* pt-claude-usage.h — Claude Code's usage, fetched from Anthropic.
 *
 * Claude Code keeps no local record of its limit windows, so unlike Codex
 * there is nothing to read: the numbers only exist on the far end of an HTTP
 * call made with the token the CLI already stored. That is why this one is off
 * until the user turns it on — it puts their credential on the wire, which is
 * their call to make, not pt's.
 *
 * The endpoint is not a documented public API and its response has already
 * changed shape once, so the parser below accepts both shapes it is known to
 * have taken and goes quiet rather than wrong when it meets a third. */
#pragma once
#include <glib.h>
#include "pt-usage.h"

#define PT_CLAUDE_USAGE_URL "https://api.anthropic.com/api/oauth/usage"
/* The beta header the CLI sends with the same call. */
#define PT_CLAUDE_OAUTH_BETA "oauth-2025-04-20"

typedef struct {
  char *token;        /* the OAuth access token; NULL when there is none */
  gint64 expires_at;  /* unix seconds; 0 when the file did not say */
  char plan[24];      /* "max", "pro"; "" when the file did not say */
} PtClaudeCreds;

/* ~/.claude/.credentials.json. Caller frees. */
char *pt_claude_creds_path(void);

/* Read the stored login. FALSE when the file is missing, unreadable or holds
 * no token — all of which mean the same thing to the caller: the user has not
 * logged in to Claude Code on this machine.
 *
 * This never writes the file and never refreshes an expired token. Refreshing
 * rotates the token Claude Code itself depends on, so pt doing it behind the
 * CLI's back would log the user out of their own agent to draw a progress bar. */
gboolean pt_claude_creds_read(const char *path, PtClaudeCreds *out);

/* Frees the token, wiping it first: it lived in pt's heap only as long as one
 * request needed it. */
void pt_claude_creds_clear(PtClaudeCreds *c);

/* Fill `out` from a usage response body.
 *
 * Handles both known shapes — the older object with a member per window, and
 * the newer flat `limits` array whose weekly entries name the model they cap —
 * and returns FALSE when it recognises neither, which the caller shows as a
 * failed lookup rather than as an account with no limits. */
gboolean pt_claude_usage_parse(const char *json, gint64 now, PtUsage *out);

/* Split a `curl -i` transcript into its status, its Retry-After and its body,
 * and return a pointer into `text` at the body.
 *
 * Header blocks plural: a 100-continue or a redirect puts more than one in
 * front of the body, and only the last describes the response that arrived.
 * Done by hand rather than with curl's %header{} write-out variable, which is
 * recent enough that an older curl would hand back an empty string with no
 * error to notice.
 *
 * `retry_after` is in seconds and 0 when the header was absent or carried an
 * HTTP date instead — the caller falls back to its own backoff there, which is
 * the safe direction to be wrong in. */
const char *pt_claude_http_split(const char *text, int *status,
                                 gint64 *retry_after);
