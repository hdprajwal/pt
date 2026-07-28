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

## Selecting and copying text

Left-click and drag selects. Double-click selects a word, triple-click selects
a line, and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> copies the selection
(<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> pastes). Typing clears the
selection and snaps the view back to the prompt.

That holds inside full-screen apps too — Claude Code, vim, htop, lazygit, fzf.
Those apps normally ask the terminal for the mouse, and pt does not hand it
over: `mouse-reporting` ships off, which is the one place pt deliberately
differs from ghostty's defaults. The cost is that mouse-driven features inside
those apps (clicking a vim buffer, dragging a pane divider) do nothing.

Set `mouse-reporting = true` for the usual terminal behaviour. A plain drag
then goes to the app, and holding <kbd>Shift</kbd> takes the pointer back for
the length of one gesture so you can still select. Either way, "Toggle mouse
reporting" in the command palette (<kbd>Ctrl</kbd>+<kbd>K</kbd>) flips the
focused pane for the rest of the session without touching the config.

## Opening links

A program can mark a piece of its output as a link (`ls --hyperlink=auto` does,
and so do a lot of test runners and build tools). pt underlines those and opens
them on <kbd>Ctrl</kbd>+click; the pointer turns into a hand when there is one
under it. Only `http`, `https`, `file` and `mailto` links open — anything else
is left alone, because the link text and the address behind it are both written
by whatever is running in the pane.

A URL printed as ordinary text is not a link. The program has to say it is.

Inside an app that has the mouse, <kbd>Ctrl</kbd>+click goes to the app instead,
and holding <kbd>Shift</kbd> takes it back, exactly as with selection.

## Configuration

pt reads `~/.config/pt/config`, a `key = value` file (`#` starts a comment):

```ini
theme = pt-dark
font-size = 9
font-family = JetBrains Mono
ui-font-size = 12.5
ui-font-family = IBM Plex Sans
mouse-reporting = false
```

Those six keys plus `app-*` color-token overrides (e.g.
`app-background = #101010`) are the entire config surface. Booleans accept
`true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0`. Custom themes go in
`~/.config/pt/themes/<name>`; both the config and the active theme are watched
and applied live.
