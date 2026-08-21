/* pt-integration.h — `pt integration ...`: install the agent-side hooks
 * that make session reports appear. The merge is additive and idempotent:
 * the user's file keeps everything it had. */
#pragma once
#include <glib.h>

/* settings_text: current ~/.claude/settings.json contents, or NULL/"" for a
 * missing file. pt_bin: this binary, by path — the hooks are built from it.
 * Returns the new file text with one hook entry per event pt needs appended
 * under its own key (SessionStart for the resume registration; Stop and
 * Notification for lifecycle reports) — or NULL with err set on malformed
 * JSON or a hooks shape pt does not understand (never guess at a file we did
 * not understand). Events already carrying their command are left alone, and
 * when all of them are, the input text comes back unchanged. */
char *pt_integration_claude_merged_settings(const char *settings_text,
                                            const char *pt_bin,
                                            GError **err);
/* TRUE when every hook pt installs is present under its event key. */
gboolean pt_integration_claude_installed(const char *settings_text,
                                         const char *pt_bin);
/* TRUE when the named event ("SessionStart", "Stop", "Notification") carries
 * pt's command for it — the per-hook question `pt integration status` asks. */
gboolean pt_integration_claude_event_installed(const char *settings_text,
                                               const char *pt_bin,
                                               const char *event);
/* The `pt integration ...` entry point; argv as handed to main. Prints to
 * stdout/stderr, returns the process exit code. */
int pt_integration_cli(int argc, char *argv[]);
