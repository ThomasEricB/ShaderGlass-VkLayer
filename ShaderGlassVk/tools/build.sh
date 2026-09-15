#!/usr/bin/env bash
# Build every ShaderGlassVk target: the 64-bit layer and tools, and the 32-bit layer.
#
# A 32-bit game loads only a 32-bit layer, and 32-bit games are a good share of the audience for CRT
# shaders, so the 32-bit build is part of "everything" rather than an extra.
set -euo pipefail
cd "$(dirname "$0")/.."

MESON=${MESON:-meson}
BUILD_ROOT=${SHADERGLASS_BUILD_ROOT:-build}

setup() {
    local dir="$1"; shift
    if [ -f "$dir/meson-private/coredata.dat" ]; then
        "$MESON" setup --reconfigure "$dir" "$@"
    else
        "$MESON" setup "$dir" "$@"
    fi
}

setup "$BUILD_ROOT/native" --native-file meson/native-clang.ini
setup "$BUILD_ROOT/linux32" --native-file meson/native-clang.ini \
                            --cross-file meson/cross-clang-linux32.ini

"$MESON" compile -C "$BUILD_ROOT/native"
"$MESON" compile -C "$BUILD_ROOT/linux32"

echo
echo "built:"
echo "  $BUILD_ROOT/native/layer/libVkLayer_ShaderGlass.so"
echo "  $BUILD_ROOT/linux32/layer/libVkLayer_ShaderGlass_32.so"
echo "  $BUILD_ROOT/native/tools/shaderglass-ctl"
