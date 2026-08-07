/* pt-agent.h — which coding agent, if any, is running in a pane. */
#pragma once
#include <glib.h>

/* The agents pt reads usage for. The order is the order they are looked for
 * when a pane somehow has two running under it, which only happens when one
 * agent shells out to another. */
typedef enum {
  PT_AGENT_NONE = 0,
  PT_AGENT_CLAUDE,
  PT_AGENT_CODEX,
} PtAgentKind;

/* A process name, matched exactly. Exact and not a prefix on purpose:
 * "claude-desktop" is a different program that happens to start the same way,
 * and reporting a pane as running Claude Code because the desktop app is open
 * would be worse than reporting nothing. NULL and "" answer NONE. */
PtAgentKind pt_agent_kind_from_name(const char *name);

/* The agent a /proc/<pid>/cmdline names — NUL-separated arguments, `len`
 * bytes.
 *
 * comm is the kernel's name for the executable, so Claude Code installed
 * through npm reads as "node": the interpreter, not the agent. The command
 * line is where the answer actually is. Only argv[0] and argv[1] are
 * considered — argv[0] for a wrapper that exec'd the agent directly, argv[1]
 * for the script a runtime was handed. Anything past those is the agent's own
 * arguments, and a `claude` sitting in one of those is a word someone typed,
 * not the program that is running. */
PtAgentKind pt_agent_kind_from_cmdline(const char *cmdline, gsize len);

/* "Claude Code" / "Codex"; "" for NONE. Borrowed. */
const char *pt_agent_label(PtAgentKind kind);

/* The agent running in a pane whose shell is `shell_pid`.
 *
 * `fg_name` is the pane's foreground command when the caller knows it — pt
 * polls that already, so a match there costs nothing and covers the usual
 * case of typing `claude` at the prompt. Only when it does not match does
 * this walk the shell's descendants through /proc/<pid>/task/<tid>/children,
 * bounded in both depth and total processes visited.
 *
 * `out_pid` (may be NULL) gets the agent's pid, or 0 when the fast path
 * answered and no pid was looked up. */
PtAgentKind pt_agent_detect(int shell_pid, const char *fg_name, int *out_pid);
