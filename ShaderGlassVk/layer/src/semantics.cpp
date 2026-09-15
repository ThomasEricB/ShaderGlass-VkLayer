/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "semantics.h"

#include <algorithm>
#include <cstdlib>

namespace shaderglass {

namespace {

// "OriginalHistory3" -> prefix "OriginalHistory", index 3. The whole remainder has to be digits, so
// OriginalHistorySize does not read as OriginalHistory with a junk index.
bool IndexedBy(const std::string& name, const char* prefix, uint32_t* index) {
    const size_t len = std::char_traits<char>::length(prefix);
    if (name.size() <= len || name.compare(0, len, prefix) != 0) return false;

    uint32_t value = 0;
    for (size_t i = len; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10 + uint32_t(name[i] - '0');
    }
    *index = value;
    return true;
}

}  // namespace

ScaleType ParseScaleType(const std::string& value, ScaleType fallback) {
    if (value == "source") return ScaleType::kSource;
    if (value == "viewport") return ScaleType::kViewport;
    if (value == "absolute") return ScaleType::kAbsolute;
    return fallback;
}

WrapMode ParseWrapMode(const std::string& value, WrapMode fallback) {
    if (value == "clamp_to_edge") return WrapMode::kClampToEdge;
    if (value == "clamp_to_border") return WrapMode::kClampToBorder;
    if (value == "repeat") return WrapMode::kRepeat;
    if (value == "mirrored_repeat") return WrapMode::kMirroredRepeat;
    return fallback;
}

bool ParseBool(const std::string& value, bool fallback) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

TextureRef ClassifyTexture(const std::string& name) {
    TextureRef ref;
    ref.name = name;

    if (name == "Original") {
        ref.semantic = TextureSemantic::kOriginal;
        return ref;
    }
    if (name == "Source") {
        ref.semantic = TextureSemantic::kSource;
        return ref;
    }
    if (IndexedBy(name, "OriginalHistory", &ref.index)) {
        // OriginalHistory0 is Original itself, which is what the spec says and what presets assume.
        ref.semantic = ref.index == 0 ? TextureSemantic::kOriginal : TextureSemantic::kHistory;
        return ref;
    }
    if (IndexedBy(name, "PassFeedback", &ref.index)) {
        ref.semantic = TextureSemantic::kPassFeedback;
        return ref;
    }
    if (IndexedBy(name, "PassOutput", &ref.index)) {
        ref.semantic = TextureSemantic::kPassOutput;
        return ref;
    }
    if (IndexedBy(name, "User", &ref.index)) {
        ref.semantic = TextureSemantic::kUser;
        return ref;
    }

    // "OriginalFeedback" is the previous frame's Original, which is what OriginalHistory1 already
    // means -- the same image under the older of the two names.
    if (name == "OriginalFeedback") {
        ref.semantic = TextureSemantic::kHistory;
        ref.index = 1;
        return ref;
    }

    // A pass alias with "Feedback" on the end asks for that pass's previous frame. Sixty-odd
    // shaders in the libretro tree do this -- AfterglowPassFeedback, AvgLumPassFeedback -- and
    // without it they silently read the current frame, which is a feedback loop with no delay.
    static const std::string kFeedback = "Feedback";
    if (name.size() > kFeedback.size() &&
        name.compare(name.size() - kFeedback.size(), kFeedback.size(), kFeedback) == 0) {
        ref.semantic = TextureSemantic::kAliasFeedback;
        ref.name = name.substr(0, name.size() - kFeedback.size());
        return ref;
    }

    // Anything else names a preset texture or a pass alias. Which of the two it is depends on the
    // preset, not on the name, so the chain decides.
    ref.semantic = TextureSemantic::kLut;
    return ref;
}

UniformRef ClassifyUniform(const std::string& name) {
    UniformRef ref;

    if (name == "MVP") {
        ref.semantic = UniformSemantic::kMvp;
        return ref;
    }
    if (name == "FrameCount") {
        ref.semantic = UniformSemantic::kFrameCount;
        return ref;
    }
    if (name == "FrameDirection") {
        ref.semantic = UniformSemantic::kFrameDirection;
        return ref;
    }
    if (name == "Rotation") {
        ref.semantic = UniformSemantic::kRotation;
        return ref;
    }
    if (name == "FrameTimeDelta") {
        ref.semantic = UniformSemantic::kFrameTimeDelta;
        return ref;
    }
    if (name == "OriginalAspect") {
        ref.semantic = UniformSemantic::kOriginalAspect;
        return ref;
    }
    if (name == "OriginalAspectRotated") {
        ref.semantic = UniformSemantic::kOriginalAspectRotated;
        return ref;
    }
    if (name == "TotalSubFrames") {
        ref.semantic = UniformSemantic::kTotalSubFrames;
        return ref;
    }
    if (name == "CurrentSubFrame") {
        ref.semantic = UniformSemantic::kCurrentSubFrame;
        return ref;
    }
    if (name == "OutputSize") {
        ref.semantic = UniformSemantic::kOutputSize;
        return ref;
    }
    if (name == "FinalViewportSize") {
        ref.semantic = UniformSemantic::kFinalViewportSize;
        return ref;
    }

    // "<Something>Size" sizes a texture. The catch is that a shader parameter is free to end in
    // Size too, so this only claims the name when the stem resolves to a texture semantic the
    // preset can actually satisfy -- a structural one here, and a declared LUT or alias at the
    // chain, which re-checks kLut against what the preset declared.
    static const std::string kSize = "Size";
    if (name.size() > kSize.size() &&
        name.compare(name.size() - kSize.size(), kSize.size(), kSize) == 0) {
        ref.texture = ClassifyTexture(name.substr(0, name.size() - kSize.size()));
        ref.semantic = UniformSemantic::kTextureSize;
        return ref;
    }

    ref.semantic = UniformSemantic::kParameter;
    return ref;
}

void ScaledSize(ScaleType typeX, float scaleX, ScaleType typeY, float scaleY, uint32_t inputW,
                uint32_t inputH, uint32_t viewportW, uint32_t viewportH, uint32_t* outW,
                uint32_t* outH) {
    const auto apply = [](ScaleType type, float scale, uint32_t input, uint32_t viewport) {
        float value = 0.0f;
        switch (type) {
            case ScaleType::kSource: value = float(input) * scale; break;
            case ScaleType::kViewport: value = float(viewport) * scale; break;
            case ScaleType::kAbsolute: value = scale; break;
        }
        // A pass with no pixels cannot be rendered, and a preset that asks for one is asking for a
        // render pass Vulkan will refuse, so the floor is 1 rather than an error.
        if (!(value >= 1.0f)) return 1u;
        // Far beyond any real display, but a preset with a bad absolute value should not try to
        // allocate its way out of memory.
        if (value > 16384.0f) return 16384u;
        return uint32_t(value);
    };

    *outW = apply(typeX, scaleX, inputW, viewportW);
    *outH = apply(typeY, scaleY, inputH, viewportH);
}

}  // namespace shaderglass
