# pt

A terminal workspace for Linux, built with GTK4 + libadwaita on top of
libghostty-vt.

## Install from a release

Prebuilt tarballs for Linux x86_64 and aarch64 are published on GitHub
Releases. Install the latest one with:

```sh
curl -fsSL https://github.com/hdprajwal/pt/releases/latest/download/install.sh | sh
```

This installs `pt` to `~/.local/bin`, the prompt snippets to
`~/.local/share/pt/prompt`, and a desktop entry + app icon so pt shows up
in your application launcher (set `PT_PREFIX` to change the prefix; pass a
tag as an argument to pin a version). You still need the runtime libraries from
your package manager: GTK4 ≥ 4.16, libadwaita, and json-glib — plus the
fonts listed below. To build from source instead, read on.

## Requirements

- Linux with a C11 compiler, CMake ≥ 3.19, and pkg-config
- GTK4 ≥ 4.16 — older versions parse `src/style.css` into an unstyled app
  (the CSS relies on custom properties, `:root` / `var()`)
- libadwaita, json-glib, GLib/GIO (including `glib-compile-resources`,
  shipped with the GLib development package)
- Zig 0.15.x — used to build libghostty-vt. CMake looks for `zig` on `PATH`
  and falls back to `~/.local/opt/zig-0.15.2`
- git and network access on the first configure (the ghostty source is
  fetched at a pinned commit via CMake FetchContent)

On Arch-based systems:

```sh
sudo pacman -S --needed base-devel cmake pkgconf gtk4 libadwaita json-glib
```

Fonts: the defaults are JetBrains Mono (terminal) and IBM Plex Sans (UI
chrome). Neither is bundled — install them, or point the config at fonts you
have (see below).

## Build

```sh
cmake -B build
cmake --build build
```

The first configure clones and builds ghostty's `lib-vt`, so it takes a
while; later builds are incremental.

Run the app:

```sh
./build/pt
```

Or install it, including the desktop entry and icon for your launcher:

```sh
cmake --install build --prefix ~/.local
```

## Tests

```sh
ctest --test-dir build
```

## Shell prompt integration

Source the snippet for your shell from its rc file:

```sh
# ~/.zshrc
source /path/to/pt/share/prompt/pt-prompt.zsh
```

Bash and fish variants live next to it in `share/prompt/`. The snippet is a
no-op outside pt, so it is safe to source unconditionally. Inside pt it
reports each command's exit code back to the app and prints an identity line
(path, branch, dirty count) in the project's accent color.

If a repository with a huge untracked tree makes the prompt slow, set
`PT_PROMPT_GIT_UNTRACKED=no` to count only tracked changes.

## Agent resume

Close pt with `claude` or `codex` running in a pane and the next launch brings
the conversation back: the restored pane types `claude --resume <id>` for
itself. It is the same session, not a fresh one that happens to sit in the same
directory.

The agent has to tell pt which session it is in, so each one needs a small
integration installed once:

```sh
pt integration install claude   # adds a SessionStart hook to ~/.claude/settings.json
pt integration install codex    # prints a notify line for ~/.codex/config.toml
pt integration status           # says which of the two are in place
```

Claude Code's settings are JSON, so pt edits them in place — additively, and it
refuses rather than rewrite a file it did not parse. codex's `config.toml`
carries comments and ordering that no TOML round-trip preserves, so pt prints
the `notify = [...]` line and you paste it yourself. Both helpers are inert
outside a pt pane, and `install` a second time changes nothing.

codex only reports its session once a turn completes, so a resumed codex pane
you quit before finishing a turn comes back as a plain shell next time.

The resume command is typed into the restored pane's shell, so a shell rc that
reads from stdin while it starts up can swallow it — the pane then sits at a
prompt with nothing resumed.

Set `resume-agents = false` to keep the ids but always restore plain shells.

What is saved lives in `~/.local/state/pt/agent-sessions/`: one small JSON file
per pane holding the agent's session id and working directory, swept after a
week. That is a list of what you were working on and where — treat the
directory like shell history.

## Projects

The sidebar lists your projects. Click a row to switch to it, or press
<kbd>Ctrl</kbd>+<kbd>1</kbd> … <kbd>Ctrl</kbd>+<kbd>9</kbd> to pick one by
position. <kbd>Ctrl</kbd>+<kbd>N</kbd> adds a folder, the <kbd>×</kbd> on a row
removes it, and <kbd>Ctrl</kbd>+<kbd>B</kbd> hides the whole rail.

Drag a row up or down to reorder the list. A line on the row you are over shows
where the dragged one will land, and the new order is saved with the rest of
the session. The number shortcuts go by position, so after a reorder they point
at whatever now sits in those slots.

Dragging is off while the search box has text in it: the filtered list is not
the list you would be reordering, so a drop there would move a project
somewhere you never pointed at.

## Scrolling back

The wheel moves the view through a pane's history. While it moves, a thin bar
on the right edge says where you are and how much there is — the shorter the
thumb, the more history behind it. It fades out a moment after you stop, so an
idle pane is just the pane. Typing snaps the view back to the prompt.

There is no bar when there is nothing above the screen, and none inside a
full-screen app such as `less` or `nvim`: that app owns the pane and keeps no
history of its own. Quitting it brings both back.

## Selecting and copying text

Left-click and drag selects. Double-click selects a word, triple-click selects
a line, and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> copies the selection
(<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> pastes). Typing clears the
selection and snaps the view back to the prompt.

Full-screen apps — Claude Code, vim, htop, lazygit, fzf — usually ask the
terminal for the mouse, and pt hands it over. That is what makes their own
mouse features work: clicking a vim buffer, dragging a pane divider, and
selecting text inside Claude Code, which copies it for you. Hold
<kbd>Shift</kbd> while you drag to take the pointer back for that one gesture
and select with pt instead.

The wheel is not part of that deal. An app that asks for the mouse gets the
wheel whatever `mouse-reporting` says, because scrolling is not how you select
text and an app on the alternate screen leaves pt nothing to scroll. Holding
<kbd>Shift</kbd> takes the wheel back and moves pt's own view, the way every
other terminal does it.

Set `mouse-reporting = false` if you would rather a plain drag always selected
pt's own text, even inside those apps. The cost is that their click-driven
features stop working, including Claude Code's selection. Either way, "Toggle
mouse reporting" in the command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>) flips
the focused pane for the rest of the session without touching the config.

## When a program copies for you

A program can put text on your clipboard by printing an escape sequence
(OSC 52). That is how a yank in nvim over ssh, tmux's copy mode, and selecting
text inside Claude Code all reach the clipboard on the machine you are sitting
at. pt allows it out of the box.

The same sequence has a second form that asks the terminal what is on the
clipboard instead. pt never answers it, at any setting. Anything running in a
pane could use it to read what you last copied, including something on the far
end of an ssh session, and a password manager's clipboard is a bad thing to
hand out on request.

Set `osc52 = ask` if you would rather approve each one. The dialog says which
pane asked and how many bytes it wants to copy, and saying no leaves the
clipboard as it was. `osc52 = off` ignores clipboard writes completely.

Text that arrives this way is put on the clipboard as it is. Nothing is
stripped from it, because it is text you are going to paste somewhere, and its
newlines and tabs are the point. The checks happen when it comes back the other
way: pasting into a pane still rewrites control bytes and still asks first if
the text could run on its own.

## Opening links

A program can mark a piece of its output as a link (`ls --hyperlink=auto` does,
and so do a lot of test runners and build tools). pt underlines those and opens
them on <kbd>Ctrl</kbd>+click; the pointer turns into a hand when there is one
under it. Only `http`, `https`, `file` and `mailto` links open — anything else
is left alone, because the link text and the address behind it are both written
by whatever is running in the pane.

A URL printed as ordinary text is a link too — the `http://localhost:5173/` a
dev server prints, an address in a stack trace. Hold <kbd>Ctrl</kbd> and the
one under the pointer underlines; click it and it opens. Only the address under
the pointer is underlined, not every URL on screen, because a build log full of
underlines tells you nothing.

Bare addresses are found by pattern, and the pattern is careful about where
prose ends and an address begins: a trailing full stop or comma is not part of
it, `(https://example.com)` does not take the closing paren, but
`https://en.wikipedia.org/wiki/Rust_(video_game)` keeps its own. An address
that ran off the right edge is still one address — the rows it wrapped across
are matched as one line. Only the four schemes above are looked for at all, so
nothing is ever underlined that pt would then refuse to open.

Inside an app that has the mouse, <kbd>Ctrl</kbd>+click goes to the app instead,
and holding <kbd>Shift</kbd> takes it back, exactly as with selection. That is
also how ghostty behaves, and how you reach a link inside Claude Code or a
pager: <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+click.

## When a program wants your attention

A long build finishes while pt is on another workspace and nothing tells you.
Programs can say so: printing `\033]9;build done\007` raises a desktop
notification, and `\033]777;notify;Build;done\007` does the same with a title
of its own. Anything can send them — `printf` at the end of a command works —
and pt raises them out of the box.

Clicking one comes back to the pane that sent it: the right project, the right
tab, the right pane, and the window in front. A pane that has closed since is
simply not there any more, and the click does nothing.

The pane you are already reading stays quiet. A notification for the pane in
front of you is telling you something you can see, so pt drops those without
raising anything.

Beyond that, one notification a second, and five seconds before the same text
can repeat. A program in a loop cannot queue thousands of them at your desktop,
and it cannot crowd out the one real notification another pane is trying to
send. Long bodies are cut to fit rather than dropped, up to a point: past 8K
the whole sequence is thrown away, because at that size it is not a message
for a human and pt will not hold on to it to find out.

`\033]9;4;...` is a progress report, not a notification, and does not raise
one.

## Agent usage

Run `claude` or `codex` in a pane and the info panel (⌃I) grows an AGENT USAGE
block: a bar per limit window with how much of it is gone and when it turns
over, plus how full the session's context is. You find out you are near a limit
before the agent stops, rather than after.

Codex needs no setup. It writes its limits into its own session log after every
turn, so pt reads them off disk — no network, no credentials, and nothing
leaves the machine. The log matching the pane's directory is the one that
supplies the context bar; the limits themselves are account-wide.

Claude Code is off until you turn it on, with a button in the panel. It keeps
no local record of its limits, so the only way to show them is to ask Anthropic
using the token the CLI already stored — your credential on the wire, so your
call. pt never refreshes that token: rotating it would log you out of your own
CLI, so an expired login is reported and left alone for you to fix with
`claude`. Turning the setting back off stops the lookups and drops what they
fetched.

Neither one is polled faster than every two minutes, and nothing is polled at
all while the panel is closed or no agent is running. Starting an agent, or
moving to a pane in another directory, asks again ahead of the poll — but no
more often than every few seconds, since the directory follows whichever pane
has focus. A failed lookup leaves the last reading on screen with the error
under it rather than blanking the card. Anthropic's usage endpoint is not a
public API and will change; when it does, the block says the lookup failed
instead of inventing a number.

## Configuration

pt reads `~/.config/pt/config`, a `key = value` file (`#` starts a comment):

```ini
theme = pt-dark
font-size = 9
font-family = JetBrains Mono
ui-font-size = 12.5
ui-font-family = IBM Plex Sans
mouse-reporting = true
osc52 = write
claude-usage = false
resume-agents = true
```

Those nine keys plus `app-*` color-token overrides (e.g.
`app-background = #101010`) are the entire config surface. Booleans accept
`true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0`. `osc52` takes `write`, `ask`
or `off` (see above). `claude-usage` is the info panel's Claude Code opt-in
(see below); the Turn on button writes it here, and setting it back to `false`
turns the lookups off again. `resume-agents` is on by default and restores a
pane's agent conversation (see above). Custom themes go in
`~/.config/pt/themes/<name>`; both the config and the active theme are watched
and applied live.
