#!/bin/bash
#
# Apply ShaderGlassVk's patches to a checkout of libretro/slang-shaders.
#
# The catalogue is generated from that tree unmodified wherever possible. These patches exist for
# shaders that are broken as written -- not merely different from what we would do -- and each one
# carries its reasoning in the patch header. Run this after cloning and before shaderglass-gen.
#
#   tools/patch-shaders.sh /path/to/slang-shaders
#   tools/patch-shaders.sh --revert /path/to/slang-shaders
#
# The tree is modified in place. Idempotent: a patch already applied is skipped, not reapplied, and
# --revert takes them all back out again.
set -u

revert=0
if [ "${1:-}" = "--revert" ]; then
    revert=1
    shift
fi

tree="${1:-}"
if [ -z "$tree" ] || [ ! -d "$tree" ]; then
    echo "usage: $0 [--revert] /path/to/slang-shaders" >&2
    exit 2
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
patches="$here/patches"
applied=0 skipped=0 failed=0

# Forward is "apply if it would apply, skip if already there"; --revert is the same test the other
# way round.
if [ "$revert" -eq 1 ]; then
    forward="-R"
    check=""
    did="reverted"
    already="not applied"
else
    forward=""
    check="-R"
    did="applied"
    already="already"
fi

for p in "$patches"/*.patch; do
    [ -e "$p" ] || continue
    name="$(basename "$p")"
    if patch -d "$tree" -p1 $forward --dry-run --silent --force < "$p" >/dev/null 2>&1; then
        patch -d "$tree" -p1 $forward --silent --force < "$p" >/dev/null 2>&1
        echo "$did  $name"
        applied=$((applied + 1))
    elif patch -d "$tree" -p1 $check --dry-run --silent --force < "$p" >/dev/null 2>&1; then
        echo "$already  $name"
        skipped=$((skipped + 1))
    else
        echo "FAILED   $name (the shader has changed upstream; check whether it still needs this)" >&2
        failed=$((failed + 1))
    fi
done

echo "$applied $did, $skipped $already, $failed failed"
[ "$failed" -eq 0 ]
