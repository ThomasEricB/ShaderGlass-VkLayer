/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Preset textures: LUTs, bezel art, overlays.

The catalogue carries these as the bytes that sit on disk -- PNG and JPEG -- rather than as decoded
pixels, which is the same choice the Windows app makes, and for the same reason: over the libretro
tree the files total 43 MiB compressed and 1.19 GiB as raw RGBA, so decoding at build time would
make the catalogue about six times larger than the rest of it put together. Upstream decodes with
WIC at load time; this decodes with stb_image, which is header-only and so adds nothing to what the
layer links against -- worth caring about for a library mapped into every game on the system.

Without stb_image the layer still builds and every other part of a preset still runs; presets that
sample a texture lose the texture. DecodeImageReason() says which case you are in.
*/

#pragma once

#include <cstdint>
#include <vector>

namespace shaderglass {

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // tightly packed, four bytes a pixel
};

// Decodes PNG or JPEG from memory into RGBA8. False on anything it cannot read.
bool DecodeImage(const uint8_t* bytes, size_t length, DecodedImage& out);

// Why the last decode failed, or why decoding is unavailable at all.
const char* DecodeImageReason();

}  // namespace shaderglass
