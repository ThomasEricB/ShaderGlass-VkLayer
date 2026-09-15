/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

The libretro preset vocabulary: what a preset's keys mean, and what a shader's reflected names refer
to.

A .slangp is a flat list of strings -- scale_type0 = "source", filter_linear2 = "true",
alias3 = "PrePass". A .slang declares a uniform block and a set of samplers whose *names* carry the
meaning: a sampler called Original wants the frame as the game presented it, one called
PassFeedback2 wants pass 2's output from the previous frame, and a vec4 called SourceSize wants
(w, h, 1/w, 1/h) of whatever the pass is reading.

Nothing here touches Vulkan. It is all string in, enum out, which is what makes it the part of the
pass model that can be tested without a device -- see tests/semantics_test.cpp.
*/

#pragma once

#include <cstdint>
#include <string>

namespace shaderglass {

// How a pass's output size is derived, per axis. libretro allows the two axes to differ, and
// presets use that: a pass that works per scanline is often "source" vertically and "viewport"
// horizontally.
enum class ScaleType {
    kSource,    // multiply what this pass reads
    kViewport,  // multiply the final output
    kAbsolute,  // a pixel count, ignoring both
};

enum class WrapMode {
    kClampToEdge,
    kClampToBorder,
    kRepeat,
    kMirroredRepeat,
};

// What a sampler name in a .slang refers to.
enum class TextureSemantic {
    kUnknown,
    kOriginal,     // the frame as presented, at the source raster
    kSource,       // what this pass reads: the previous pass, or Original for pass 0
    kHistory,      // OriginalHistory#: Original as it was # frames ago
    kPassOutput,   // PassOutput#: an earlier pass's output, this frame
    kPassFeedback, // PassFeedback#: a pass's own output from the previous frame
    kUser,         // User#: a preset texture by index
    kAliasFeedback,// <Alias>Feedback: the previous frame's output of the pass aliased <Alias>
    kLut,          // a preset texture by name, including a pass alias
};

// What a uniform member name refers to. Anything unrecognised is a shader parameter, which is the
// large majority of them.
enum class UniformSemantic {
    kParameter,
    kMvp,
    kFrameCount,
    kFrameDirection,
    kRotation,       // 0..3, quarter turns of the output
    kFrameTimeDelta, // microseconds since the previous frame
    kOriginalAspect, // Original's width over its height
    kOriginalAspectRotated,
    kTotalSubFrames, // subframe rendering, which this layer does not do: always 1 of 1
    kCurrentSubFrame,
    kOutputSize,
    kFinalViewportSize,
    kTextureSize,  // <Something>Size, where Something is a texture semantic
};

// A resolved sampler name.
struct TextureRef {
    TextureSemantic semantic = TextureSemantic::kUnknown;
    uint32_t index = 0;   // the # in OriginalHistory#, PassOutput#, PassFeedback#, User#
    std::string name;     // for kLut, the name as written
};

// A resolved uniform member name. For kTextureSize the texture it sizes is in `texture`.
struct UniformRef {
    UniformSemantic semantic = UniformSemantic::kParameter;
    TextureRef texture;
};

ScaleType ParseScaleType(const std::string& value, ScaleType fallback);
WrapMode ParseWrapMode(const std::string& value, WrapMode fallback);
bool ParseBool(const std::string& value, bool fallback);

// Classify a sampler name. Names that match nothing structural are LUTs -- a preset texture or a
// pass alias -- which the chain resolves against what the preset actually declared.
TextureRef ClassifyTexture(const std::string& name);

// Classify a uniform block member name.
UniformRef ClassifyUniform(const std::string& name);

// The size a pass writes, given its scale settings, what it reads, and the final output.
void ScaledSize(ScaleType typeX, float scaleX, ScaleType typeY, float scaleY, uint32_t inputW,
                uint32_t inputH, uint32_t viewportW, uint32_t viewportH, uint32_t* outW,
                uint32_t* outH);

}  // namespace shaderglass
