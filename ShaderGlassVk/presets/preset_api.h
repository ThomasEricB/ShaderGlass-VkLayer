/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

The contract between the generated preset catalogue and whoever loads it.

The catalogue is generated C compiled into libShaderGlassPresets.so -- the same model the Windows app
uses, where every shader is a byte array in the binary, except that the bytes are SPIR-V rather than
DXBC. It lives in its own shared object rather than in the layer because the layer is mapped into
every game that has ShaderGlass armed, and a game with no preset selected should pay nothing for a
catalogue it is not using. The layer dlopens it on the first frame a preset is actually chosen.

A plain C ABI on purpose: dlopen plus four symbols, no name mangling, no C++ objects crossing the
boundary, and everything const and static so there is nothing to free and no initialisation order to
get wrong.
*/

#ifndef SHADERGLASS_PRESET_API_H
#define SHADERGLASS_PRESET_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes shape. The loader refuses a catalogue it does not
// recognise rather than reading a struct laid out differently.
#define SG_PRESET_ABI_VERSION 2

// A preset key that applies to one pass or one texture: "scale_type" -> "viewport",
// "filter_linear" -> "true", and the rest of the libretro vocabulary. Kept as strings because that
// is what the preset format is, and because the runtime is the right place to interpret them.
typedef struct SgKeyValue {
    const char* key;
    const char* value;
} SgKeyValue;

// One shader parameter: where it lives in the uniform block, and what the shader declared about it.
//
// `buffer` follows ShaderGC's convention -- 0 and above is a UBO binding, negative is a push
// constant block. `offset` and `size` are bytes within that block, taken from reflection, which is
// what lets the runtime write a value without knowing anything about the shader's source.
typedef struct SgParam {
    const char* name;
    const char* description;
    int32_t buffer;
    int32_t offset;
    int32_t size;
    float min_value;
    float max_value;
    float default_value;
    float step_value;
} SgParam;

typedef struct SgSampler {
    const char* name;
    int32_t binding;
} SgSampler;

// One compiled .slang, shared by every pass that runs it.
//
// The sharing is in the format on purpose. 3379 presets are backed by 1463 unique shaders, and the
// Mega Bezel family has hundreds of variants over one set of passes -- carrying the SPIR-V per pass
// rather than per shader multiplies the catalogue by an order of magnitude. The Windows app is built
// the same way: one <Shader>ShaderDef.h per unique shader, referenced by every preset using it.
typedef struct SgShader {
    const char* name;

    // SPIR-V, as word arrays -- the units vkCreateShaderModule wants.
    const uint32_t* vertex_spirv;
    size_t vertex_words;
    const uint32_t* fragment_spirv;
    size_t fragment_words;

    const SgParam* params;
    size_t param_count;

    const SgSampler* samplers;
    size_t sampler_count;

    // From "#pragma format" in the .slang; empty when the shader did not ask for one.
    const char* format;
} SgShader;

// One pass: which shader runs at this position in the chain, and the keys the preset set for it.
typedef struct SgPass {
    const SgShader* shader;

    // scale_type, scale, filter_linear, srgb_framebuffer, alias, wrap_mode, frame_count_mod, ...
    const SgKeyValue* preset_params;
    size_t preset_param_count;
} SgPass;

// A texture's bytes, embedded exactly as they sit on disk and shared by content -- one set of bezel
// art backs hundreds of presets. The generator does not decode images, so the runtime is what turns
// these bytes into a Vulkan image.
typedef struct SgTextureData {
    const uint8_t* data;
    size_t length;
} SgTextureData;

// A texture as a preset refers to it: the name its shaders sample it by, its bytes, and its keys.
typedef struct SgTexture {
    const char* name;
    const SgTextureData* data;
    const SgKeyValue* preset_params;
    size_t preset_param_count;
} SgTexture;

typedef struct SgOverride {
    const char* name;
    float value;
} SgOverride;

typedef struct SgPreset {
    const char* id;        // catalogue id, e.g. "crt/crt-geom"
    const char* name;      // the preset's own file name
    const char* category;  // its top-level folder, e.g. "crt"

    const SgPass* passes;
    size_t pass_count;

    const SgTexture* textures;
    size_t texture_count;

    const SgOverride* overrides;
    size_t override_count;
} SgPreset;

// The four symbols the loader resolves.
uint32_t SgPresetAbiVersion(void);
size_t SgPresetCount(void);
const SgPreset* SgPresetAt(size_t index);
const SgPreset* SgFindPreset(const char* id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // SHADERGLASS_PRESET_API_H
