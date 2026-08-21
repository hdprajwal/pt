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
- `tic`, the terminfo compiler, to build the `xterm-ghostty` entry pt ships
  (package `ncurses` on Fedora and Arch, `ncurses-bin` on Debian)
- git and network access on the first configure (the ghostty source is
  fetched at a pinned commit via CMake FetchContent)

On Arch-based systems:

```sh
sudo pacman -S --needed base-devel cmake pkgconf ncurses gtk4 libadwaita json-glib
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

## Terminfo

pt compiles ghostty's `xterm-ghostty` terminfo entry from
`share/terminfo/xterm-ghostty.src` and installs it under `share/pt/terminfo`,
so it is there whether or not ghostty itself is installed on the machine. Every
pane gets that directory at the front of `TERMINFO_DIRS`, which puts pt's copy
first without hiding the system database behind it.

`TERM` travels over ssh but terminfo does not, so a remote host that has never
heard of the entry cannot look it up. The standard remedy is to send it over
once:

```sh
infocmp -x xterm-ghostty | ssh host 'tic -x -'
```

Doing that automatically, the way ghostty's ssh integration does, is follow-up
work and is not shipped here.

`sudo` has the same problem closer to home. It resets the environment by
default, so `TERMINFO_DIRS` does not reach the command it runs and pt's copy of
the entry is out of reach even though it is on the same machine. `sudo -E`
keeps the variable, or add it once:

```sh
# /etc/sudoers.d/terminfo, via visudo
Defaults env_keep += "TERMINFO_DIRS"
```

`su` and `docker exec` drop it for the same reason. If none of that suits you,
set `term = xterm-256color` in the config and pt stops sending the ghostty name
at all. Nothing else about pt changes: `TERM_PROGRAM`, `TERM_PROGRAM_VERSION`
and the `XTVERSION` reply still say ghostty, because they describe what pt
implements rather than which terminfo entry it asks you to look up.

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
pt integration install claude   # adds its hooks to ~/.claude/settings.json
pt integration install codex    # prints a notify line for ~/.codex/config.toml
pt integration status           # says which of the two are in place
```

Claude Code's settings are JSON, so pt edits them in place — additively, and it
refuses rather than rewrite a file it did not parse. codex's `config.toml`
carries comments and ordering that no TOML round-trip preserves, so pt prints
the `notify = ["<path-to-pt>", "agent-report", "codex-notify"]` line and you
paste it yourself. Both hooks run `pt agent-report`, which is inert outside a
pt pane, and `install` a second time changes nothing.

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

### Lifecycle notifications

The same report files carry the news, too. When an agent finishes a turn or
stops to ask you something while its pane is in the background, pt raises a
desktop notification; clicking it brings the pane forward. Claude Code needs
two more hooks for this, which `pt integration install claude` adds on top of
the SessionStart one — Stop (turn finished) and Notification (waiting on you).
`pt integration status` lists each hook separately. codex needs nothing extra:
its `notify` already fires on turn completion and approvals, and pt reads the
difference out of the payload.

A pane you are looking at never notifies, the same event does not repeat for
the same pane, and a plan-limit warning comes once per episode rather than
once per poll — it re-arms only after usage drops back down. The limit notice
also waits until the info panel is closed: if the panel is open, the bars are
already on screen.

### Recent agent sessions

<kbd>Ctrl</kbd>+<kbd>K</kbd> opens the command palette; "Recent agent sessions"
lists the conversations those report files still describe, newest first. Each
row shows the directory, the agent and how long since it was last active.
Enter resumes
the session in a new tab: if its directory is already an open project the tab
goes there, otherwise pt adds the folder as a project first. The ⧉ at the end
of a row copies the session id, in case you would rather paste
`claude --resume <id>` yourself.

This is a recent list, not history: each pane keeps its report file current
while an agent runs there, and files older than a week are deleted. A row whose
directory no longer exists shows as `[missing]` and cannot be activated.


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

## Panes

<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>D</kbd> splits side by side and
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>S</kbd> one above the other; the new pane
starts in the focused pane's directory.
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>O</kbd> cycles between panes, and
<kbd>Ctrl</kbd>+<kbd>Alt</kbd>+an arrow key moves in that direction.
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>W</kbd> closes the focused pane.

<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>Z</kbd> zooms the focused pane: it takes
over the whole tab area while every other pane keeps running, hidden. The
status bar shows a small `⤢ zoomed` chip while it lasts, and pressing the
binding again puts every pane back — pt sets each divider from what it
remembers rather than trusting the resized layout. If the window kept its
size the splits land exactly where they were; if it changed, a divider can
end up a pixel or two off.

Zoom is a view on the grid, not a change to it, so it does not survive much:
splitting or closing a pane, moving focus to another pane, switching tabs or
projects — any of these un-zooms first and shows the full grid again. A
single-pane tab has nothing to fill, so the binding does nothing there.

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
term = xterm-ghostty
mouse-reporting = true
osc52 = write
claude-usage = false
resume-agents = true
scrollback-limit = 10000000
window-padding-x = 20
window-padding-y = 18
```

Those thirteen keys plus `app-*` color-token overrides (e.g.
`app-background = #101010`) are the entire config surface. Booleans accept
`true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0`. `osc52` takes `write`, `ask`
or `off` (see above). `claude-usage` is the info panel's Claude Code opt-in
(see below); the Turn on button writes it here, and setting it back to `false`
turns the lookups off again. `resume-agents` is on by default and restores a
pane's agent conversation (see above). `scrollback-limit` is how much history
one pane keeps, in bytes rather than lines — ghostty's key, its semantics and
its 10MB default — and it is read when a pane spawns, so a change applies to
panes opened after it. `window-padding-x` and `window-padding-y` are the pixels
between a pane's edge and its text, ghostty's keys of the same name, and they
apply live to every open pane. `term` is what a pane's child is told `TERM` is,
ghostty's key of the same name and its default (see above); it is read when a
pane spawns, and a name with no terminfo entry behind it falls back to
`xterm-256color`. Custom themes go in
`~/.config/pt/themes/<name>`; both the config and the active theme are watched
and applied live.

### Keybindings

App shortcuts can be rebound from the same file:

```ini
bind alt+j switch-project-1
unbind ctrl+b
```

A bind line is `bind <accel> <action>` and an unbind line is `unbind <accel>`.
There is no `=` sign on these lines, on purpose: `=` is itself a key you can
bind. The accelerator is lowercase modifiers joined by `+`, then a key name.
Modifiers are `ctrl`, `shift`, `alt` and `super`, and an accelerator needs at
least one of them — a bare key would be caught before any pane saw it. A plain
`ctrl+<letter>` is accepted but prints a warning, since programs running in
the pane see the same chord (Ctrl+C sends SIGINT). Keys are letters, digits,
`f1` through `f24`, and these named keys:

```text
enter tab escape space up down left right pgup pgdn home end delete backspace
equal plus minus comma period slash backslash semicolon quote
bracketleft bracketright
```

Punctuation travels by name only — write `equal`, never `=`. Everything on a
bind line reads either case.

The action names, with the keys they are on by default:

| Action | Default |
| --- | --- |
| `switch-project-1` … `switch-project-9` | Ctrl+1 … Ctrl+9 |
| `switch-tab-1` … `switch-tab-9` | Alt+1 … Alt+9 |
| `new-tab` | Ctrl+T, Ctrl+Shift+T |
| `add-project` | Ctrl+N |
| `toggle-sidebar` | Ctrl+B |
| `toggle-infopanel` | Ctrl+I |
| `next-tab` / `prev-tab` | Ctrl+PgDn / Ctrl+PgUp |
| `next-project` / `prev-project` | (none) |
| `split-h` / `split-v` | Ctrl+Shift+D / Ctrl+Shift+S |
| `close-pane` | Ctrl+Shift+W |
| `focus-next` | Ctrl+Shift+O, Ctrl+Super+] |
| `focus-prev` | Ctrl+Super+[ |
| `focus-left` / `focus-right` / `focus-up` / `focus-down` | Ctrl+Alt+arrows |
| `paste` / `copy` | Ctrl+Shift+V / Ctrl+Shift+C |
| `font-zoom-in` | Ctrl+= (also +, keypad +) |
| `font-zoom-out` | Ctrl+- (also _, keypad −) |
| `font-zoom-reset` | Ctrl+0 |
| `pane-zoom` | Ctrl+Shift+Z |

Rules worth knowing:

- One action carries one binding. Rebinding an action moves it off every key
  it was on, including aliases like the keypad plus next to `=`; unbinding a
  key removes whatever sat on it. The last line for an accelerator or an
  action wins.
- A line that does not parse prints a warning naming the file's line, and the
  rest still apply. Nothing crashes and nothing half-applies.
- The command palette shows what a shortcut actually is now: project rows
  carry their rebound chord, and the pane-zoom row names the key that flips
  it.
- Bindings are read once at startup. Everything else in the config applies
  live when the file changes; keys do not, until the next launch.

Two things are not rebindable yet. Ctrl+K (command palette) and Ctrl+, 
(settings) are wired by hand before the table is consulted, and the Tab
chords — Ctrl+Tab cycles projects, Alt+Tab cycles tabs, Shift runs each
backwards — cannot be spelled as accelerators at all, because Tab reaches pt
as either of two keysyms depending on the compositor and the table can only
ask for one.


## Built on

pt stands on other people's open source work:

- [ghostty](https://github.com/ghostty-org/ghostty)'s `libghostty-vt` is the
  core of the terminal emulation, and its behavior is the reference for how
  pt handles the terminal protocol. The `xterm-ghostty` terminfo entry pt
  compiles and installs is ghostty's too.
- [GTK4](https://gtk.org) and
  [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/) draw the UI,
  with GLib/GIO and json-glib underneath.
- [Zig](https://ziglang.org) builds `libghostty-vt`, and
  [curl](https://curl.se) does the network call for the Claude usage lookup.

## License

MIT, see [LICENSE](LICENSE). The `lib-vt` code fetched from ghostty at build
time is MIT too, and its notice covers the release binaries built from it. The
terminfo entry in `share/terminfo/` is ghostty's work under the same license,
and its notice covers the compiled copy in the release tarballs.
