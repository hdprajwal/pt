#!/bin/sh
# pt installer — downloads a release tarball from GitHub and installs it.
#
#   curl -fsSL https://github.com/hdprajwal/pt/releases/latest/download/install.sh | sh
#
# Usage: install.sh [tag]     install a specific release (default: latest)
# Env:   PT_PREFIX=<dir>      install prefix (default: ~/.local)
set -eu

REPO=hdprajwal/pt
PREFIX=${PT_PREFIX:-"$HOME/.local"}
TAG=${1:-latest}

case "$(uname -sm)" in
  "Linux x86_64")  ARCH=x86_64 ;;
  "Linux aarch64") ARCH=aarch64 ;;
  *) echo "install.sh: unsupported platform: $(uname -sm)" >&2; exit 1 ;;
esac

if [ "$TAG" = latest ]; then
  TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" |
        sed -n 's/^ *"tag_name": *"\([^"]*\)".*/\1/p' | head -n1)
  if [ -z "$TAG" ]; then
    echo "install.sh: could not resolve the latest release tag" >&2
    exit 1
  fi
fi

NAME="pt-$TAG-linux-$ARCH"
BASE="https://github.com/$REPO/releases/download/$TAG"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "downloading $NAME.tar.gz"
curl -fsSL -o "$TMP/$NAME.tar.gz" "$BASE/$NAME.tar.gz"
curl -fsSL -o "$TMP/checksums.txt" "$BASE/checksums.txt"
(cd "$TMP" && grep " $NAME.tar.gz\$" checksums.txt | sha256sum -c -)

tar -xzf "$TMP/$NAME.tar.gz" -C "$TMP"
mkdir -p "$PREFIX/bin" "$PREFIX/share/pt/prompt" \
         "$PREFIX/share/applications" \
         "$PREFIX/share/icons/hicolor/scalable/apps"
install -m755 "$TMP/$NAME/bin/pt" "$PREFIX/bin/pt"
install -m644 "$TMP/$NAME"/share/pt/prompt/pt-prompt.* "$PREFIX/share/pt/prompt/"
install -m644 "$TMP/$NAME/share/icons/hicolor/scalable/apps/dev.hdprajwal.pt.svg" \
  "$PREFIX/share/icons/hicolor/scalable/apps/"
# Absolute Exec: app launchers do not necessarily share the shell's PATH.
sed "s|^Exec=pt$|Exec=$PREFIX/bin/pt|" \
  "$TMP/$NAME/share/applications/dev.hdprajwal.pt.desktop" \
  > "$PREFIX/share/applications/dev.hdprajwal.pt.desktop"
command -v update-desktop-database >/dev/null 2>&1 &&
  update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null 2>&1 &&
  gtk-update-icon-cache -qt "$PREFIX/share/icons/hicolor" 2>/dev/null || true

echo "installed pt $TAG -> $PREFIX/bin/pt (desktop entry + icon included)"

case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "note: $PREFIX/bin is not on your PATH" ;;
esac

echo "
runtime dependencies (install via your package manager):
  gtk4 >= 4.16, libadwaita, json-glib
fonts (defaults, override in ~/.config/pt/config):
  JetBrains Mono, IBM Plex Sans

shell prompt integration — add to your shell rc:
  source $PREFIX/share/pt/prompt/pt-prompt.zsh   # or .bash / .fish"
