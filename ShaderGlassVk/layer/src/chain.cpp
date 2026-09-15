/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "chain.h"
#include "catalogue.h"
#include "texture.h"
#include "shaders/passthrough_spv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace shaderglass {

namespace {

// The unit quad, in the layout libretro passes expect: a vec4 position and a vec2 texture
// coordinate, six floats a vertex, drawn as a triangle strip.
//
// Positions run 0..1 and the MVP maps them into clip space. Vulkan's NDC has y pointing down and the
// texture origin is top-left, so position y and texture v agree and neither needs flipping -- the
// vertical flip that catches ports of GL code out does not arise here.
struct Vertex {
    float x, y, z, w;
    float u, v;
};

const Vertex kQuad[4] = {
    {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},  // top-left
    {1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},  // top-right
    {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},  // bottom-left
    {1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},  // bottom-right
};

// Column-major, mapping the unit quad to clip space: (0,0) -> (-1,-1), (1,1) -> (1,1).
const float kMvp[16] = {
    2.0f, 0.0f, 0.0f, 0.0f,   //
    0.0f, 2.0f, 0.0f, 0.0f,   //
    0.0f, 0.0f, 1.0f, 0.0f,   //
    -1.0f, -1.0f, 0.0f, 1.0f  //
};

// Bytes a pixel, for the formats the chain works in.
uint32_t FormatBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM: return 8u;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16u;
        default: return 4u;
    }
}

bool SelfTestWanted() {
    static const bool v = [] {
        const char* p = getenv("SHADERGLASS_SELFTEST");
        return p && p[0] == '1';
    }();
    return v;
}

void FillSize(float out[4], uint32_t w, uint32_t h) {
    out[0] = float(w);
    out[1] = float(h);
    out[2] = w ? 1.0f / float(w) : 0.0f;
    out[3] = h ? 1.0f / float(h) : 0.0f;
}

VkShaderModule MakeModule(const DeviceTable* vk, VkDevice device, const uint32_t* code,
                          size_t words) {
    VkShaderModuleCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * sizeof(uint32_t);
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vk->vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

// "#pragma format R16G16B16A16_SFLOAT" -- the whole set the libretro spec defines, because the
// numeric type matters and not just the width: a shader that declares R32_UINT samples the result
// through a usampler2D, and handing it a UNORM image is undefined, not merely imprecise.
VkFormat ParsePragmaFormat(const char* text) {
    if (!text || !text[0]) return VK_FORMAT_UNDEFINED;
    const std::string f(text);

    if (f == "R8_UNORM") return VK_FORMAT_R8_UNORM;
    if (f == "R8_UINT") return VK_FORMAT_R8_UINT;
    if (f == "R8_SINT") return VK_FORMAT_R8_SINT;
    if (f == "R8G8_UNORM") return VK_FORMAT_R8G8_UNORM;
    if (f == "R8G8_UINT") return VK_FORMAT_R8G8_UINT;
    if (f == "R8G8_SINT") return VK_FORMAT_R8G8_SINT;
    if (f == "R8G8B8A8_UNORM") return VK_FORMAT_R8G8B8A8_UNORM;
    if (f == "R8G8B8A8_UINT") return VK_FORMAT_R8G8B8A8_UINT;
    if (f == "R8G8B8A8_SINT") return VK_FORMAT_R8G8B8A8_SINT;
    if (f == "R8G8B8A8_SRGB") return VK_FORMAT_R8G8B8A8_SRGB;

    if (f == "A2B10G10R10_UNORM_PACK32") return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    if (f == "A2B10G10R10_UINT_PACK32") return VK_FORMAT_A2B10G10R10_UINT_PACK32;

    if (f == "R16_UINT") return VK_FORMAT_R16_UINT;
    if (f == "R16_SINT") return VK_FORMAT_R16_SINT;
    if (f == "R16_SFLOAT") return VK_FORMAT_R16_SFLOAT;
    if (f == "R16G16_UINT") return VK_FORMAT_R16G16_UINT;
    if (f == "R16G16_SINT") return VK_FORMAT_R16G16_SINT;
    if (f == "R16G16_SFLOAT") return VK_FORMAT_R16G16_SFLOAT;
    if (f == "R16G16B16A16_UNORM") return VK_FORMAT_R16G16B16A16_UNORM;
    if (f == "R16G16B16A16_UINT") return VK_FORMAT_R16G16B16A16_UINT;
    if (f == "R16G16B16A16_SINT") return VK_FORMAT_R16G16B16A16_SINT;
    if (f == "R16G16B16A16_SFLOAT") return VK_FORMAT_R16G16B16A16_SFLOAT;

    if (f == "R32_UINT") return VK_FORMAT_R32_UINT;
    if (f == "R32_SINT") return VK_FORMAT_R32_SINT;
    if (f == "R32_SFLOAT") return VK_FORMAT_R32_SFLOAT;
    if (f == "R32G32_UINT") return VK_FORMAT_R32G32_UINT;
    if (f == "R32G32_SINT") return VK_FORMAT_R32G32_SINT;
    if (f == "R32G32_SFLOAT") return VK_FORMAT_R32G32_SFLOAT;
    if (f == "R32G32B32A32_UINT") return VK_FORMAT_R32G32B32A32_UINT;
    if (f == "R32G32B32A32_SINT") return VK_FORMAT_R32G32B32A32_SINT;
    if (f == "R32G32B32A32_SFLOAT") return VK_FORMAT_R32G32B32A32_SFLOAT;

    return VK_FORMAT_UNDEFINED;
}

// Whether sampling this format yields integers rather than floats. A pass whose format is integral
// can only be read through a usampler2D or isampler2D, so substituting a float format for it is not
// a loss of precision but a type error the shader cannot survive.
bool IntegerFormat(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT: return true;
        default: return false;
    }
}

VkFormat SrgbTwin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        default: return f;
    }
}

VkSamplerAddressMode AddressFor(WrapMode wrap) {
    switch (wrap) {
        case WrapMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case WrapMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case WrapMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case WrapMode::kClampToEdge:
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// A preset's key/value pairs, looked up without building a map per pass.
const char* Lookup(const SgKeyValue* kv, size_t count, const char* key) {
    for (size_t i = 0; i < count; ++i)
        if (std::strcmp(kv[i].key, key) == 0) return kv[i].value;
    return nullptr;
}

std::string LookupStr(const SgKeyValue* kv, size_t count, const char* key) {
    const char* v = Lookup(kv, count, key);
    return v ? std::string(v) : std::string();
}

float LookupFloat(const SgKeyValue* kv, size_t count, const char* key, float fallback) {
    const char* v = Lookup(kv, count, key);
    if (!v || !v[0]) return fallback;
    return float(atof(v));
}

// Samplers are keyed by the only three things that vary here.
uint32_t SamplerKey(bool linear, WrapMode wrap, bool mipmapped) {
    return (linear ? 1u : 0u) | (uint32_t(wrap) << 1) | (mipmapped ? 1u << 4 : 0u);
}

}  // namespace

Chain::Chain(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
             VkPhysicalDevice physical)
    : _vk(vk), _instance(instance), _device(device), _physical(physical) {
    if (!DeviceTableComplete(*vk)) {
        _reason = "the device does not expose everything a shader pass needs";
        Log("[chain] %s", _reason.c_str());
        return;
    }

    VkPhysicalDeviceProperties props {};
    instance->vkGetPhysicalDeviceProperties(physical, &props);
    // Capped at the size of the scratch RecordPass writes into, so that buffer is provably big
    // enough. No libretro shader comes near it; devices that advertise more are not useful here.
    _maxPushConstants = std::min<uint32_t>(props.limits.maxPushConstantsSize, 256u);

    _usable = true;
}

Chain::~Chain() { DropAll(); }

void Chain::DropSized() {
    for (auto& pass : _passes) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (pass.framebuffer[i]) _vk->vkDestroyFramebuffer(_device, pass.framebuffer[i], nullptr);
            pass.framebuffer[i] = VK_NULL_HANDLE;
            if (pass.targetView[i]) _vk->vkDestroyImageView(_device, pass.targetView[i], nullptr);
            pass.targetView[i] = VK_NULL_HANDLE;
            DropImage(_vk, _device, pass.output[i]);
        }
    }
    for (auto& img : _history) DropImage(_vk, _device, img);
    _history.clear();

    DropImage(_vk, _device, _frame);
    DropImage(_vk, _device, _originalScaled);

    DropHostBuffer(_vk, _device, _selfTest);
    _selfTestHalf = 0;
    _selfTestRecorded = false;
}

void Chain::DropAll() {
    DropSized();

    for (auto& pass : _passes) {
        if (pass.pipeline) _vk->vkDestroyPipeline(_device, pass.pipeline, nullptr);
        if (pass.pipelineLayout) _vk->vkDestroyPipelineLayout(_device, pass.pipelineLayout, nullptr);
        if (pass.setLayout) _vk->vkDestroyDescriptorSetLayout(_device, pass.setLayout, nullptr);
        if (pass.renderPass) _vk->vkDestroyRenderPass(_device, pass.renderPass, nullptr);
    }
    _passes.clear();

    for (auto& lut : _luts) DropImage(_vk, _device, lut.image);
    _luts.clear();

    for (auto& kv : _samplers) _vk->vkDestroySampler(_device, kv.second, nullptr);
    _samplers.clear();

    if (_descriptorPool) _vk->vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    _descriptorPool = VK_NULL_HANDLE;

    DropHostBuffer(_vk, _device, _vertices);
    DropHostBuffer(_vk, _device, _uniforms);
    DropHostBuffer(_vk, _device, _lutStaging);
    _lutUploads.clear();

    _swapWidth = _swapHeight = 0;
    _sourceWidth = _sourceHeight = 0;
    _historyDepth = 0;
    _preset = nullptr;
}

VkSampler Chain::SamplerFor(bool linear, WrapMode wrap, bool mipmapped) {
    const uint32_t key = SamplerKey(linear, wrap, mipmapped);
    auto it = _samplers.find(key);
    if (it != _samplers.end()) return it->second;

    VkSamplerCreateInfo sci {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    // Only an image with a chain gets interpolation between levels; on a single-level image
    // LINEAR and NEAREST are the same thing, but saying NEAREST keeps the two cases distinct.
    sci.mipmapMode = mipmapped ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = AddressFor(wrap);
    sci.addressModeV = AddressFor(wrap);
    sci.addressModeW = AddressFor(wrap);
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = VK_LOD_CLAMP_NONE;

    VkSampler sampler = VK_NULL_HANDLE;
    if (_vk->vkCreateSampler(_device, &sci, nullptr, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    _samplers[key] = sampler;
    return sampler;
}

VkFormat Chain::FormatFor(const Pass& pass, bool last) const {
    // The last pass is copied straight into the swapchain, so it renders in the chain's format and
    // at the swapchain's size regardless of what the preset asked for -- which is also what
    // RetroArch does with a final pass.
    if (last) return _chainFormat;

    VkFormat wanted = VK_FORMAT_UNDEFINED;
    if (pass.shader) wanted = ParsePragmaFormat(pass.shader->format);
    if (wanted == VK_FORMAT_UNDEFINED && pass.floatFb) wanted = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (wanted == VK_FORMAT_UNDEFINED && pass.srgb) wanted = SrgbTwin(_chainFormat);
    if (wanted == VK_FORMAT_UNDEFINED) return _chainFormat;

    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!FormatSupports(_instance, _physical, wanted, need)) {
        // Substituting is only safe when the substitute reads back the same way. A float format
        // downgraded to the chain's own loses precision, which the pass survives; an integer one
        // would change what the next shader's sampler returns, which it does not.
        if (IntegerFormat(wanted)) return VK_FORMAT_UNDEFINED;
        Log("[chain] format %d is not renderable here; using the chain format instead", (int) wanted);
        return _chainFormat;
    }
    return wanted;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

bool Chain::BuildPasses(const SgPreset* preset) {
    const size_t count = preset ? preset->pass_count : 1;
    _passes.resize(count);

    for (size_t i = 0; i < count; ++i) {
        Pass& pass = _passes[i];
        pass = Pass {};

        if (!preset) {
            pass.name = "passthrough";
            continue;
        }

        const SgPass& sp = preset->passes[i];
        pass.shader = sp.shader;
        pass.name = sp.shader && sp.shader->name ? sp.shader->name : "pass";

        const SgKeyValue* kv = sp.preset_params;
        const size_t n = sp.preset_param_count;

        // scale_type sets both axes; scale_type_x and scale_type_y override one each. Same for the
        // values. A preset that says nothing means "source, x1", which is the libretro default.
        const ScaleType both = ParseScaleType(LookupStr(kv, n, "scale_type"), ScaleType::kSource);
        pass.scaleTypeX = ParseScaleType(LookupStr(kv, n, "scale_type_x"), both);
        pass.scaleTypeY = ParseScaleType(LookupStr(kv, n, "scale_type_y"), both);

        const float scale = LookupFloat(kv, n, "scale", 1.0f);
        pass.scaleX = LookupFloat(kv, n, "scale_x", scale);
        pass.scaleY = LookupFloat(kv, n, "scale_y", scale);

        pass.filterLinear = ParseBool(LookupStr(kv, n, "filter_linear"), false);
        pass.wrap = ParseWrapMode(LookupStr(kv, n, "wrap_mode"), WrapMode::kClampToEdge);
        pass.srgb = ParseBool(LookupStr(kv, n, "srgb_framebuffer"), false);
        pass.floatFb = ParseBool(LookupStr(kv, n, "float_framebuffer"), false);
        pass.mipmapInput = ParseBool(LookupStr(kv, n, "mipmap_input"), false);
        pass.frameCountMod = uint32_t(LookupFloat(kv, n, "frame_count_mod", 0.0f));
        // Set either by the preset's alias# or, failing that, by the shader's own #pragma name --
        // the generator has already folded the two into one key, in that order of precedence.
        pass.alias = LookupStr(kv, n, "alias");
    }

    // Which passes are read back as feedback, and how far back OriginalHistory reaches. Both are
    // decided by what the shaders sample, not by anything the preset declares, so they can only be
    // known once every pass has been looked at.
    _historyDepth = 0;
    for (const auto& pass : _passes) {
        if (!pass.shader) continue;
        for (size_t s = 0; s < pass.shader->sampler_count; ++s) {
            const TextureRef ref = ClassifyTexture(pass.shader->samplers[s].name);
            if (ref.semantic == TextureSemantic::kPassFeedback && ref.index < _passes.size())
                _passes[ref.index].feedback = true;
            if (ref.semantic == TextureSemantic::kAliasFeedback) {
                // By alias rather than by index, so the pass it names has to be looked up.
                for (auto& other : _passes)
                    if (!other.alias.empty() && other.alias == ref.name) other.feedback = true;
            }
            if (ref.semantic == TextureSemantic::kHistory)
                _historyDepth = std::max(_historyDepth, ref.index);
        }
    }

    // Beyond this a preset is asking for more past frames than it can plausibly use, and each one
    // is a full copy of the source every frame.
    if (_historyDepth > 8) _historyDepth = 8;

    // mipmap_input is declared by the pass that *reads* the chain, so it marks the image before it:
    // pass 0 reads Original, and pass i reads pass i-1's output.
    _originalMipmapped = false;
    for (size_t i = 0; i < _passes.size(); ++i) {
        if (!_passes[i].mipmapInput) continue;
        if (i == 0) _originalMipmapped = true;
        else _passes[i - 1].mipmapped = true;
    }
    return true;
}

// Work out where every uniform member and every sampler of one pass gets its value, once, so
// recording a frame never compares a string.
bool Chain::BuildPassResources(Pass& pass, uint32_t index) {
    const bool last = (index + 1 == _passes.size());

    pass.uniforms.clear();
    pass.samplers.clear();
    pass.uboBinding = -1;
    pass.uboSize = 0;
    pass.pushSize = 0;

    if (pass.shader) {
        for (size_t p = 0; p < pass.shader->param_count; ++p) {
            const SgParam& sp = pass.shader->params[p];

            UniformSlot slot;
            slot.name = sp.name ? sp.name : "";
            const UniformRef ref = ClassifyUniform(slot.name);
            slot.semantic = ref.semantic;
            slot.texture = ref.texture;
            slot.push = sp.buffer < 0;
            slot.offset = uint32_t(sp.offset);
            slot.size = uint32_t(sp.size);
            slot.value = sp.default_value;
            slot.fallback = sp.default_value;

            if (slot.push) {
                pass.pushSize = std::max(pass.pushSize, slot.offset + slot.size);
            } else {
                pass.uboBinding = sp.buffer;
                pass.uboSize = std::max(pass.uboSize, slot.offset + slot.size);
            }
            pass.uniforms.push_back(std::move(slot));
        }

        for (size_t s = 0; s < pass.shader->sampler_count; ++s) {
            SamplerSlot slot;
            slot.binding = uint32_t(pass.shader->samplers[s].binding);
            slot.ref = ClassifyTexture(pass.shader->samplers[s].name ? pass.shader->samplers[s].name
                                                                     : "");
            pass.samplers.push_back(std::move(slot));
        }
    } else {
        // The built-in passthrough, which declares the same contract by hand.
        UniformSlot mvp;
        mvp.semantic = UniformSemantic::kMvp;
        mvp.offset = 0;
        mvp.size = 64;
        pass.uniforms.push_back(mvp);
        pass.uboBinding = 0;
        pass.uboSize = 128;

        SamplerSlot src;
        src.binding = 1;
        src.ref.semantic = TextureSemantic::kSource;
        pass.samplers.push_back(src);
    }

    pass.format = FormatFor(pass, last);
    if (pass.format == VK_FORMAT_UNDEFINED) {
        _reason = "this device cannot provide a pass format the preset requires";
        Log("[chain] pass %u (%s) needs '%s', which this device cannot render to", index,
            pass.name.c_str(), pass.shader && pass.shader->format ? pass.shader->format : "?");
        return false;
    }
    return true;
}

bool Chain::BuildShared() {
    if (!MakeHostBuffer(_vk, _instance, _device, _physical, _vertices, sizeof(kQuad),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
        _reason = "could not allocate the vertex buffer";
        return false;
    }
    std::memcpy(_vertices.mapped, kQuad, sizeof(kQuad));

    // A uniform buffer binding can be offset into, but only to a multiple of the device's own
    // alignment, so each pass's slice starts on that boundary rather than wherever the last ended.
    VkPhysicalDeviceProperties props {};
    _instance->vkGetPhysicalDeviceProperties(_physical, &props);
    const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment
                                   ? props.limits.minUniformBufferOffsetAlignment
                                   : 1;

    VkDeviceSize offset = 0;
    uint32_t uboCount = 0, samplerCount = 0;
    for (auto& pass : _passes) {
        pass.uboOffset = offset;
        if (pass.uboSize) {
            offset += ((pass.uboSize + align - 1) / align) * align;
            ++uboCount;
        }
        samplerCount += uint32_t(pass.samplers.size());
    }
    _uboRingStride = offset ? offset : align;

    if (!MakeHostBuffer(_vk, _instance, _device, _physical, _uniforms, _uboRingStride * kRing,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        _reason = "could not allocate the uniform ring";
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::max(1u, uboCount * kRing)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, samplerCount * kRing)};

    VkDescriptorPoolCreateInfo dpci {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = uint32_t(_passes.size()) * kRing;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    if (_vk->vkCreateDescriptorPool(_device, &dpci, nullptr, &_descriptorPool) != VK_SUCCESS) {
        _reason = "could not create the descriptor pool";
        return false;
    }
    return true;
}

bool Chain::BuildLuts(const SgPreset* preset) {
    if (!preset) return true;

    for (size_t i = 0; i < preset->texture_count; ++i) {
        const SgTexture& st = preset->textures[i];
        if (!st.data || !st.data->data || !st.data->length) continue;

        DecodedImage decoded;
        if (!DecodeImage(st.data->data, st.data->length, decoded)) {
            // A preset whose bezel art cannot be decoded still runs; the pass that samples it sees
            // the frame instead of the overlay, which is wrong but visible, and the alternative is
            // no preset at all.
            Log("[chain] could not decode texture '%s': %s", st.name ? st.name : "?",
                DecodeImageReason());
            continue;
        }

        Lut lut;
        lut.name = st.name ? st.name : "";
        lut.linear = ParseBool(LookupStr(st.preset_params, st.preset_param_count, "linear"), false);
        lut.mipmap = ParseBool(LookupStr(st.preset_params, st.preset_param_count, "mipmap"), false);
        lut.wrap = ParseWrapMode(LookupStr(st.preset_params, st.preset_param_count, "wrap_mode"),
                                 WrapMode::kClampToEdge);

        const uint32_t lutLevels =
            lut.mipmap ? MipLevelsFor(decoded.width, decoded.height) : 1u;

        if (!MakeImage(_vk, _instance, _device, _physical, lut.image, decoded.width, decoded.height,
                       VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, lutLevels)) {
            Log("[chain] no room for texture '%s' (%ux%u)", lut.name.c_str(), decoded.width,
                decoded.height);
            continue;
        }

        // The pixels go up through a staging buffer the first frame records; holding the decoded
        // bytes until then costs a few megabytes and saves a second decode.
        lut.image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        _lutUploads.push_back({uint32_t(_luts.size()), std::move(decoded)});
        _luts.push_back(std::move(lut));
    }
    return true;
}

bool Chain::BuildHistory() {
    if (!_historyDepth) return true;

    _history.resize(_historyDepth);
    for (uint32_t i = 0; i < _historyDepth; ++i) {
        // A history frame is a copy of Original, and only ever level 0 of it: no preset asks for a
        // mip chain on a past frame, and building one every frame per entry would not be cheap.
        if (!MakeImage(_vk, _instance, _device, _physical, _history[i], _sourceWidth, _sourceHeight,
                       _chainFormat,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            _reason = "no room for the frame history this preset asks for";
            return false;
        }
    }
    return true;
}

bool Chain::BuildPassPipeline(Pass& pass, uint32_t index) {
    // Every pass ends in SHADER_READ_ONLY: the next pass samples it, and the last one is
    // transitioned to TRANSFER_SRC by hand before the copy into the swapchain. One less render pass
    // variant to keep straight, at the cost of a single barrier on the final pass.
    VkAttachmentDescription colour {};
    colour.format = pass.format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colour.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colourRef {};
    colourRef.attachment = 0;
    colourRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colourRef;

    // Reads of everything this pass samples must finish before it writes, and the write must finish
    // before the next pass samples it or the copy reads it. Stated rather than left to an implicit
    // dependency, which would only cover the first and only from TOP_OF_PIPE.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colour;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (_vk->vkCreateRenderPass(_device, &rpci, nullptr, &pass.renderPass) != VK_SUCCESS) {
        _reason = "could not create a render pass";
        return false;
    }

    // The bindings are the shader's own, taken from reflection, so a preset that numbers its
    // samplers unusually still works.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (pass.uboSize && pass.uboBinding >= 0) {
        VkDescriptorSetLayoutBinding b {};
        b.binding = uint32_t(pass.uboBinding);
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }
    for (const auto& s : pass.samplers) {
        VkDescriptorSetLayoutBinding b {};
        b.binding = s.binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }

    VkDescriptorSetLayoutCreateInfo dlci {};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = uint32_t(bindings.size());
    dlci.pBindings = bindings.empty() ? nullptr : bindings.data();
    if (_vk->vkCreateDescriptorSetLayout(_device, &dlci, nullptr, &pass.setLayout) != VK_SUCCESS) {
        _reason = "could not create a descriptor set layout";
        return false;
    }

    if (pass.pushSize > _maxPushConstants) {
        // Nothing can be done about it here: the block is the shader's own layout, and splitting it
        // would mean recompiling the SPIR-V. Say so plainly rather than let pipeline creation fail
        // with a validation error the user cannot act on.
        _reason = "a pass needs more push constants than this device provides";
        Log("[chain] pass %u (%s) wants %u bytes of push constants, the device allows %u", index,
            pass.name.c_str(), pass.pushSize, _maxPushConstants);
        return false;
    }

    VkPushConstantRange push {};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = pass.pushSize;

    VkPipelineLayoutCreateInfo plci {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &pass.setLayout;
    plci.pushConstantRangeCount = pass.pushSize ? 1 : 0;
    plci.pPushConstantRanges = pass.pushSize ? &push : nullptr;
    if (_vk->vkCreatePipelineLayout(_device, &plci, nullptr, &pass.pipelineLayout) != VK_SUCCESS) {
        _reason = "could not create a pipeline layout";
        return false;
    }

    VkDescriptorSetLayout layouts[kRing];
    for (uint32_t i = 0; i < kRing; ++i) layouts[i] = pass.setLayout;

    VkDescriptorSetAllocateInfo dsai {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = kRing;
    dsai.pSetLayouts = layouts;
    if (_vk->vkAllocateDescriptorSets(_device, &dsai, pass.sets) != VK_SUCCESS) {
        _reason = "could not allocate descriptor sets";
        return false;
    }

    const uint32_t* vertCode = kPassthroughVertSpv;
    size_t vertWords = kPassthroughVertSpvLen;
    const uint32_t* fragCode = kPassthroughFragSpv;
    size_t fragWords = kPassthroughFragSpvLen;
    if (pass.shader) {
        vertCode = pass.shader->vertex_spirv;
        vertWords = pass.shader->vertex_words;
        fragCode = pass.shader->fragment_spirv;
        fragWords = pass.shader->fragment_words;
    }
    if (!vertCode || !fragCode || !vertWords || !fragWords) {
        _reason = "a pass has no compiled shader";
        return false;
    }

    VkShaderModule vert = MakeModule(_vk, _device, vertCode, vertWords);
    VkShaderModule frag = MakeModule(_vk, _device, fragCode, fragWords);
    if (!vert || !frag) {
        if (vert) _vk->vkDestroyShaderModule(_device, vert, nullptr);
        if (frag) _vk->vkDestroyShaderModule(_device, frag, nullptr);
        _reason = "could not create the shader modules for a pass";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vb {};
    vb.binding = 0;
    vb.stride = sizeof(Vertex);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, x);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, u);

    VkPipelineVertexInputStateCreateInfo vi {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    // Viewport and scissor are dynamic so a resize does not rebuild pipelines -- only the images
    // and the framebuffers follow the raster.
    VkPipelineViewportStateCreateInfo vp {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamics;

    VkPipelineRasterizationStateCreateInfo rs {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend {};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkGraphicsPipelineCreateInfo gp {};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pass.pipelineLayout;
    gp.renderPass = pass.renderPass;
    gp.subpass = 0;

    const VkResult made =
        _vk->vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &gp, nullptr, &pass.pipeline);
    _vk->vkDestroyShaderModule(_device, vert, nullptr);
    _vk->vkDestroyShaderModule(_device, frag, nullptr);

    if (made != VK_SUCCESS) {
        _reason = "could not create a graphics pipeline";
        Log("[chain] pass %u (%s): vkCreateGraphicsPipelines -> %d", index, pass.name.c_str(),
            (int) made);
        return false;
    }
    return true;
}

bool Chain::BuildPassImages(Pass& pass, uint32_t index) {
    const bool last = (index + 1 == _passes.size());

    if (last) {
        // The final pass writes what goes on screen, so it renders at the swapchain's raster.
        pass.width = _swapWidth;
        pass.height = _swapHeight;
    } else {
        const uint32_t inW = index == 0 ? _sourceWidth : _passes[index - 1].width;
        const uint32_t inH = index == 0 ? _sourceHeight : _passes[index - 1].height;
        ScaledSize(pass.scaleTypeX, pass.scaleX, pass.scaleTypeY, pass.scaleY, inW, inH, _swapWidth,
                   _swapHeight, &pass.width, &pass.height);
    }

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Two images when this pass is sampled as PassFeedback, so what it wrote last frame survives
    // being overwritten this frame; the pair alternates rather than being copied.
    const uint32_t levels =
        pass.mipmapped ? MipLevelsFor(pass.width, pass.height) : 1u;

    const uint32_t copies = pass.feedback ? 2u : 1u;
    for (uint32_t i = 0; i < copies; ++i) {
        if (!MakeImage(_vk, _instance, _device, _physical, pass.output[i], pass.width, pass.height,
                       pass.format, usage, levels)) {
            _reason = "no room for a pass output image";
            return false;
        }

        // A framebuffer attachment must be a view of exactly one mip level, while the view the
        // image carries spans the whole chain for sampling. So a mipmapped pass needs a second
        // view, over level 0 alone, to render through.
        if (levels > 1) {
            VkImageViewCreateInfo vi {};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = pass.output[i].image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = pass.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (_vk->vkCreateImageView(_device, &vi, nullptr, &pass.targetView[i]) != VK_SUCCESS) {
                _reason = "could not create a render target view";
                return false;
            }
        }

        VkImageView attachment = levels > 1 ? pass.targetView[i] : pass.output[i].view;

        VkFramebufferCreateInfo fci {};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = pass.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &attachment;
        fci.width = pass.width;
        fci.height = pass.height;
        fci.layers = 1;
        if (_vk->vkCreateFramebuffer(_device, &fci, nullptr, &pass.framebuffer[i]) != VK_SUCCESS) {
            _reason = "could not create a framebuffer";
            return false;
        }
    }
    pass.current = 0;
    return true;
}

bool Chain::Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat,
                    uint32_t sourceWidth, uint32_t sourceHeight, const std::string& presetId) {
    if (!_usable) return false;

    if (_swapWidth == swapWidth && _swapHeight == swapHeight && _swapFormat == swapFormat &&
        _sourceWidth == sourceWidth && _sourceHeight == sourceHeight && _presetId == presetId &&
        _frame.image)
        return true;

    // Every internal surface uses the swapchain format's UNORM twin: sampling an _SRGB view would
    // decode to linear on the way in and re-encode on the way out, and the chain wants exactly the
    // numbers the game wrote.
    VkFormat chainFormat = swapFormat;
    switch (swapFormat) {
        case VK_FORMAT_B8G8R8A8_SRGB: chainFormat = VK_FORMAT_B8G8R8A8_UNORM; break;
        case VK_FORMAT_R8G8B8A8_SRGB: chainFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: chainFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32; break;
        default: break;
    }

    // The pass writes through a colour attachment and reads through a sampler, so the format has to
    // support both. A device that cannot do it leaves the game's frame alone rather than failing.
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (!FormatSupports(_instance, _physical, chainFormat, need)) {
        _reason = "this device cannot render to the swapchain's format";
        Log("[chain] %s (format %d)", _reason.c_str(), (int) chainFormat);
        _usable = false;
        return false;
    }

    DropAll();

    _chainFormat = chainFormat;
    _swapWidth = swapWidth;
    _swapHeight = swapHeight;
    _swapFormat = swapFormat;
    _sourceWidth = sourceWidth;
    _sourceHeight = sourceHeight;
    _presetId = presetId;

    // A named preset that is not in the catalogue is not a reason to stop: the chain falls back to
    // the passthrough, which is what "no preset" looks like anyway.
    _preset = Catalogue::Instance().Find(presetId);
    if (!presetId.empty() && !_preset)
        Log("[chain] preset '%s' is not in the catalogue; passing frames through", presetId.c_str());

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags src = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags dst = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // BuildPasses decides whether Original needs a mip chain, so the passes are worked out first
    // and the images that depend on that answer are made after.
    bool ok = BuildPasses(_preset);

    const uint32_t originalLevels =
        _originalMipmapped ? MipLevelsFor(_sourceWidth, _sourceHeight) : 1u;
    const bool scaled = (_sourceWidth != _swapWidth || _sourceHeight != _swapHeight);

    ok = ok && MakeImage(_vk, _instance, _device, _physical, _frame, _swapWidth, _swapHeight,
                         _chainFormat, sampled | dst | src, scaled ? 1u : originalLevels);
    if (ok && scaled)
        ok = MakeImage(_vk, _instance, _device, _physical, _originalScaled, _sourceWidth,
                       _sourceHeight, _chainFormat, sampled | dst | src, originalLevels);

    if (ok)
        for (uint32_t i = 0; i < _passes.size() && ok; ++i)
            ok = BuildPassResources(_passes[i], i);
    ok = ok && BuildShared();
    if (ok)
        for (uint32_t i = 0; i < _passes.size() && ok; ++i)
            ok = BuildPassPipeline(_passes[i], i) && BuildPassImages(_passes[i], i);
    ok = ok && BuildLuts(_preset);
    ok = ok && BuildHistory();

    if (ok && SelfTestWanted()) {
        _selfTestHalf = VkDeviceSize(_swapWidth) * _swapHeight * FormatBytes(_chainFormat);
        if (!MakeHostBuffer(_vk, _instance, _device, _physical, _selfTest, _selfTestHalf * 2,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            Log("[chain] self-test buffer could not be allocated; continuing without it");
            _selfTestHalf = 0;
        }
        _selfTestReported = false;
    }

    if (!ok) {
        Log("[chain] %s", _reason.empty() ? "could not build the chain" : _reason.c_str());
        DropAll();
        _usable = false;
        return false;
    }

    ApplyParameters(nullptr, 0);  // fold in the preset's own overrides and any value already set
    _clearPending = true;
    _historyCursor = 0;

    uint32_t feedbackPasses = 0;
    for (const auto& pass : _passes)
        if (pass.feedback) ++feedbackPasses;

    Log("[chain] built %ux%u, source %ux%u, format %d, %zu pass%s%s", _swapWidth, _swapHeight,
        _sourceWidth, _sourceHeight, (int) _chainFormat, _passes.size(),
        _passes.size() == 1 ? "" : "es", _preset ? "" : " (passthrough)");
    if (feedbackPasses || _historyDepth || !_luts.empty()) {
        // Texture memory is worth naming: bezel art is 4K, a Mega Bezel preset carries a couple of
        // dozen pieces of it, and all of it is allocated inside somebody else's game.
        size_t textureBytes = 0;
        for (const auto& lut : _luts)
            textureBytes += size_t(lut.image.width) * lut.image.height * 4;

        // In KiB below a megabyte: a mask LUT is a few tens of kilobytes, and rounding that to
        // "0.0 MiB" reads like a texture that failed to load rather than one that is simply small.
        char size[32];
        if (textureBytes >= 1024 * 1024)
            snprintf(size, sizeof(size), "%.1f MiB", double(textureBytes) / (1024.0 * 1024.0));
        else
            snprintf(size, sizeof(size), "%zu KiB", (textureBytes + 1023) / 1024);

        uint32_t mipped = _originalMipmapped ? 1u : 0u;
        for (const auto& pass : _passes)
            if (pass.mipmapped) ++mipped;
        for (const auto& lut : _luts)
            if (lut.image.levels > 1) ++mipped;

        Log("[chain]   %u feedback pass%s, %u history frame%s, %zu texture%s (%s), %u mip chain%s",
            feedbackPasses, feedbackPasses == 1 ? "" : "es", _historyDepth,
            _historyDepth == 1 ? "" : "s", _luts.size(), _luts.size() == 1 ? "" : "s", size, mipped,
            mipped == 1 ? "" : "s");
    }
    _reason.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

void Chain::ApplyParameters(const ShmParam* params, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        // The interface writes fixed-width names; a name that fills the field has no terminator.
        const char* raw = params[i].name;
        const size_t len = strnlen(raw, kParamNameBytes);
        if (!len) continue;
        _parameters[std::string(raw, len)] = ShmLoadParam(params[i]);
    }

    // Precedence, lowest first: what the shader declared, what the preset overrode, what the user
    // set. Resolved here rather than looked up per frame, since a preset like crt-royale has
    // dozens of parameters across dozens of passes.
    for (auto& pass : _passes) {
        for (auto& slot : pass.uniforms) {
            if (slot.semantic != UniformSemantic::kParameter) continue;
            slot.value = slot.fallback;

            if (_preset) {
                for (size_t o = 0; o < _preset->override_count; ++o) {
                    if (_preset->overrides[o].name && slot.name == _preset->overrides[o].name) {
                        slot.value = _preset->overrides[o].value;
                        break;
                    }
                }
            }

            auto it = _parameters.find(slot.name);
            if (it != _parameters.end()) slot.value = it->second;
        }
    }
}

// ---------------------------------------------------------------------------
// Resolving names to images
// ---------------------------------------------------------------------------

const Image* Chain::ResolveStrict(const TextureRef& ref, size_t passIndex,
                                  const Image* original) const {
    if (ref.semantic == TextureSemantic::kLut) {
        for (const auto& p : _passes)
            if (!p.alias.empty() && p.alias == ref.name) return &p.output[p.current];
        for (const auto& lut : _luts)
            if (lut.name == ref.name && lut.image.image) return &lut.image;
        return nullptr;
    }
    return Resolve(ref, passIndex, original);
}

const Image* Chain::Resolve(const TextureRef& ref, size_t passIndex, const Image* original) const {
    switch (ref.semantic) {
        case TextureSemantic::kOriginal:
            return original;

        case TextureSemantic::kSource:
            if (passIndex == 0) return original;
            return &_passes[passIndex - 1].output[_passes[passIndex - 1].current];

        case TextureSemantic::kHistory: {
            // OriginalHistory1 is the previous frame. The ring is written at the end of each frame,
            // so counting back from the cursor lands on the right one.
            if (_history.empty() || ref.index == 0) return original;
            const uint32_t depth = uint32_t(_history.size());
            if (ref.index > depth) return original;
            const uint32_t at = (_historyCursor + depth - ref.index) % depth;
            return &_history[at];
        }

        case TextureSemantic::kPassOutput:
            // This frame's output of an *earlier* pass. A pass asking for its own index or a later
            // one is asking for something that has not been rendered yet -- and in the self case,
            // for the very image it is drawing into, which is a read-write hazard the validation
            // layer rightly rejects. libretro ships test/feedback-noncausal for exactly this, and
            // the name is the verdict: it is not causal, so it does not resolve.
            if (ref.index >= passIndex) return original;
            return &_passes[ref.index].output[_passes[ref.index].current];

        case TextureSemantic::kPassFeedback: {
            if (ref.index >= _passes.size()) return original;
            const Pass& p = _passes[ref.index];
            // Only a pass built with two images has a previous frame to give back. Every pass named
            // by a PassFeedback sampler is built that way, so this is a guard rather than a path:
            // without two images the only thing at that index is the target of a pass that may not
            // have run yet.
            if (!p.feedback) return original;
            return &p.output[1 - p.current];
        }

        case TextureSemantic::kUser:
            if (ref.index < _luts.size()) return &_luts[ref.index].image;
            return original;

        case TextureSemantic::kAliasFeedback: {
            for (const auto& p : _passes) {
                if (p.alias.empty() || p.alias != ref.name) continue;
                return p.feedback ? &p.output[1 - p.current] : &p.output[p.current];
            }
            // No pass carries that alias. It may still be a texture whose name simply ends in
            // "Feedback", so the LUT lookup gets a turn before this gives up.
            for (const auto& lut : _luts)
                if (lut.name == ref.name + "Feedback" && lut.image.image) return &lut.image;
            return original;
        }

        case TextureSemantic::kLut:
        default: {
            // A pass alias shadows a texture name, which is the order libretro resolves them in.
            for (const auto& p : _passes)
                if (!p.alias.empty() && p.alias == ref.name) return &p.output[p.current];
            for (const auto& lut : _luts)
                if (lut.name == ref.name && lut.image.image) return &lut.image;

            // Nothing declared it. Binding the frame keeps the descriptor valid, which matters:
            // an unwritten descriptor is undefined behaviour, and a wrong-looking pass is better
            // than a crash inside the game.
            return original;
        }
    }
}

void Chain::WriteUniforms(Pass& pass, size_t passIndex, uint64_t frameCount, const Image* original,
                          uint8_t* ubo, uint8_t* push) {
    for (const auto& slot : pass.uniforms) {
        uint8_t* base = slot.push ? push : ubo;
        if (!base) continue;

        switch (slot.semantic) {
            case UniformSemantic::kMvp:
                if (slot.size >= sizeof(kMvp)) std::memcpy(base + slot.offset, kMvp, sizeof(kMvp));
                break;

            case UniformSemantic::kFrameCount: {
                // frame_count_mod keeps a counter that a shader uses for a repeating pattern from
                // growing without bound, and from losing precision once it does.
                uint32_t value = uint32_t(frameCount);
                if (pass.frameCountMod) value = uint32_t(frameCount % pass.frameCountMod);
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kFrameDirection: {
                // Rewind is a libretro notion with no equivalent here, so always forwards.
                const int32_t value = 1;
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kRotation: {
                // Quarter turns of the output. The interface's rotation setting arrives with the
                // rest of the output controls in phase 6; until then the frame is never rotated,
                // and the shaders that read this -- 75 of them -- need it to say so rather than
                // pick up whatever a same-named shader parameter left behind.
                const uint32_t value = 0;
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kFrameTimeDelta: {
                // Microseconds since the previous frame. Shaders that decay a phosphor or advance
                // an animation integrate over it, and a constant zero freezes them.
                const uint32_t value = _frameTimeDeltaUs;
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kOriginalAspect:
            case UniformSemantic::kOriginalAspectRotated: {
                // Rotated and unrotated are the same until the output controls arrive in phase 6;
                // a quarter turn is what makes them differ.
                const uint32_t w = original ? original->width : 0;
                const uint32_t h = original ? original->height : 0;
                const float value = h ? float(w) / float(h) : 0.0f;
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kTotalSubFrames:
            case UniformSemantic::kCurrentSubFrame: {
                // This layer composes once per present, so there is one subframe and it is the
                // first. Leaving these at zero risks a divide by TotalSubFrames.
                const uint32_t value = 1;
                if (slot.size >= sizeof(value)) std::memcpy(base + slot.offset, &value, sizeof(value));
                break;
            }

            case UniformSemantic::kOutputSize: {
                float v[4];
                FillSize(v, pass.width, pass.height);
                if (slot.size >= sizeof(v)) std::memcpy(base + slot.offset, v, sizeof(v));
                break;
            }

            case UniformSemantic::kFinalViewportSize: {
                float v[4];
                FillSize(v, _swapWidth, _swapHeight);
                if (slot.size >= sizeof(v)) std::memcpy(base + slot.offset, v, sizeof(v));
                break;
            }

            case UniformSemantic::kTextureSize: {
                const Image* img = ResolveStrict(slot.texture, passIndex, original);
                if (!img) {
                    // Nothing by that name. It is most likely a shader parameter that happens to
                    // end in "Size", so it keeps the value a parameter would have had.
                    if (slot.size >= sizeof(float))
                        std::memcpy(base + slot.offset, &slot.value, sizeof(float));
                    break;
                }
                float v[4];
                FillSize(v, img->width, img->height);
                if (slot.size >= sizeof(v)) std::memcpy(base + slot.offset, v, sizeof(v));
                break;
            }

            case UniformSemantic::kParameter:
            default:
                if (slot.size >= sizeof(float))
                    std::memcpy(base + slot.offset, &slot.value, sizeof(float));
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void Chain::RecordPass(VkCommandBuffer cb, size_t passIndex, uint32_t slot, uint64_t frameCount,
                       const Image* original) {
    Pass& pass = _passes[passIndex];

    // The uniform block goes into this frame's slice of the ring; push constants go straight into
    // the command buffer, so they need a scratch of their own.
    // Sized to the largest push constant block Vulkan permits any device to advertise, so the
    // scratch never needs to grow; BuildPassPipeline has already refused anything the device
    // itself cannot take.
    uint8_t pushBytes[256] = {};
    uint8_t* ubo = nullptr;
    if (pass.uboSize)
        ubo = (uint8_t*) _uniforms.mapped + _uboRingStride * slot + pass.uboOffset;
    if (ubo) std::memset(ubo, 0, pass.uboSize);

    WriteUniforms(pass, passIndex, frameCount, original,
                  ubo, pass.pushSize ? pushBytes : nullptr);

    // Descriptors for this frame's slot. Every sampler the shader declared gets written, whether or
    // not the preset had anything sensible to put there.
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(pass.samplers.size());
    writes.reserve(pass.samplers.size() + 1);

    VkDescriptorBufferInfo bufferInfo {};
    if (pass.uboSize && pass.uboBinding >= 0) {
        bufferInfo = {_uniforms.buffer, _uboRingStride * slot + pass.uboOffset, pass.uboSize};
        VkWriteDescriptorSet w {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = pass.sets[slot];
        w.dstBinding = uint32_t(pass.uboBinding);
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufferInfo;
        writes.push_back(w);
    }

    for (const auto& s : pass.samplers) {
        const Image* img = Resolve(s.ref, passIndex, original);
        if (!img || !img->view) img = original;
        if (!img || !img->view) continue;

        // A LUT carries its own filter and wrap; everything else takes the pass's.
        bool linear = pass.filterLinear;
        WrapMode wrap = pass.wrap;
        if (s.ref.semantic == TextureSemantic::kLut || s.ref.semantic == TextureSemantic::kUser) {
            for (const auto& lut : _luts) {
                if (&lut.image == img) {
                    linear = lut.linear;
                    wrap = lut.wrap;
                    break;
                }
            }
        }

        images.push_back({SamplerFor(linear, wrap, img->levels > 1), img->view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});

        VkWriteDescriptorSet w {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = pass.sets[slot];
        w.dstBinding = s.binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }

    if (!writes.empty())
        _vk->vkUpdateDescriptorSets(_device, uint32_t(writes.size()), writes.data(), 0, nullptr);

    // The attachment's initial layout is UNDEFINED, so whatever the image held is discarded rather
    // than transitioned; the draw covers every pixel of it.
    Image& target = pass.output[pass.current];
    target.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkRenderPassBeginInfo rpbi {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = pass.renderPass;
    rpbi.framebuffer = pass.framebuffer[pass.current];
    rpbi.renderArea.extent = {pass.width, pass.height};
    _vk->vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {};
    viewport.width = float(pass.width);
    viewport.height = float(pass.height);
    viewport.maxDepth = 1.0f;
    _vk->vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor {};
    scissor.extent = {pass.width, pass.height};
    _vk->vkCmdSetScissor(cb, 0, 1, &scissor);

    _vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
    _vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipelineLayout, 0, 1,
                                 &pass.sets[slot], 0, nullptr);
    if (pass.pushSize)
        _vk->vkCmdPushConstants(cb, pass.pipelineLayout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                pass.pushSize, pushBytes);

    const VkDeviceSize offset = 0;
    _vk->vkCmdBindVertexBuffers(cb, 0, 1, &_vertices.buffer, &offset);
    _vk->vkCmdDraw(cb, 4, 1, 0, 0);

    _vk->vkCmdEndRenderPass(cb);

    // The render pass declared this as the attachment's final layout -- but only for level 0, which
    // is the only level it wrote.
    target.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // The pass that reads this one asked for mipmap_input, so the rest of the chain is filled from
    // what was just drawn, before anything samples it.
    if (target.levels > 1) GenerateMipmaps(_vk, cb, target);
}

bool Chain::Record(VkCommandBuffer cb, VkImage swapchainImage, uint64_t frameCount,
                   VkImageLayout externalLayout) {
    if (!_usable || !_frame.image || _passes.empty()) return false;

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kRing;

    // What FrameTimeDelta reports. Measured here rather than passed in, because this is the only
    // point in the layer that runs exactly once per composed frame.
    const double nowMs = NowMs();
    _frameTimeDeltaUs =
        _lastFrameMs > 0.0 ? uint32_t((nowMs - _lastFrameMs) * 1000.0) : 0u;
    _lastFrameMs = nowMs;

    // ---- the frame, as the game presented it ----
    TransitionForeign(_vk, cb, swapchainImage, externalLayout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy copy {};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_swapWidth, _swapHeight, 1};
    _vk->vkCmdCopyImage(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _frame.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Straight back, so every path out of here -- including the ones that give up -- leaves the
    // image in the layout the presentation engine requires.
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      externalLayout);

    // ---- one-time work, on the first frame after a build ----
    if (_clearPending) {
        // Feedback and history images are sampled before anything writes them. Undefined contents
        // are not merely ugly here, they are undefined behaviour, so they start black.
        const VkClearColorValue black {{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        for (auto& pass : _passes) {
            if (!pass.feedback) continue;
            for (uint32_t i = 0; i < 2; ++i) {
                if (!pass.output[i].image) continue;
                Transition(_vk, cb, pass.output[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                _vk->vkCmdClearColorImage(cb, pass.output[i].image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            }
        }
        for (auto& img : _history) {
            Transition(_vk, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            _vk->vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                      &range);
        }
        _clearPending = false;
    }

    if (!_lutUploads.empty()) {
        // One staging buffer for every texture the preset declared, filled and copied in this
        // frame and then released -- the decoded bytes are not needed again.
        VkDeviceSize total = 0;
        for (const auto& up : _lutUploads) total += up.pixels.rgba.size();

        if (MakeHostBuffer(_vk, _instance, _device, _physical, _lutStaging, total,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            VkDeviceSize at = 0;
            for (const auto& up : _lutUploads) {
                std::memcpy((uint8_t*) _lutStaging.mapped + at, up.pixels.rgba.data(),
                            up.pixels.rgba.size());

                Image& img = _luts[up.lut].image;
                Transition(_vk, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                VkBufferImageCopy region {};
                region.bufferOffset = at;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {up.pixels.width, up.pixels.height, 1};
                _vk->vkCmdCopyBufferToImage(cb, _lutStaging.buffer, img.image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                at += up.pixels.rgba.size();
            }
        } else {
            Log("[chain] no room to upload this preset's textures");
        }
        _lutUploads.clear();
        _lutUploads.shrink_to_fit();
    }

    // ---- the source raster ----
    Image* original = &_frame;
    if (_originalScaled.image) {
        // A linear blit, which is a placeholder: it is a bilinear reduction and aliases on a large
        // ratio. The real thing is an area average, and it arrives with the rest of the source-
        // resolution work in phase 6.
        Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(_vk, cb, _originalScaled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageBlit blit {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t) _swapWidth, (int32_t) _swapHeight, 1};
        blit.dstOffsets[1] = {(int32_t) _sourceWidth, (int32_t) _sourceHeight, 1};
        _vk->vkCmdBlitImage(cb, _frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            _originalScaled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                            VK_FILTER_LINEAR);
        original = &_originalScaled;
    }

    // Everything a pass can sample has to be readable before the first of them runs. Original gets
    // a mip chain here when pass 0 declared mipmap_input over it.
    if (original->levels > 1) GenerateMipmaps(_vk, cb, *original);
    else Transition(_vk, cb, *original, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (auto& img : _history) Transition(_vk, cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (auto& lut : _luts) {
        if (!lut.image.image) continue;
        // Only just uploaded, so this is where its chain is filled; on later frames the levels are
        // already there and it is a layout no-op.
        if (lut.image.levels > 1 && lut.image.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            GenerateMipmaps(_vk, cb, lut.image);
        else
            Transition(_vk, cb, lut.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (auto& pass : _passes)
        if (pass.feedback)
            Transition(_vk, cb, pass.output[1 - pass.current],
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ---- the passes ----
    for (size_t i = 0; i < _passes.size(); ++i) RecordPass(cb, i, slot, frameCount, original);

    Image& last = _passes.back().output[_passes.back().current];

    // ---- the self-test readback ----
    // Both copies are taken from what the chain actually consumed and produced, before anything
    // else touches either, so a mismatch can only come from the chain itself.
    _selfTestRecorded = false;
    if (_selfTestHalf && _selfTest.buffer && !_selfTestReported) {
        Transition(_vk, cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(_vk, cb, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy r {};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {_swapWidth, _swapHeight, 1};
        _vk->vkCmdCopyImageToBuffer(cb, _frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    _selfTest.buffer, 1, &r);
        r.bufferOffset = _selfTestHalf;
        _vk->vkCmdCopyImageToBuffer(cb, last.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    _selfTest.buffer, 1, &r);
        _selfTestRecorded = true;
    }

    // ---- back into the swapchain ----
    Transition(_vk, cb, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionForeign(_vk, cb, swapchainImage, externalLayout,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    _vk->vkCmdCopyImage(cb, last.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionForeign(_vk, cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      externalLayout);

    // ---- what the next frame inherits ----
    if (!_history.empty()) {
        // Written after the passes have read the ring, which matters at depth 1, where the entry
        // being written is the one OriginalHistory1 just pointed at.
        Image& into = _history[_historyCursor];
        Transition(_vk, cb, *original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(_vk, cb, into, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageCopy hc {};
        hc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        hc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        hc.extent = {std::min(original->width, into.width), std::min(original->height, into.height),
                     1};
        _vk->vkCmdCopyImage(cb, original->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &hc);
        _historyCursor = (_historyCursor + 1) % uint32_t(_history.size());
    }

    for (auto& pass : _passes)
        if (pass.feedback) pass.current = 1 - pass.current;

    return true;
}

void Chain::FrameCompleted() {
    // The upload recorded in the retired frame has happened, so the staging buffer can go. For a
    // Mega Bezel preset it is tens of megabytes of host memory, in a process that belongs to
    // someone else.
    if (_lutStaging.buffer && _lutUploads.empty()) DropHostBuffer(_vk, _device, _lutStaging);

    ConsumeSelfTest();
}

void Chain::ConsumeSelfTest() {
    if (!_selfTestRecorded || _selfTestReported || !_selfTest.mapped) return;
    _selfTestRecorded = false;
    _selfTestReported = true;

    const auto* before = (const uint8_t*) _selfTest.mapped;
    const auto* after = before + _selfTestHalf;

    size_t differing = 0;
    size_t firstAt = 0;
    for (size_t i = 0; i < (size_t) _selfTestHalf; ++i) {
        if (before[i] != after[i]) {
            if (!differing) firstAt = i;
            ++differing;
        }
    }

    // With a preset the result is not meant to match the frame, so there is nothing to pass or
    // fail -- but how much of the frame it changed is worth saying, because a preset that builds,
    // records and submits without complaint and then changes nothing looks identical to a working
    // one from every log line above this.
    if (_preset) {
        Log("[selftest] preset '%s': %.1f%% of the frame's bytes differ from the input (%zu pass%s)",
            _presetId.c_str(),
            _selfTestHalf ? 100.0 * double(differing) / double(_selfTestHalf) : 0.0,
            _passes.size(), _passes.size() == 1 ? "" : "es");
        return;
    }
    if (_originalScaled.image) {
        // The shader was shown a reduced raster, so the result is not meant to match the frame.
        Log("[selftest] source %ux%u differs from the frame %ux%u; exactness is not expected here",
            _sourceWidth, _sourceHeight, _swapWidth, _swapHeight);
        return;
    }

    if (!differing) {
        Log("[selftest] PASS: the passthrough reproduced the frame exactly (%ux%u, %llu bytes)",
            _swapWidth, _swapHeight, (unsigned long long) _selfTestHalf);
    } else {
        Log("[selftest] FAIL: %llu of %llu bytes differ, first at %llu (%ux%u)",
            (unsigned long long) differing, (unsigned long long) _selfTestHalf,
            (unsigned long long) firstAt, _swapWidth, _swapHeight);
    }
}

}  // namespace shaderglass
