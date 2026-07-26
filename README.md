# pt

A terminal workspace for Linux, built with GTK4 + libadwaita on top of
libghostty-vt.

## Install from a release

Prebuilt tarballs for Linux x86_64 and aarch64 are published on GitHub
Releases. Install the latest one with:

```sh
curl -fsSL https://github.com/hdprajwal/pt/releases/latest/download/install.sh | sh
```

This installs `pt` to `~/.local/bin` and the prompt snippets to
`~/.local/share/pt/prompt` (set `PT_PREFIX` to change the prefix; pass a tag
as an argument to pin a version). You still need the runtime libraries from
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

## Configuration

pt reads `~/.config/pt/config`, a `key = value` file (`#` starts a comment):

```ini
theme = pt-dark
font-size = 9
font-family = JetBrains Mono
ui-font-size = 12.5
ui-font-family = IBM Plex Sans
```

Those five keys plus `app-*` color-token overrides (e.g.
`app-background = #101010`) are the entire config surface. Custom themes go
in `~/.config/pt/themes/<name>`; both the config and the active theme are
watched and applied live.
