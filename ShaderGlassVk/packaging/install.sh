#!/usr/bin/env bash
# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
# Derived in structure from DLSS5VKLayer's packaging (relicensed to GPL-3.0, see RELICENSE.md).
#
# Install from the release tarball.
#
#   ./install.sh                  for this user, under ~/.local (the default)
#   ./install.sh --system         for everyone, under /usr/local (as root)
#   ./install.sh --prefix DIR     under DIR instead; with --system, DIR/lib, DIR/bin, DIR/share
#
# Everything installed is listed in <libdir>/installed-files, which is what uninstall.sh removes --
# nothing more, and nothing it has to guess at.
set -euo pipefail

usage() { sed -n '/^# Install from/,/^set -euo/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'; }

MODE=user
PREFIX=""
while [ $# -gt 0 ]; do
    case "$1" in
        --user) MODE=user ;;
        --system) MODE=system ;;
        --prefix) PREFIX=${2:-}; [ -n "$PREFIX" ] || { usage >&2; exit 2; }; shift ;;
        --prefix=*) PREFIX=${1#*=} ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

src="$(cd "$(dirname "$0")" && pwd)/root/usr"
[ -d "$src/lib/shaderglass" ] || { echo "no root/usr beside this script: run it from the unpacked tarball" >&2; exit 1; }

if [ "$MODE" = system ]; then
    [ "$(id -u)" -eq 0 ] || { echo "--system needs root" >&2; exit 1; }
    PREFIX=${PREFIX:-/usr/local}
    # The loader and systemd read their directories under /usr and /usr/local; anywhere else, the
    # manifests and the session variable go under /etc, which both read too.
    case "$PREFIX" in
        /usr|/usr/local)
            MANIFESTS="$PREFIX/share/vulkan/implicit_layer.d"
            ENVD="$PREFIX/lib/environment.d" ;;
        *)
            MANIFESTS=/etc/vulkan/implicit_layer.d
            ENVD=/etc/environment.d ;;
    esac
    DATA="$PREFIX/share"
else
    PREFIX=${PREFIX:-$HOME/.local}
    DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
    [ "$PREFIX" = "$HOME/.local" ] || DATA="$PREFIX/share"
    # The loader looks for a user's implicit layers only here, whatever the prefix.
    MANIFESTS="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
    ENVD="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d"
fi
LIBDIR="$PREFIX/lib/shaderglass"
BINDIR="$PREFIX/bin"

list="$LIBDIR/installed-files"
mkdir -p "$LIBDIR"
: > "$list.new"
put() { # mode, source, destination
    install -D -m "$1" "$2" "$3"
    printf '%s\n' "$3" >> "$list.new"
}

for f in "$src"/lib/shaderglass/*; do put 755 "$f" "$LIBDIR/$(basename "$f")"; done
for f in "$src"/bin/*; do put 755 "$f" "$BINDIR/$(basename "$f")"; done

# The manifests name the library by absolute path, so they are rewritten for where it now is.
# Written under a temporary name and renamed: the loader may be reading this directory the moment a
# game starts, and a half-written manifest is a layer it silently drops.
mkdir -p "$MANIFESTS"
for m in "$src"/share/vulkan/implicit_layer.d/*.json; do
    out="$MANIFESTS/$(basename "$m")"
    sed "s#/usr/lib/shaderglass/#$LIBDIR/#" "$m" > "$out.new.$$"
    mv -f "$out.new.$$" "$out"
    printf '%s\n' "$out" >> "$list.new"
done

put 644 "$src/lib/environment.d/zz-shaderglass.conf" "$ENVD/zz-shaderglass.conf"
put 644 "$src/share/applications/shaderglass.desktop" "$DATA/applications/shaderglass.desktop"
put 644 "$src/share/icons/hicolor/128x128/apps/shaderglass.png" \
        "$DATA/icons/hicolor/128x128/apps/shaderglass.png"
while IFS= read -r -d '' f; do
    rel=${f#"$src/share/"}
    put 644 "$f" "$DATA/$rel"
done < <(find "$src/share/doc" "$src/share/licenses" -type f -print0)

mv -f "$list.new" "$list"
install -m755 "$(dirname "$0")/uninstall.sh" "$LIBDIR/uninstall.sh"

cat <<EOM
installed ShaderGlassVk under $PREFIX
  layers and catalogues   $LIBDIR
  programs                $BINDIR/shaderglass-{gui,ctl,run,gen}
  layer manifests         $MANIFESTS
  session layer order     $ENVD/zz-shaderglass.conf

Log out and back in once: the layer order is read when the session starts, and programs already
running -- Steam among them -- keep what they started with.

Then a game's launch options are just:

  SHADERGLASS=1 %command%

To remove it:  $LIBDIR/uninstall.sh
EOM
case ":$PATH:" in *":$BINDIR:"*) ;; *) echo; echo "note: $BINDIR is not on your PATH" ;; esac
