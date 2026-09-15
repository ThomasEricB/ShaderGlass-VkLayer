/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Ported from ShaderGC (mausimus), the .slangp compiler ShaderGlass already had.

What is kept: the whole front end. Preset parsing with #reference following, key/value resolution
with per-shader suffixes, .slang loading with #include following, stage splitting on #pragma stage,
#pragma parameter declarations, #pragma format, texture collection, and the leftover-keys-are-
parameter-overrides rule.

What is gone: the SPIRV-Cross-to-HLSL step and the D3D compiler behind it. Those existed only to
feed a D3D11 renderer; the Vulkan layer wants the SPIR-V glslang already produced.
*/

#pragma once

#include "common.h"

#include <memory>

namespace shaderglass {

// One shader parameter, as declared by "#pragma parameter" and then located by reflection.
struct GenParam {
    GenParam() = default;

    // Parses the declaration form: #pragma parameter NAME "Description" default min max [step]
    GenParam(const std::string& pragma, int size, int buffer);

    int buffer = 0;  // negative: push constant. 0 and up: a UBO binding
    int declIndex = -1;
    int size = 0;
    int offset = 0;
    std::string name;
    std::string desc;
    float min = 0.0f;
    float max = 0.0f;
    float def = 0.0f;
    float step = 0.0f;
};

struct GenSampler {
    std::string name;
    int binding = 0;
};

// One compiled .slang: the two SPIR-V stages and what reflection found in them.
//
// Shared, not copied. 3379 presets are backed by 1463 unique .slang files, and the Mega Bezel
// family alone has hundreds of variants over one set of passes -- so a preset refers to a shader
// rather than carrying it. That is how the Windows app's catalogue is arranged too: one
// <Shader>ShaderDef.h per unique shader, referenced by every preset that uses it.
struct GenShaderData {
    std::string key;  // the canonical source path, which is what makes it unique
    std::string name;
    std::string format;  // from #pragma format
    std::string alias;   // from #pragma name
    std::vector<uint32_t> vertex;
    std::vector<uint32_t> fragment;
    std::vector<GenParam> params;
    std::vector<GenSampler> samplers;

    // Assigned by the emitter: the symbol this shader is emitted under.
    std::string symbol;
};

// One pass within a preset: which shader it runs, and the keys the preset set for this position in
// the chain (scale_type, filter_linear, srgb_framebuffer and the rest).
struct GenPass {
    std::shared_ptr<GenShaderData> shader;
    std::map<std::string, std::string> presetParams;
};

// A preset texture's bytes -- a LUT, a bezel, an overlay -- embedded exactly as they sit on disk.
// Nothing here decodes them, which is why the generator needs no image library. Shared by content,
// because the same bezel art is referenced by hundreds of presets.
struct GenTextureData {
    std::string key;  // content hash
    std::vector<uint8_t> data;
    std::string symbol;
};

// One texture as a preset refers to it: the name the shaders sample it by, its bytes, and its keys.
struct GenTexture {
    std::string name;
    std::shared_ptr<GenTextureData> data;
    std::map<std::string, std::string> presetParams;
};

struct GenOverride {
    std::string name;
    float value = 0.0f;
};

struct GenPreset {
    std::string id;        // catalogue id: the path within the shader tree, without extension
    std::string name;      // the preset's file name
    std::string category;  // its top-level folder
    std::vector<GenPass> passes;
    std::vector<GenTexture> textures;
    std::vector<GenOverride> overrides;
};

// Everything compiled so far, so a shader or a texture shared between presets is compiled, hashed
// and emitted exactly once. Pass the same registry across a whole tree.
struct Registry {
    std::map<std::string, std::shared_ptr<GenShaderData>> shaders;   // by canonical source path
    std::map<std::string, std::shared_ptr<GenTextureData>> textures;  // by content hash

    // How much work the sharing saved, for the generator's summary.
    size_t shaderRequests = 0;
    size_t textureRequests = 0;
};

// Compile a .slangp preset, or a bare .slang treated as a one-pass preset.
GenPreset CompilePreset(const std::filesystem::path& input, Registry& registry, std::ostream& log,
                        bool& warn);

// The pieces, exposed for testing and for the --reflect-check path.
std::vector<std::string> LoadSource(const std::filesystem::path& input, bool followIncludes);
void ParsePreset(const std::filesystem::path& input, std::map<std::string, std::string>& keyValues,
                 std::map<std::string, std::filesystem::path>& valuePaths);

}  // namespace shaderglass
