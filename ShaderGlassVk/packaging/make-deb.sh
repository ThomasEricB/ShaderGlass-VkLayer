#!/usr/bin/env bash
# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
# Derived in structure from DLSS5VKLayer's packaging (relicensed to GPL-3.0, see RELICENSE.md).
#
# Build a .deb from the tree make-dist.sh staged. Called by it (packaging/make-dist.sh deb); needs
# dpkg-deb.
#
#   packaging/make-deb.sh dist/shaderglass-vk-<version>-<release>-linux-x86_64
set -euo pipefail
cd "$(dirname "$0")/.."

STAGE=${1:?usage: $0 <staged directory>}
[ -d "$STAGE/root/usr" ] || { echo "no $STAGE/root/usr -- run packaging/make-dist.sh first" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found (Debian/Ubuntu: dpkg-dev)" >&2; exit 1; }

NAME=shaderglass-vk
VERSION=${SHADERGLASS_VERSION:?}
RELEASE=${SHADERGLASS_RELEASE:-1}
MAINTAINER=${DEB_MAINTAINER:-Thomas Eric <thombelcar@gmail.com>}
WORK="dist/deb-$NAME"
DEB="dist/${NAME}_${VERSION}-${RELEASE}_amd64.deb"

rm -rf "$WORK"
mkdir -p "$WORK/DEBIAN"
cp -a "$STAGE/root/usr" "$WORK/usr"

# Debian keeps licences as copyright files, not in /usr/share/licenses.
install -d "$WORK/usr/share/doc/$NAME"
mv "$WORK/usr/share/licenses/$NAME/LICENSE" "$WORK/usr/share/doc/$NAME/copyright"
rm -rf "$WORK/usr/share/licenses"

size=$(du -sk "$WORK/usr" | cut -f1)
cat > "$WORK/DEBIAN/control" <<EOF
Package: $NAME
Version: $VERSION-$RELEASE
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $size
Section: games
Priority: optional
Homepage: https://github.com/mausimus/ShaderGlass
Depends: libc6, libvulkan1, libqt6widgets6t64 | libqt6widgets6
Recommends: gamescope
Suggests: libc6-i386
Description: RetroArch shader presets applied to games, as a Vulkan layer
 ShaderGlassVk applies libretro/RetroArch slang shader presets -- CRT,
 scanline, handheld and upscaling effects, about 3300 of them -- to a
 game's own frames. It is a Vulkan implicit layer loaded into the game's
 process, so there is no window capture and no overlay. A game is switched
 on with SHADERGLASS=1 in its launch options; shaderglass-gui chooses and
 tunes the effect while it runs.
 .
 shaderglass-gen, for compiling presets of your own, needs glslang's shared
 libraries (libglslang.so.16), which are not a dependency of this package.
EOF

dpkg-deb --build --root-owner-group "$WORK" "$DEB"
echo "built $DEB"
