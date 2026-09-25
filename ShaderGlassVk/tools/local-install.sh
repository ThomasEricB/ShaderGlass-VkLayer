#!/bin/bash
# ShaderGlassVk: ShaderGlass on a Vulkan layer
# Copyright (C) 2021-2025 mausimus (mausimus.net)
# Copyright (C) 2026 Thomas Eric, bmitch87
# GNU General Public License v3.0
#
# Put the layer manifests where the Vulkan loader looks, so that running a game needs SHADERGLASS=1
# and nothing else.
#
# The manifests point at the build tree rather than copying anything out of it, which is what makes
# a rebuild take effect without reinstalling. The catalogue is not named here at all: the layer finds
# it relative to its own library (see CandidatePaths in layer/src/catalogue.cpp).
#
#   tools/local-install.sh              install for this user
#   tools/local-install.sh --uninstall  remove it again
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
manifest_dir="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
envd_dir="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d"
# After dlssnr.conf in lexical order, which is the order systemd reads these in, so the value it set
# is already there to be appended to.
envd_file="$envd_dir/zz-shaderglass.conf"

install_one() { # name, library, manifest file
    local name=$1 lib=$2 file=$3
    if [ ! -f "$lib" ]; then
        echo "  skipping $name: $lib not built"
        return
    fi
    # Written to a temporary name and renamed into place. A manifest read while half-written is a
    # layer the loader silently drops, and the loader may be reading this the moment a game starts.
    cat > "$manifest_dir/$file.new.$$" <<JSON
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "$name",
    "type": "GLOBAL",
    "library_path": "$lib",
    "api_version": "1.3.277",
    "implementation_version": "1",
    "description": "ShaderGlass shader effect layer",
    "enable_environment": { "SHADERGLASS": "1" },
    "disable_environment": { "SHADERGLASS_DISABLE": "1" }
  }
}
JSON
    mv -f "$manifest_dir/$file.new.$$" "$manifest_dir/$file"
    echo "  $name -> $lib"
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$manifest_dir/VK_LAYER_SHADERGLASS.x86_64.json" \
          "$manifest_dir/VK_LAYER_SHADERGLASS_32.i686.json" \
          "$envd_file"
    echo "removed from $manifest_dir and $envd_dir (takes effect at next login)"
    exit 0
fi

mkdir -p "$manifest_dir"
echo "installing to $manifest_dir"
install_one VK_LAYER_SHADERGLASS \
            "$root/build/native/layer/libVkLayer_ShaderGlass.so" \
            VK_LAYER_SHADERGLASS.x86_64.json
install_one VK_LAYER_SHADERGLASS_32 \
            "$root/build/linux32/layer/libVkLayer_ShaderGlass_32.so" \
            VK_LAYER_SHADERGLASS_32.i686.json

# The layer is also named in the session's VK_INSTANCE_LAYERS, after whatever is already there.
#
# That is what puts it below DLSS5VKLayer. DLSS5VKLayer's own installer names it in
# VK_INSTANCE_LAYERS, which makes it an explicitly enabled layer in every process -- and the loader
# places explicit layers below implicit ones. An implicit ShaderGlass therefore always sat above it and
# composed first, leaving DLSS to reconstruct an image that had already been shadered. Named after it
# in the same list, ShaderGlass is explicit too and comes later, so it composes last and the shader
# lands on the finished picture. See docs/DLSS5VKLayer.md for how that was established.
#
# Both architectures' names are listed. A process loads the one matching its own word size and
# skips the other without complaint -- checked, not assumed.
#
# Being named here loads the layer into every Vulkan program in the session. It stays idle and says
# nothing unless SHADERGLASS=1 is set in that program, which is what keeps the one-variable switch.
mkdir -p "$envd_dir"
printf '%s\n' \
    '# Written by ShaderGlassVk tools/local-install.sh. Appends to anything set earlier (DLSS5VKLayer' \
    '# sets this in dlssnr.conf), so ShaderGlass lands below it in the layer chain.' \
    'VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:+${VK_INSTANCE_LAYERS}:}VK_LAYER_SHADERGLASS:VK_LAYER_SHADERGLASS_32"' \
    > "$envd_file.new.$$"
mv -f "$envd_file.new.$$" "$envd_file"
echo "  session layer order -> $envd_file"

cat <<EOM

The layer order takes effect at your next login: environment.d is read once, when the session
starts, and programs already running (Steam among them) keep what they started with.

Launch options are now just:

  SHADERGLASS=1 %command%

Optional, and only when you want them:
  SHADERGLASS_VERBOSE=1        a line per presented frame
  SHADERGLASS_LOG=/tmp/sg.log  send the log to a file instead of stderr
  SHADERGLASS_DISABLE=1        keep the layer out of one game
EOM
