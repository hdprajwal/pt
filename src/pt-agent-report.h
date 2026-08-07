/* pt-agent-report.h — the `pt agent-report` subcommand.
 *
 * A subcommand rather than a second binary so pt ships one file, and the hooks
 * it installs point at the same pt the user already has. It runs inside the
 * agent, never inside pt's UI, so main() dispatches here before any GTK. */
#pragma once

/* argv is the process argv: argv[1] is "agent-report", argv[2] the mode. */
int pt_agent_report_cli(int argc, char *argv[]);
