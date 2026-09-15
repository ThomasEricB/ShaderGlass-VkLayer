/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The libretro vocabulary, checked without a device.

Most of what can go wrong in the pass model is a name read the wrong way -- OriginalHistorySize
taken for a history texture, PassFeedback2 bound to pass 2's current output instead of its previous
one, a scale_type_y that silently inherits the wrong default. None of that needs a GPU to catch, so
it is tested here rather than only in a game.
*/

#include "../layer/src/semantics.h"

#include <cstdio>
#include <string>

using namespace shaderglass;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

void CheckTexture(const char* name, TextureSemantic semantic, uint32_t index) {
    const TextureRef ref = ClassifyTexture(name);
    if (ref.semantic != semantic || ref.index != index) {
        std::printf("  FAIL  texture '%s': got semantic %d index %u\n", name, (int) ref.semantic,
                    ref.index);
        ++g_failures;
    }
}

void CheckUniform(const char* name, UniformSemantic semantic) {
    const UniformRef ref = ClassifyUniform(name);
    if (ref.semantic != semantic) {
        std::printf("  FAIL  uniform '%s': got semantic %d\n", name, (int) ref.semantic);
        ++g_failures;
    }
}

}  // namespace

int main() {
    // --- textures ---
    CheckTexture("Original", TextureSemantic::kOriginal, 0);
    CheckTexture("Source", TextureSemantic::kSource, 0);

    // OriginalHistory0 is Original itself, which is what the spec says and what presets assume.
    CheckTexture("OriginalHistory0", TextureSemantic::kOriginal, 0);
    CheckTexture("OriginalHistory1", TextureSemantic::kHistory, 1);
    CheckTexture("OriginalHistory7", TextureSemantic::kHistory, 7);

    CheckTexture("PassOutput0", TextureSemantic::kPassOutput, 0);
    CheckTexture("PassOutput12", TextureSemantic::kPassOutput, 12);
    CheckTexture("PassFeedback3", TextureSemantic::kPassFeedback, 3);
    CheckTexture("User2", TextureSemantic::kUser, 2);

    // A preset texture or a pass alias; the name alone does not say which, so both are kLut and the
    // chain decides.
    CheckTexture("SamplerLUT1", TextureSemantic::kLut, 0);
    CheckTexture("bezel", TextureSemantic::kLut, 0);

    // The trap: a prefix match that is not an index must not be read as one.
    CheckTexture("PassOutputSize", TextureSemantic::kLut, 0);
    CheckTexture("OriginalHistory", TextureSemantic::kLut, 0);
    CheckTexture("OriginalSize", TextureSemantic::kLut, 0);
    CheckTexture("PassFeedback1x", TextureSemantic::kLut, 0);

    // A pass alias with "Feedback" on the end is that pass's previous frame, and the stem is the
    // alias to look it up by. Sixty-odd libretro shaders are written this way.
    {
        const TextureRef ref = ClassifyTexture("AfterglowPassFeedback");
        Check(ref.semantic == TextureSemantic::kAliasFeedback, "AfterglowPassFeedback is alias feedback");
        Check(ref.name == "AfterglowPass", "...of the pass aliased AfterglowPass");
    }

    // "OriginalFeedback" is the previous frame's Original, which OriginalHistory1 already names.
    CheckTexture("OriginalFeedback", TextureSemantic::kHistory, 1);

    // PassFeedback# is indexed, and must not be mistaken for the alias form.
    {
        const TextureRef ref = ClassifyTexture("PassFeedback2");
        Check(ref.semantic == TextureSemantic::kPassFeedback, "PassFeedback2 stays indexed");
        Check(ref.index == 2, "...at index 2");
    }

    // --- uniforms ---
    CheckUniform("MVP", UniformSemantic::kMvp);
    CheckUniform("FrameCount", UniformSemantic::kFrameCount);
    CheckUniform("FrameDirection", UniformSemantic::kFrameDirection);
    CheckUniform("OutputSize", UniformSemantic::kOutputSize);
    CheckUniform("FinalViewportSize", UniformSemantic::kFinalViewportSize);
    CheckUniform("Rotation", UniformSemantic::kRotation);
    CheckUniform("FrameTimeDelta", UniformSemantic::kFrameTimeDelta);
    CheckUniform("OriginalFPS", UniformSemantic::kOriginalFPS);
    CheckUniform("OriginalAspect", UniformSemantic::kOriginalAspect);
    CheckUniform("OriginalAspectRotated", UniformSemantic::kOriginalAspectRotated);
    CheckUniform("TotalSubFrames", UniformSemantic::kTotalSubFrames);
    CheckUniform("CurrentSubFrame", UniformSemantic::kCurrentSubFrame);
    CheckUniform("SourceSize", UniformSemantic::kTextureSize);
    CheckUniform("OriginalSize", UniformSemantic::kTextureSize);
    CheckUniform("PassOutput2Size", UniformSemantic::kTextureSize);

    // Shader parameters, which is what most members are.
    CheckUniform("CRTgamma", UniformSemantic::kParameter);
    CheckUniform("beam_min", UniformSemantic::kParameter);

    // A feedback alias's size follows the same route: strip "Size", then classify the stem.
    {
        const UniformRef ref = ClassifyUniform("AvgLumPassFeedbackSize");
        Check(ref.semantic == UniformSemantic::kTextureSize, "AvgLumPassFeedbackSize is a texture size");
        Check(ref.texture.semantic == TextureSemantic::kAliasFeedback, "...of an alias feedback");
        Check(ref.texture.name == "AvgLumPass", "...named AvgLumPass");
    }

    // "<X>Size" names a texture, and the stem is classified in its own right.
    {
        const UniformRef ref = ClassifyUniform("OriginalHistory2Size");
        Check(ref.semantic == UniformSemantic::kTextureSize, "OriginalHistory2Size is a texture size");
        Check(ref.texture.semantic == TextureSemantic::kHistory, "...of a history texture");
        Check(ref.texture.index == 2, "...at index 2");
    }

    // --- preset values ---
    Check(ParseScaleType("source", ScaleType::kViewport) == ScaleType::kSource, "scale_type source");
    Check(ParseScaleType("viewport", ScaleType::kSource) == ScaleType::kViewport,
          "scale_type viewport");
    Check(ParseScaleType("absolute", ScaleType::kSource) == ScaleType::kAbsolute,
          "scale_type absolute");
    Check(ParseScaleType("", ScaleType::kViewport) == ScaleType::kViewport,
          "an absent scale_type keeps the fallback");

    Check(ParseWrapMode("mirrored_repeat", WrapMode::kClampToEdge) == WrapMode::kMirroredRepeat,
          "wrap_mode mirrored_repeat");
    Check(ParseWrapMode("nonsense", WrapMode::kRepeat) == WrapMode::kRepeat,
          "an unknown wrap_mode keeps the fallback");

    Check(ParseBool("true", false), "filter_linear true");
    Check(!ParseBool("false", true), "filter_linear false");
    Check(ParseBool("1", false), "a numeric true");
    Check(ParseBool("", true), "an absent bool keeps the fallback");

    // --- sizes ---
    {
        uint32_t w = 0, h = 0;

        // The common shape: one axis follows the source, the other the viewport.
        ScaledSize(ScaleType::kSource, 1.0f, ScaleType::kViewport, 1.0f, 320, 240, 1920, 1080, &w, &h);
        Check(w == 320 && h == 1080, "source x1 by viewport x1");

        ScaledSize(ScaleType::kSource, 2.0f, ScaleType::kSource, 2.0f, 320, 240, 1920, 1080, &w, &h);
        Check(w == 640 && h == 480, "source x2");

        ScaledSize(ScaleType::kAbsolute, 512.0f, ScaleType::kAbsolute, 256.0f, 320, 240, 1920, 1080,
                   &w, &h);
        Check(w == 512 && h == 256, "absolute");

        // A pass with no pixels is a render pass Vulkan refuses, so the floor is 1.
        ScaledSize(ScaleType::kSource, 0.0f, ScaleType::kSource, 0.01f, 320, 240, 1920, 1080, &w, &h);
        Check(w == 1 && h == 2, "a scale that rounds to nothing still has a pixel");

        // And a preset with a bad absolute value should not allocate its way out of memory.
        ScaledSize(ScaleType::kAbsolute, 1e9f, ScaleType::kAbsolute, 1e9f, 320, 240, 1920, 1080, &w,
                   &h);
        Check(w == 16384 && h == 16384, "an absurd size is capped");
    }

    if (g_failures) {
        std::printf("\n%d check%s failed\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("semantics: all checks passed\n");
    return 0;
}
