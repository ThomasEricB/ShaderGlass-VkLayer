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
#include "pixel_grid.h"
#include "placement.h"
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

    // Something the user should be told about a chain that is otherwise running -- a preset that
    // was named but could not be loaded, so the frame is going through untouched. Distinct from
    // Reason(), which means the chain cannot run at all. Empty when there is nothing to say, so
    // publishing it every frame also clears it once the cause is fixed.
    const char* Notice() const { return _notice.c_str(); }

    // Build or rebuild for this swapchain, this source raster and this preset. Cheap and a no-op
    // when nothing has changed, so it is safe to call every present.
    bool Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat, uint32_t sourceWidth,
                 uint32_t sourceHeight, const std::string& presetId);

    // The same, with the output settings: where in the swapchain the effect reads and writes, what
    // the last pass renders at, and whether it is turned. Only the target's size, the view and the
    // turn rebuild anything; where the picture lands, the bars and the mirrors are applied at record
    // time, so moving a crop or flipping the image costs nothing. See placement.h.
    //
    // The overload above is this one with the identity placement -- the whole swapchain, one to
    // one -- which is what every caller before phase 6 meant, and what the self-test depends on.
    bool Prepare(uint32_t swapWidth, uint32_t swapHeight, VkFormat swapFormat, uint32_t sourceWidth,
                 uint32_t sourceHeight, const Placement& placement, const std::string& presetId);

    // Place the last composed picture into the swapchain again, without running a pass. What frame
    // skip draws on the frames it skips, and what a paused chain draws every frame. Returns false
    // when there is nothing composed yet to show.
    bool RecordReplay(VkCommandBuffer cb, VkImage swapchainImage,
                      VkImageLayout externalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Parameter values from the interface, matched to the preset's declarations by name. Cheap and
    // does not rebuild anything -- a parameter tweak only changes the bytes written to a uniform
    // block, which is why the protocol separates tuningSeq from controlSeq.
    void ApplyParameters(const ShmParam* params, uint32_t count);

    // Record the whole chain. The swapchain image arrives in `externalLayout` and is left in it on
    // every path out, including the ones that give up, so a caller that stops here still presents
    // something valid. The layer always passes PRESENT_SRC_KHR; the parameter exists because
    // tests/chain_test.cpp drives the chain over an ordinary image, with no swapchain in sight.
    //
    // keepInput runs the passes again on the frame the chain already holds rather than taking the
    // swapchain's new one -- a paused chain, where the picture stays still but a parameter can still
    // be adjusted against it. Ignored until the chain holds a frame, since until then there is
    // nothing to hold.
    bool Record(VkCommandBuffer cb, VkImage swapchainImage, uint64_t frameCount,
                VkImageLayout externalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                bool keepInput = false);

    // Whether there is a composed picture to replay.
    bool HasComposed() const { return _composed; }

    // Whether there is a game frame to compose from without reading the swapchain -- which after a
    // rebuild there still is, when the new chain's frame is the same size and format as the old: a
    // preset changed while the game is not presenting is composed from the frame it last presented.
    bool HoldsInput() const { return _inputHeld; }

    // Write the result back through this storage view instead of copying it. For a target the chain
    // may not copy into: gamescope's output images carry Storage but not TransferDst, and the view
    // is the one gamescope itself binds to write them, so this write is exactly as valid as its own.
    // VK_NULL_HANDLE goes back to copying, which is what every swapchain uses.
    void SetStorageTarget(VkImageView view) { _storageView = view; }

    // Call once the previous Record's submit has retired -- the layer does it after waiting on that
    // frame's fence. Resources a command buffer was still referencing can only be released here,
    // and the self-test readback can only be read here.
    //
    // This is the chain's one ordering requirement on its caller, so it is a method rather than
    // something inferred inside Record: a caller that records twice without a fence in between gets
    // a buffer destroyed underneath a command buffer that still refers to it, which is exactly what
    // tests/chain_test.cpp caught when Record tried to be clever about it.
    void FrameCompleted();

    // Ask for one matched pair of frames -- what the game presented, and what it presented after
    // the chain ran -- written as PPM beside the mapping. The interface shows the pair; nothing
    // here renders a preview (decision 8), because a second render in another process would be a
    // different frame at a different raster and would prove nothing about this one.
    void RequestCapture();

    // Ask for one measurement of the game's own raster out of the next recorded frame. The answer
    // arrives after that frame's FrameCompleted, and is read with TakeGridEstimate.
    void RequestGridProbe();

    // The last measurement, once. Returns false until a probe has completed, and false again for
    // every call after the first, so a caller cannot act twice on one measurement.
    bool TakeGridEstimate(GridEstimate* out);

    uint32_t PassCount() const { return uint32_t(_passes.size()); }

    // What a pass wrote, for tools that want to look inside the chain. A signal chain -- NTSC
    // encode, DAC, demodulate -- carries values that are not an image until the last pass decodes
    // them, so "the final frame is black" says nothing about which pass broke it.
    struct PassInfo {
        const char* name = "";
        VkImage image = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t levels = 1;
        bool feedback = false;
        const char* alias = "";
    };
    PassInfo PassAt(uint32_t index) const;
    // The view: what the preset's last pass renders at, before it is placed. It was the swapchain
    // until phase 6, which is why the protocol calls it the output raster.
    uint32_t OutputWidth() const { return _viewWidth; }
    uint32_t OutputHeight() const { return _viewHeight; }
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

        // The chain's own quarter-turn pass, appended after the preset's when the output is turned.
        // A passthrough with a rotated MVP -- a blit can mirror an image but cannot turn it.
        bool turn = false;
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
    std::string _notice;

    uint32_t _swapWidth = 0, _swapHeight = 0;
    uint32_t _sourceWidth = 0, _sourceHeight = 0;

    // Phase 6 geometry. The target is where in the swapchain the effect reads and writes; the view
    // is what the preset's last pass renders at, which libretro calls the viewport and which is no
    // longer the swapchain. The placement's per-frame half lives in _placement.
    uint32_t _targetWidth = 0, _targetHeight = 0;
    uint32_t _viewWidth = 0, _viewHeight = 0;
    bool _quarterTurn = false;
    size_t _userPassCount = 0;  // the preset's own passes; the turn pass, if any, comes after
    Placement _placement {};
    bool _composed = false;  // there is a result in the last pass that a replay could show
    bool _inputHeld = false;  // _frame holds a frame the game presented

    // _frame, taken out of the old chain's way across a rebuild and put back if the new one wants the
    // same image. Dropped with the sized surfaces if nobody claims it.
    Image _keptFrame {};

    // Black, one pixel, blitted to fill the bars around a letterboxed picture. vkCmdClearColorImage
    // clears a whole image or nothing, and a bar is a rectangle inside the swapchain.
    Image _black {};

    // Where the picture is composed before it goes to the swapchain, in the chain's own format.
    //
    // Not blitted into the swapchain directly, because a blit converts: an _SRGB swapchain behind a
    // chain that works in the _UNORM twin would have every value sRGB-encoded on the way in, and the
    // picture would come out visibly too bright. The chain uses the UNORM twin precisely so that the
    // numbers the game wrote are the numbers it works on, and the one step that reaches the
    // swapchain has to be a raw copy to keep that true. Only the target rectangle is copied, so
    // everything outside a crop is never written at all.
    Image _placed {};

    bool IsIdentityPlacement() const;
    void RecordOutput(VkCommandBuffer cb, VkImage swapchainImage, VkImageLayout externalLayout);
    void RecordPlacement(VkCommandBuffer cb, VkImage swapchainImage, VkImageLayout externalLayout);
    void ComposePlaced(VkCommandBuffer cb);

    // The storage write-back: built the first time a storage target is used, and kept, since it does
    // not depend on the swapchain's size.
    VkImageView _storageView = VK_NULL_HANDLE;
    VkDescriptorSetLayout _wbSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _wbLayout = VK_NULL_HANDLE;
    VkPipeline _wbPipeline = VK_NULL_HANDLE;
    VkDescriptorPool _wbPool = VK_NULL_HANDLE;
    VkDescriptorSet _wbSet = VK_NULL_HANDLE;
    VkSampler _wbSampler = VK_NULL_HANDLE;
    bool BuildWriteback();
    void DropWriteback();
    void RecordWriteback(VkCommandBuffer cb, VkImage target, VkImageLayout externalLayout);
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

    // What OriginalFPS reports. Smoothed, because the shaders that read it use it to derive line
    // counts and carrier timing, and a rate that jitters frame to frame makes the picture jitter
    // with it. 60 until there is enough to measure.
    double _smoothedFrameMs = 0.0;
    float _originalFps = 60.0f;

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

    // Only allocated once a capture is actually asked for.
    HostBuffer _capture {};
    VkDeviceSize _captureHalf = 0;
    bool _captureArmed = false;
    bool _capturePending = false;
    uint64_t _captureSerial = 0;

    void WriteCapture();

    // Only allocated once the source detector is switched on. Two packed strips -- scanlines across
    // the frame for the horizontal period, columns down it for the vertical -- rather than the whole
    // frame, which at a 4K swapchain would be 33 MB of readback and 8 million texel conversions on
    // the game's own thread every few seconds.
    HostBuffer _probe {};
    VkDeviceSize _probeBytes = 0;
    uint32_t _probeLines = 0;      // scanlines sampled per axis
    VkDeviceSize _probeColOffset = 0;  // where the column strip starts
    bool _probeArmed = false;
    bool _probePending = false;
    bool _probeReady = false;
    GridEstimate _probeResult {};

    void ReadGridProbe();

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
