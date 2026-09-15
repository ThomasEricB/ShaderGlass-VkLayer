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
      -> source       the frame at the raster the shader is shown (the reinterpreted "pixel size")
      -> pass 0..N    the preset's passes, each a fullscreen quad through a graphics pipeline
      -> swapchain    the result, copied back before the game's present goes through

Phase 2 builds that path with one built-in passthrough pass. The pass is written to the real libretro
contract -- vec4 Position and vec2 TexCoord attributes, an MVP, and the standard uniform block -- so
the executor around it is the one that will run real presets in phase 4 rather than a stand-in that
has to be replaced.

Every failure is fail-open: a chain that cannot be built leaves the game's own frame on screen.
*/

#pragma once

#include "vk_util.h"

#include <string>

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

    // Build or rebuild for this swapchain and this source raster. Cheap and a no-op when nothing has
    // changed, so it is safe to call every present.
    bool Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat, uint32_t sourceWidth,
                 uint32_t sourceHeight);

    // Record the whole chain. The swapchain image arrives in PRESENT_SRC_KHR and is left in it on
    // every path out, including the ones that give up, so a caller that stops here still presents
    // something valid.
    bool Record(VkCommandBuffer cb, VkImage swapchainImage, uint64_t frameCount);

    // The passthrough at matching rasters must reproduce the frame exactly. SHADERGLASS_SELFTEST=1
    // copies both the frame that went in and the result that came out, and compares them once the
    // fence has landed -- turning "the API calls were legal" into "the pixels are identical".
    void ConsumeSelfTest();

    uint32_t PassCount() const { return _usable ? 1u : 0u; }
    uint32_t OutputWidth() const { return _swapWidth; }
    uint32_t OutputHeight() const { return _swapHeight; }
    uint32_t SourceWidth() const { return _sourceWidth; }
    uint32_t SourceHeight() const { return _sourceHeight; }

  private:
    // What the shader reads, in the std140 layout the libretro contract declares. Real presets read
    // these even when the passthrough does not, so the block is filled properly from the start.
    struct Ubo {
        float mvp[16];
        float sourceSize[4];    // w, h, 1/w, 1/h
        float originalSize[4];  // the frame as the game presented it
        float outputSize[4];    // what this pass writes
        uint32_t frameCount;
        uint32_t pad[3];
    };
    static_assert(sizeof(Ubo) == 128, "the uniform block must match the std140 layout in the GLSL");

    bool BuildStatic();   // pipeline, sampler, vertex buffer, descriptors -- raster-independent
    bool BuildSized();    // images and framebuffer -- rebuilt when the raster changes
    void DropSized();
    void DropStatic();

    const DeviceTable* _vk = nullptr;
    const InstanceTable* _instance = nullptr;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;

    bool _usable = false;
    bool _staticBuilt = false;
    std::string _reason;

    uint32_t _swapWidth = 0, _swapHeight = 0;
    uint32_t _sourceWidth = 0, _sourceHeight = 0;
    VkFormat _swapFormat = VK_FORMAT_UNDEFINED;
    VkFormat _chainFormat = VK_FORMAT_UNDEFINED;

    // The frame as the game presented it, the same frame at the source raster, and what the pass
    // writes. _source is only allocated when the source raster differs from the swapchain.
    Image _frame {}, _source {}, _output {};

    VkRenderPass _renderPass = VK_NULL_HANDLE;
    VkFramebuffer _framebuffer = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkSampler _sampler = VK_NULL_HANDLE;

    HostBuffer _vertices {};
    HostBuffer _uniforms {};

    // Only allocated when the self-test is asked for.
    HostBuffer _selfTest {};
    VkDeviceSize _selfTestHalf = 0;
    bool _selfTestRecorded = false;
    bool _selfTestReported = false;

    // A ring, so a frame still in flight cannot have its uniforms or its descriptor rewritten under
    // it. The layer waits on the previous frame's fence before re-recording, which makes one slot
    // sufficient -- the ring is what keeps that from being load-bearing.
    static constexpr uint32_t kRing = 3;
    VkDescriptorSet _sets[kRing] = {};
    VkDeviceSize _uboStride = 0;
    uint32_t _slot = 0;
};

}  // namespace shaderglass
