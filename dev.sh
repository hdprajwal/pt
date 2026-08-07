#!/bin/sh
# Build pt from this working tree and run it beside the pt you already have
# open, with neither one able to disturb the other.
#
#   ./dev.sh                build and run
#   ./dev.sh --no-build     run whatever is already built
#   ./dev.sh --clean        start from an empty config instead of a copy
#   PT_DEV_HOME=<dir>       where the dev instance keeps its config
#
# `./build/pt` on its own does the wrong thing on a machine where pt is
# already running, in two separate ways:
#
# - pt is a single-instance GApplication. A second launch finds the first one
#   over the session bus and asks it to open a window, so you get an empty
#   window inside the session you are working in and no dev instance at all.
#   dbus-run-session gives this one a bus of its own, and there is nothing on
#   it for the launch to find.
# - config, state.json and themes all resolve through $XDG_CONFIG_HOME, and pt
#   writes its session back when it closes. A dev instance sharing that
#   directory rewrites your real project list on exit. Pointing it somewhere
#   else is the only thing that stops it.
#
# Your config, projects and themes are copied in the first time so the dev
# instance opens on something familiar. Copies: nothing here writes to
# ~/.config/pt. ~/.codex and ~/.claude are read from $HOME either way, so the
# agent usage panel still sees real data.
set -eu

DEV_HOME=${PT_DEV_HOME:-/tmp/pt-dev}
BUILD=build
DO_BUILD=1
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --clean)    CLEAN=1 ;;
    -h|--help)
      echo "usage: dev.sh [--no-build] [--clean]   (PT_DEV_HOME=<dir>)"
      exit 0 ;;
    *) echo "dev.sh: unknown option: $arg" >&2; exit 1 ;;
  esac
done

cd "$(dirname "$0")"

if ! command -v dbus-run-session >/dev/null 2>&1; then
  echo "dev.sh: dbus-run-session not found — it ships with dbus" >&2
  exit 1
fi

# --clean is an rm -rf on a path the caller can set, so it gets a floor.
case "$DEV_HOME" in
  ""|"/"|"$HOME"|"$HOME/")
    echo "dev.sh: refusing to use '$DEV_HOME' as PT_DEV_HOME" >&2
    exit 1 ;;
esac

if [ "$DO_BUILD" = 1 ]; then
  echo "==> building"
  cmake -S . -B "$BUILD" >/dev/null
  cmake --build "$BUILD" -j "$(nproc)"
fi

if [ "$CLEAN" = 1 ]; then rm -rf "$DEV_HOME"; fi

if [ ! -d "$DEV_HOME/pt" ]; then
  mkdir -p "$DEV_HOME/pt"
  for f in config state.json; do
    if [ -f "$HOME/.config/pt/$f" ]; then
      cp "$HOME/.config/pt/$f" "$DEV_HOME/pt/$f"
    fi
  done
  if [ -d "$HOME/.config/pt/themes" ]; then
    cp -r "$HOME/.config/pt/themes" "$DEV_HOME/pt/"
  fi
  echo "==> seeded $DEV_HOME/pt from ~/.config/pt"
fi

echo "==> running $BUILD/pt (config: $DEV_HOME)"
export XDG_CONFIG_HOME="$DEV_HOME"
exec dbus-run-session -- "./$BUILD/pt"
