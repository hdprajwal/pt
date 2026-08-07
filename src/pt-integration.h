/* pt-integration.h — `pt integration ...`: install the agent-side hooks
 * that make session reports appear. The merge is additive and idempotent:
 * the user's file keeps everything it had. */
#pragma once
#include <glib.h>

/* settings_text: current ~/.claude/settings.json contents, or NULL/"" for a
 * missing file. Returns the new file text with a SessionStart hook entry
 * running `command` appended under hooks.SessionStart — or NULL with err set
 * on malformed JSON (never guess at a file we did not understand). If the
 * command is already present, returns the input text unchanged. */
char *pt_integration_claude_merged_settings(const char *settings_text,
                                            const char *command,
                                            GError **err);
gboolean pt_integration_claude_installed(const char *settings_text,
                                         const char *command);
/* The `pt integration ...` entry point; argv as handed to main. Prints to
 * stdout/stderr, returns the process exit code. */
int pt_integration_cli(int argc, char *argv[]);
