#!/usr/bin/env bash
# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
# Derived in structure from DLSS5VKLayer's packaging (relicensed to GPL-3.0, see RELICENSE.md).
#
# Remove what install.sh installed -- exactly the files it listed, and nothing else.
#
#   <libdir>/uninstall.sh                     the copy install.sh left beside the layer
#   ./uninstall.sh [--system] [--prefix DIR]  from the tarball, with the options install.sh was given
#
# Settings, profiles and captures are the user's and are left alone.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$here/installed-files" ]; then
    LIBDIR=$here
else
    MODE=user
    PREFIX=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --user) MODE=user ;;
            --system) MODE=system ;;
            --prefix) PREFIX=${2:-}; shift ;;
            --prefix=*) PREFIX=${1#*=} ;;
            -h|--help) sed -n '/^# Remove what/,/^set -euo/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
        shift
    done
    if [ "$MODE" = system ]; then PREFIX=${PREFIX:-/usr/local}; else PREFIX=${PREFIX:-$HOME/.local}; fi
    LIBDIR="$PREFIX/lib/shaderglass"
fi

list="$LIBDIR/installed-files"
[ -f "$list" ] || { echo "nothing installed at $LIBDIR (no $list)" >&2; exit 1; }
if [ ! -w "$LIBDIR" ]; then
    echo "$LIBDIR is not writable: run as the user (or root) that installed it" >&2
    exit 1
fi

removed=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if [ -e "$f" ] || [ -L "$f" ]; then
        rm -f -- "$f"
        removed=$((removed + 1))
    fi
done < "$list"

# This package's own directories, once empty -- deepest first. Shared ones (bin, applications, the
# manifest directory, environment.d) are left even when empty: something else may have made them.
grep '/shaderglass-vk/' "$list" | xargs -r -n1 dirname | sort -ru |
    while IFS= read -r d; do rmdir -- "$d" 2>/dev/null || true; done
rm -f "$list" "$LIBDIR/uninstall.sh"
rmdir "$LIBDIR" 2>/dev/null || true

echo "removed $removed files installed under $LIBDIR and beside it"
echo "the session layer order goes at your next login"
