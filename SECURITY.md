# Security

## Reporting a vulnerability

If you find a security problem in pt, please report it privately here:
[github.com/hdprajwal/pt/security/advisories/new](https://github.com/hdprajwal/pt/security/advisories/new).
Please do not open a public issue for anything exploitable.

I will reply within a few days. Once a fix is out, the advisory goes public,
with credit to you unless you would rather stay unnamed.

## What counts as a security bug

pt is a terminal emulator, so the untrusted input is whatever programs print
inside it. Reading a hostile file with `cat`, or ssh-ing into a hostile
server, should never be able to do more than draw on the screen. The areas I
care about most:

- escape and OSC sequence handling (titles, links, clipboard, prompt marks)
- what happens when you click a detected link
- the agent report files and the `pt agent-report` subcommand
- parsing of outside JSON (usage endpoints, session files)

If a bug only triggers from your own config file or command line flags, it is
a normal bug. File a regular issue for it.

## Supported versions

Fixes go to the latest release and `main` only.
