/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "texture.h"

#ifdef SG_HAVE_STB_IMAGE

// stb_image brings in a lot of symbols. They are all static here, and the layer's version script
// exports only vkNegotiateLoaderLayerInterfaceVersion and the two GetProcAddr entry points, so none
// of it can collide with a game that has its own copy -- which, for an image decoder, many do.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include SG_STB_IMAGE_HEADER

#include <cstring>

namespace shaderglass {

namespace {
const char* g_reason = "";
}

bool DecodeImage(const uint8_t* bytes, size_t length, DecodedImage& out) {
    if (!bytes || !length) {
        g_reason = "the texture is empty";
        return false;
    }
    if (length > size_t(INT32_MAX)) {
        g_reason = "the texture is too large to decode";
        return false;
    }

    int w = 0, h = 0, channels = 0;
    // Four channels always, so the upload is one format and the shader samples RGBA whatever the
    // file happened to store.
    stbi_uc* pixels = stbi_load_from_memory(bytes, int(length), &w, &h, &channels, 4);
    if (!pixels) {
        g_reason = "not a PNG or JPEG this build can read";
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        g_reason = "the texture has no pixels";
        return false;
    }

    out.width = uint32_t(w);
    out.height = uint32_t(h);
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    std::memcpy(out.rgba.data(), pixels, out.rgba.size());
    stbi_image_free(pixels);

    g_reason = "";
    return true;
}

const char* DecodeImageReason() { return g_reason; }

}  // namespace shaderglass

#else  // SG_HAVE_STB_IMAGE

namespace shaderglass {

bool DecodeImage(const uint8_t*, size_t, DecodedImage&) { return false; }

const char* DecodeImageReason() {
    return "this build has no image decoder (stb_image was not found at build time)";
}

}  // namespace shaderglass

#endif  // SG_HAVE_STB_IMAGE
