# Contributing

## Building and running

The [README](README.md) has the requirements and the build steps. The short
version:

```sh
cmake -B build
cmake --build build
```

To try your changes, run `./dev.sh` instead of `./build/pt`. pt only allows
one instance, so running the build directly on a machine where pt is already
open just opens a window in the running pt. The script gives the dev build
its own session bus and its own config folder, so it runs beside your real pt
without touching anything.

## Tests

```sh
ctest --test-dir build
```

The parsers and the core logic (terminal core, git parsing, config, links,
sessions, usage) have C tests in `tests/`. If you change any of that, update
the matching test file. New logic that does not need a GTK window should come
with a test.

## Terminal behavior

For anything that touches the terminal protocol (escape sequences, OSC
handling, selection, links), pt copies what ghostty does, on purpose. If you
are not sure how something should behave, check ghostty and do the same. Its
source is downloaded into `build/_deps/ghostty-src` on the first configure.

## Pull requests

- Keep a PR to one change, and say what changes and why.
- Match the style of the code around you. There is no formatter config, so
  the existing files are the spec.
- For new features or UI changes, open an issue first so we can agree on the
  design before you build it. pt stays minimal on purpose.

## Reporting bugs

Open an issue with your distro, your GTK4 and libadwaita versions, and steps
to reproduce. For rendering or escape sequence bugs, include the program and
output that trigger it. A `printf` one-liner is perfect.

For security problems, use [SECURITY.md](SECURITY.md) instead of the issue
tracker.
