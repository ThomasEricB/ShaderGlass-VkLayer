#!/usr/bin/env bash
# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
# Derived in structure from DLSS5VKLayer's packaging (relicensed to GPL-3.0, see RELICENSE.md).
#
# Build the release packages.
#
#   packaging/make-dist.sh [tar|rpm|deb|all|stage]     (default: tar)
#
# "stage" stops once the tree is staged, and prints where: the PKGBUILD packages from it.
#
# Everything is staged once, into dist/<name>-<version>-<release>-linux-x86_64/root/usr, and every
# format is made from that one tree: the tarball carries it with install.sh, and the RPM and DEB use
# it as their payload. Three formats cannot drift apart when there is only one layout.
#
# What is built first, if it is not built already: both layers and the tools (tools/build.sh), and
# both catalogues (tools/build-catalogue.sh) -- which applies the shader patches, and refuses to build
# a catalogue from a tree it could not patch. That refusal is why packaging goes through it rather
# than around it: a shipped catalogue cannot quietly be missing them.
#
# Environment:
#   SHADERGLASS_VERSION   default: the version in meson.build
#   SHADERGLASS_RELEASE   default: 1
#   SHADERGLASS_BUILD_ROOT, SHADERS_URL, SHADERS_REF   as tools/build-catalogue.sh
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-tar}
case "$MODE" in
    tar|rpm|deb|all|stage) ;;
    *) echo "usage: $0 [tar|rpm|deb|all|stage]" >&2; exit 2 ;;
esac

NAME=shaderglass-vk
VERSION=${SHADERGLASS_VERSION:-$(sed -n "s/^ *version: *'\([^']*\)'.*/\1/p" meson.build | head -1)}
RELEASE=${SHADERGLASS_RELEASE:-1}
BUILD=${SHADERGLASS_BUILD_ROOT:-build}
DIST=dist
STAGE="$DIST/$NAME-$VERSION-$RELEASE-linux-x86_64"
ROOT="$STAGE/root"

# The layer's private directory. /usr/lib rather than /usr/lib64 on every distribution: it holds both
# architectures' layers and catalogues side by side -- each layer finds its own catalogue beside it --
# so it is not a 64-bit library directory in the sense lib64 means.
LIBDIR=/usr/lib/shaderglass

need() { [ -f "$1" ] || return 1; }
if ! need "$BUILD/native/layer/libVkLayer_ShaderGlass.so" ||
   ! need "$BUILD/linux32/layer/libVkLayer_ShaderGlass_32.so" ||
   ! need "$BUILD/native/gui/shaderglass-gui" ||
   ! need "$BUILD/native/tools/shaderglass-ctl" ||
   ! need "$BUILD/native/gen/shaderglass-gen"; then
    SHADERGLASS_BUILD_ROOT="$BUILD" tools/build.sh
fi
if ! need "$BUILD/catalog/cm/libShaderGlassPresets.so" ||
   ! need "$BUILD/catalog/cm32/libShaderGlassPresets_32.so"; then
    SHADERGLASS_BUILD_ROOT="$BUILD" tools/build-catalogue.sh
fi

rm -rf "$STAGE"
install -d "$ROOT$LIBDIR" "$ROOT/usr/bin" "$ROOT/usr/share/vulkan/implicit_layer.d" \
           "$ROOT/usr/lib/environment.d" "$ROOT/usr/share/applications" \
           "$ROOT/usr/share/icons/hicolor/128x128/apps" "$ROOT/usr/share/doc/$NAME/docs" \
           "$ROOT/usr/share/licenses/$NAME"

install -m755 "$BUILD/native/layer/libVkLayer_ShaderGlass.so" "$ROOT$LIBDIR/"
install -m755 "$BUILD/linux32/layer/libVkLayer_ShaderGlass_32.so" "$ROOT$LIBDIR/"
install -m755 "$BUILD/catalog/cm/libShaderGlassPresets.so" "$ROOT$LIBDIR/"
install -m755 "$BUILD/catalog/cm32/libShaderGlassPresets_32.so" "$ROOT$LIBDIR/"

install -m755 "$BUILD/native/gui/shaderglass-gui" "$ROOT/usr/bin/"
install -m755 "$BUILD/native/tools/shaderglass-ctl" "$ROOT/usr/bin/"
install -m755 "$BUILD/native/gen/shaderglass-gen" "$ROOT/usr/bin/"
install -m755 tools/shaderglass-run "$ROOT/usr/bin/"

# One manifest per architecture, each naming its own library and layer name. The loader keys
# implicit layers by name, so a shared name would keep one entry and reject it for the wrong word
# size; library_arch lets it skip the other without trying to load it.
manifest() { # source manifest, library, output file, architecture
    sed -e "s#\"./[^\"]*\"#\"$LIBDIR/$2\"#" \
        -e "s#\"implementation_version\"#\"library_arch\": \"$4\",\n    \"implementation_version\"#" \
        "$1" > "$ROOT/usr/share/vulkan/implicit_layer.d/$3"
}
manifest layer/manifest/VK_LAYER_SHADERGLASS.json libVkLayer_ShaderGlass.so \
         VK_LAYER_SHADERGLASS.x86_64.json 64
manifest layer/manifest/VK_LAYER_SHADERGLASS_32.json libVkLayer_ShaderGlass_32.so \
         VK_LAYER_SHADERGLASS_32.i686.json 32

install -m644 packaging/zz-shaderglass.conf "$ROOT/usr/lib/environment.d/"
install -m644 packaging/shaderglass.desktop "$ROOT/usr/share/applications/"
install -m644 packaging/shaderglass.png "$ROOT/usr/share/icons/hicolor/128x128/apps/"
install -m644 README.md DESIGN.md RELICENSE.md "$ROOT/usr/share/doc/$NAME/"
install -m644 docs/*.md "$ROOT/usr/share/doc/$NAME/docs/"
install -m644 ../LICENSE "$ROOT/usr/share/licenses/$NAME/LICENSE"

install -m755 packaging/install.sh packaging/uninstall.sh "$STAGE/"
if [ "$MODE" = stage ]; then
    echo "$STAGE"
    exit 0
fi
tar -C "$DIST" -czf "$STAGE.tar.gz" "$(basename "$STAGE")"
echo "built $STAGE.tar.gz"

if [ "$MODE" = rpm ] || [ "$MODE" = all ]; then
    topdir="$PWD/$DIST/rpmbuild"
    mkdir -p "$topdir"
    rpmbuild --define "_topdir $topdir" --define "_sourcedir $PWD/$DIST" \
             --define "sg_version $VERSION" --define "sg_release $RELEASE" \
             -bb packaging/shaderglass-vk.spec
    cp "$topdir"/RPMS/x86_64/"$NAME"-*.rpm "$DIST/"
fi

if [ "$MODE" = deb ] || [ "$MODE" = all ]; then
    SHADERGLASS_VERSION="$VERSION" SHADERGLASS_RELEASE="$RELEASE" packaging/make-deb.sh "$STAGE"
fi

echo
echo "artifacts:"
ls -1 "$DIST"/"$NAME"-"$VERSION"-* "$DIST"/"$NAME"_"$VERSION"-* 2>/dev/null | grep -v '/$' |
    grep -E '\.(tar\.gz|rpm|deb)$' || true
