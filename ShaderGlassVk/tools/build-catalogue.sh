#!/usr/bin/env bash
#
# Build libShaderGlassPresets.so from a libretro slang-shaders tree.
#
# This is the whole catalogue step in one command -- fetch, patch, generate, compile -- because it
# is the step a package's build runs, and every part of it has to happen every time. In particular
# the patches in ../patches are not optional: they fix shaders that are broken as written, and a
# catalogue built without them ships presets that render black. If patching fails this stops rather
# than generating anyway.
#
#   tools/build-catalogue.sh                      # clone into build/slang-shaders, then build
#   tools/build-catalogue.sh /path/to/slang-shaders
#
# Environment:
#   SHADERGLASS_BUILD_ROOT   where build output goes (default: build)
#   SHADERGLASS_GEN          the generator to use (default: $BUILD_ROOT/native/gen/shaderglass-gen)
#   SHADERS_URL              where to clone from
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_ROOT=${SHADERGLASS_BUILD_ROOT:-build}
GEN=${SHADERGLASS_GEN:-$BUILD_ROOT/native/gen/shaderglass-gen}
SHADERS_URL=${SHADERS_URL:-https://github.com/libretro/slang-shaders.git}
TREE=${1:-$BUILD_ROOT/slang-shaders}

if [ ! -x "$GEN" ]; then
    echo "no generator at $GEN -- build it first (tools/build.sh), or set SHADERGLASS_GEN" >&2
    exit 2
fi

if [ ! -d "$TREE" ]; then
    if [ -n "${1:-}" ]; then
        echo "no shader tree at $TREE" >&2
        exit 2
    fi
    echo "cloning $SHADERS_URL into $TREE"
    git clone --depth 1 "$SHADERS_URL" "$TREE"
fi

# Not optional, and not silent: the tree is modified in place, and tools/patch-shaders.sh --revert
# undoes it.
echo "patching $TREE"
if ! tools/patch-shaders.sh "$TREE"; then
    echo >&2
    echo "refusing to build a catalogue from an unpatched tree." >&2
    echo "a patch that no longer applies usually means the shader was fixed upstream --" >&2
    echo "check, and drop the patch from patches/ if so." >&2
    exit 1
fi

OUT="$BUILD_ROOT/catalog"
echo "generating $OUT"
rm -rf "$OUT"

# The generator exits 1 when some presets failed and 2 when it could not run at all. Over the whole
# libretro tree a few dozen always fail, on references that are dangling upstream -- that is the
# normal result, not a reason to abandon 3300 working presets. Anything else is fatal.
set +e
"$GEN" --tree "$TREE" --out "$OUT"
gen_status=$?
set -e

if [ "$gen_status" -gt 1 ]; then
    echo "the generator failed (exit $gen_status)" >&2
    exit 1
fi
if [ ! -f "$OUT/sg_index.cpp" ]; then
    echo "the generator produced no index; nothing to compile" >&2
    exit 1
fi

echo "compiling $OUT/cm"
cmake -S "$OUT" -B "$OUT/cm" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$OUT/cm" -j

echo
echo "built: $OUT/cm/libShaderGlassPresets.so"
if [ "$gen_status" -ne 0 ]; then
    echo "(some presets did not compile -- see the FAIL lines above; over the full libretro tree"
    echo " these are references that are dangling upstream)"
fi
