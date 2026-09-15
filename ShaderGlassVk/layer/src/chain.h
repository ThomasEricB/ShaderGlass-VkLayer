/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

The shader chain, on the game's own device.

ShaderGlass runs a libretro preset over a captured texture in D3D11 and presents the result in its
own window. Here the frame is already in hand at present time, so the same chain runs in place:

    swapchain image
      -> frame        a copy in the chain's working format, so nothing below touches the game's image
      -> original     the frame at the raster the shader is shown (the reinterpreted "pixel size")
      -> pass 0..N    the preset's passes, each a fullscreen quad through a graphics pipeline
      -> swapchain    the last pass's result, copied back before the game's present goes through

Each pass writes its own image at its own size and format; what a pass reads is decided by the names
its shader declares, which is how libretro presets work -- see semantics.h. Passes referenced as
PassFeedback ping-pong between two images so a pass can read what it wrote last frame without a
copy, and OriginalHistory keeps a small ring of past frames.

With no preset the chain still builds, with one built-in passthrough pass. That is the default state
(decision 13) and it is also what SHADERGLASS_SELFTEST=1 proves bit-exact.

Every failure is fail-open: a chain that cannot be built leaves the game's own frame on screen.
*/

#pragma once

#include "../../common/shm_protocol.h"
#include "../../presets/preset_api.h"
#include "semantics.h"
#include "texture.h"
#include "vk_util.h"

#include <map>
#include <string>
#include <vector>

namespace shaderglass {

class Chain {
  public:
    Chain(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
          VkPhysicalDevice physical);
    ~Chain();

    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    bool Usable() const { return _usable; }

    // Why not, when not. Empty while it is working.
    const char* Reason() const { return _reason.c_str(); }

    // Build or rebuild for this swapchain, this source raster and this preset. Cheap and a no-op
    // when nothing has changed, so it is safe to call every present.
    bool Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat, uint32_t sourceWidth,
                 uint32_t sourceHeight, const std::string& presetId);

    // Parameter values from the interface, matched to the preset's declarations by name. Cheap and
    // does not rebuild anything -- a parameter tweak only changes the bytes written to a uniform
    // block, which is why the protocol separates tuningSeq from controlSeq.
    void ApplyParameters(const ShmParam* params, uint32_t count);

    // Record the whole chain. The swapchain image arrives in `externalLayout` and is left in it on
    // every path out, including the ones that give up, so a caller that stops here still presents
    // something valid. The layer always passes PRESENT_SRC_KHR; the parameter exists because
    // tests/chain_test.cpp drives the chain over an ordinary image, with no swapchain in sight.
    bool Record(VkCommandBuffer cb, VkImage swapchainImage, uint64_t frameCount,
                VkImageLayout externalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Call once the previous Record's submit has retired -- the layer does it after waiting on that
    // frame's fence. Resources a command buffer was still referencing can only be released here,
    // and the self-test readback can only be read here.
    //
    // This is the chain's one ordering requirement on its caller, so it is a method rather than
    // something inferred inside Record: a caller that records twice without a fence in between gets
    // a buffer destroyed underneath a command buffer that still refers to it, which is exactly what
    // tests/chain_test.cpp caught when Record tried to be clever about it.
    void FrameCompleted();

    uint32_t PassCount() const { return uint32_t(_passes.size()); }
    uint32_t OutputWidth() const { return _swapWidth; }
    uint32_t OutputHeight() const { return _swapHeight; }
    uint32_t SourceWidth() const { return _sourceWidth; }
    uint32_t SourceHeight() const { return _sourceHeight; }
    const std::string& PresetId() const { return _presetId; }

  private:
    // Where one member of a uniform block gets its value. Resolved once when the chain is built, so
    // recording a frame is a walk over a flat list rather than a string comparison per member.
    struct UniformSlot {
        UniformSemantic semantic = UniformSemantic::kParameter;
        TextureRef texture;      // for kTextureSize
        bool push = false;       // push constant block rather than the UBO
        uint32_t offset = 0;
        uint32_t size = 0;
        float value = 0.0f;      // for kParameter: the value in force
        float fallback = 0.0f;   // for kParameter: what the shader itself declared
        std::string name;        // for kParameter: what the interface calls it
    };

    // A sampler the shader declared, and what it resolves to.
    struct SamplerSlot {
        uint32_t binding = 0;
        TextureRef ref;
    };

    // A preset texture: a LUT, a bezel, an overlay. Decoded once when the chain is built.
    struct Lut {
        std::string name;
        Image image {};
        bool linear = false;
        bool mipmap = false;
        WrapMode wrap = WrapMode::kClampToEdge;
    };

    struct Pass {
        const SgShader* shader = nullptr;  // null for the built-in passthrough
        std::string name;

        // From the preset's keys.
        ScaleType scaleTypeX = ScaleType::kSource;
        ScaleType scaleTypeY = ScaleType::kSource;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        bool filterLinear = false;
        WrapMode wrap = WrapMode::kClampToEdge;
        bool srgb = false;
        bool floatFb = false;
        bool mipmapInput = false;  // this pass wants a mip chain on what it reads
        uint32_t frameCountMod = 0;
        std::string alias;

        // Derived when the chain is built.
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        bool feedback = false;  // some pass samples PassFeedback<this one>
        bool mipmapped = false;  // the pass that reads this one asked for a mip chain

        // Two images when this pass is read back as feedback, so last frame's result survives
        // being overwritten; one otherwise. `current` says which the next frame writes.
        Image output[2] {};
        VkFramebuffer framebuffer[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

        // A framebuffer attachment must view exactly one mip level, while the view the image
        // carries spans the chain for sampling. Only a mipmapped pass needs this second view.
        VkImageView targetView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint32_t current = 0;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorSet sets[3] = {};

        int32_t uboBinding = -1;
        uint32_t uboSize = 0;
        uint32_t pushSize = 0;
        VkDeviceSize uboOffset = 0;  // this pass's slice of the shared uniform ring

        std::vector<UniformSlot> uniforms;
        std::vector<SamplerSlot> samplers;
    };

    // The passthrough at matching rasters must reproduce the frame exactly. SHADERGLASS_SELFTEST=1
    // copies both the frame that went in and the result that came out, and compares them once the
    // fence has landed -- turning "the API calls were legal" into "the pixels are identical".
    void ConsumeSelfTest();

    bool BuildPasses(const SgPreset* preset);
    bool BuildPassResources(Pass& pass, uint32_t index);   // what its names resolve to
    bool BuildPassPipeline(Pass& pass, uint32_t index);    // render pass, layouts, pipeline
    bool BuildPassImages(Pass& pass, uint32_t index);      // output images and framebuffers
    bool BuildShared();
    bool BuildLuts(const SgPreset* preset);
    bool BuildHistory();
    void DropAll();
    void DropSized();

    VkSampler SamplerFor(bool linear, WrapMode wrap, bool mipmapped);
    VkFormat FormatFor(const Pass& pass, bool last) const;

    // What one sampler name resolves to for the pass at passIndex. `original` is the frame at the
    // source raster, which is passed in rather than stored because it is whichever of two images
    // the raster happened to call for.
    const Image* Resolve(const TextureRef& ref, size_t passIndex, const Image* original) const;

    // As Resolve, but returns nullptr instead of falling back when a name matches nothing the
    // preset declared. A sampler must be bound to something valid, so Resolve substitutes the
    // frame; a size can honestly be zero, and saying 640x480 for a texture that does not exist is
    // worse than saying nothing -- "<something>Size" is also how a shader parameter may be spelled.
    const Image* ResolveStrict(const TextureRef& ref, size_t passIndex, const Image* original) const;

    void WriteUniforms(Pass& pass, size_t passIndex, uint64_t frameCount, const Image* original,
                       uint8_t* ubo, uint8_t* push);
    void RecordPass(VkCommandBuffer cb, size_t passIndex, uint32_t slot, uint64_t frameCount,
                    const Image* original);

    const DeviceTable* _vk = nullptr;
    const InstanceTable* _instance = nullptr;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;

    bool _usable = false;
    std::string _reason;

    uint32_t _swapWidth = 0, _swapHeight = 0;
    uint32_t _sourceWidth = 0, _sourceHeight = 0;
    VkFormat _swapFormat = VK_FORMAT_UNDEFINED;
    VkFormat _chainFormat = VK_FORMAT_UNDEFINED;

    // Vulkan guarantees only 128 bytes of push constants, and some devices give no more. Libretro
    // shaders put their parameters there, so a preset can want more than the device has.
    uint32_t _maxPushConstants = 128;
    std::string _presetId;
    const SgPreset* _preset = nullptr;

    // Set when pass 0 declares mipmap_input: what it reads is Original, so Original is the image
    // that needs the chain.
    bool _originalMipmapped = false;

    // The frame as the game presented it, and the same frame at the source raster. _original is
    // only a separate image when the source raster differs from the swapchain.
    Image _frame {};
    Image _originalScaled {};

    // OriginalHistory#: as many past frames as any shader in the preset asked for.
    std::vector<Image> _history;
    uint32_t _historyDepth = 0;

    std::vector<Lut> _luts;

    // Decoded pixels waiting for a command buffer. Textures are decoded while the chain is built,
    // but getting them onto the GPU needs a copy recorded into a frame, so they sit here until the
    // first Record and the staging buffer is released with them.
    struct LutUpload {
        uint32_t lut = 0;
        DecodedImage pixels;
    };
    std::vector<LutUpload> _lutUploads;
    HostBuffer _lutStaging {};

    std::vector<Pass> _passes;

    // Feedback and history images are sampled before anything has written them, on the first frame
    // after a build. They are cleared once, in that frame, rather than left undefined.
    bool _clearPending = false;

    // Which entry of the history ring the next frame's Original goes into.
    uint32_t _historyCursor = 0;

    // What FrameTimeDelta reports, and what it is measured against.
    double _lastFrameMs = 0.0;
    uint32_t _frameTimeDeltaUs = 0;

    // Samplers are shared across passes: there are only eight combinations and a preset that uses
    // all of them still wants one object each.
    std::map<uint32_t, VkSampler> _samplers;

    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    HostBuffer _vertices {};
    HostBuffer _uniforms {};
    VkDeviceSize _uboRingStride = 0;  // bytes between one frame's uniforms and the next

    // Parameter values from the interface, by name. Kept whether or not a preset declares them, so
    // switching presets does not lose a value the user set.
    std::map<std::string, float> _parameters;

    // Only allocated when the self-test is asked for.
    HostBuffer _selfTest {};
    VkDeviceSize _selfTestHalf = 0;
    bool _selfTestRecorded = false;
    bool _selfTestReported = false;

    // A ring, so a frame still in flight cannot have its uniforms or its descriptor rewritten under
    // it. The layer waits on the previous frame's fence before re-recording, which makes one slot
    // sufficient -- the ring is what keeps that from being load-bearing.
    static constexpr uint32_t kRing = 3;
    uint32_t _slot = 0;
};

}  // namespace shaderglass
