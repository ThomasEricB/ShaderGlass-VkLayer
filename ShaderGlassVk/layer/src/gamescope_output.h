/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Finding gamescope's composited frame from inside gamescope, on every backend.

gamescope composites each frame with a compute shader into a storage image, then hands the image on.
How it hands it on depends on the backend, and only one of them goes through Vulkan:

  SDL       the output images are swapchain images; the frame is presented, and the layer's present
            hook shades it like any other game. Nothing here is involved.
  Wayland   the image is passed to the host compositor as a buffer.
  DRM       the image is scanned out by KMS.
  OpenVR    the image is handed to the VR compositor.
  headless  the image goes nowhere. The only pixels headless ever produces are PipeWire streams and
            screenshots -- which every backend also makes, by compositing again into a texture of
            their own.

None but SDL calls vkQueuePresentKHR, so there is nothing for a present hook to see. What is always
there is the composite dispatch, and this finds it (gamescope 3.16, rendervulkan.cpp):

  1. vkCreateImage. The output images are made with usage Sampled | Storage | TransferSrc and nothing
     else; the screenshot and PipeWire textures with Storage | TransferDst and nothing else. No other
     image gamescope creates matches either: its blur and upscale scratch is Sampled | Storage.
  2. vkBindImageMemory. Its partial-composition overlays have the output images' usage but alias
     their memory. An image bound to memory another tracked image already owns is one of those, and
     is dropped -- shading an overlay-only frame would shade the overlay, not the picture.
  3. vkCreateImageView, vkUpdateDescriptorSets, vkCmdBindDescriptorSets. The target is bound as a
     storage image, in a set gamescope binds right before every dispatch; following view -> set ->
     command buffer says which image the next dispatch writes, and through which view.
  4. vkCmdDispatch. The first dispatch in a recording that writes a tracked image is the composite --
     an upscaler's first pass writes its scratch, not the target. The layer records its chain into
     gamescope's command buffer right behind it, so everything after -- the conversion of a PipeWire
     frame to NV12, the hand-off to the backend -- sees the shaded frame. Only one per recording: a
     composite that goes on to copy its output into a PipeWire texture would otherwise be shaded twice.

A frame gamescope scans out directly -- a fullscreen game with nothing on top -- is never composited,
and so never passes through here. gamescope's --force-composition, or `gamescopectl composite_force 1`
on one that is already running, turns that off.

Pure bookkeeping: Vulkan types, no Vulkan calls, so the sequence can be tested without gamescope.
*/

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <mutex>
#include <unordered_map>

namespace shaderglass {

// True in gamescope's own process: one named "gamescope", or the one running the binary that
// SHADERGLASS_GAMESCOPE names by path, for a custom build under another name. The hooks this module
// needs are hot paths in any other program, so the layer only installs them here -- everywhere else
// they are not in the chain at all.
bool InGamescope();

// Exactly the usage gamescope gives its output images.
bool IsGamescopeOutputUsage(const VkImageCreateInfo& info);

// Exactly the usage gamescope gives its screenshot and PipeWire textures. The chain reads its target
// with a blit, which these cannot be read by as made; the layer adds TransferSrc when it creates them.
bool IsGamescopeCaptureUsage(const VkImageCreateInfo& info);

enum class TargetKind { kOutput, kCapture };

struct CompositeTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // the storage view gamescope writes through
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    TargetKind kind = TargetKind::kOutput;
};

class OutputTracker {
  public:
    void OnCreateImage(VkImage image, const VkImageCreateInfo& info);
    void OnDestroyImage(VkImage image);
    void OnBindMemory(VkImage image, VkDeviceMemory memory);
    void OnCreateView(VkImageView view, VkImage image);
    void OnDestroyView(VkImageView view);
    void OnUpdateSets(uint32_t count, const VkWriteDescriptorSet* writes);
    void OnBindSets(VkCommandBuffer cb, uint32_t count, const VkDescriptorSet* sets);
    void OnBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline);
    void OnCreateCommandPool(VkCommandPool pool, uint32_t family);
    void OnDestroyCommandPool(VkCommandPool pool);
    void OnAllocateCommandBuffers(VkCommandPool pool, uint32_t count, const VkCommandBuffer* cbs);
    void OnFreeCommandBuffers(uint32_t count, const VkCommandBuffer* cbs);
    void OnBeginCommandBuffer(VkCommandBuffer cb);

    // The tracked image the next dispatch recorded into this command buffer writes, if any.
    bool TargetOf(VkCommandBuffer cb, CompositeTarget* out) const;

    // TargetOf, for a dispatch the layer is about to shade behind -- once per recording, so the
    // second time it says no.
    bool ClaimDispatch(VkCommandBuffer cb, CompositeTarget* out);

    // The compute pipeline gamescope last bound here, which the layer's own compute work displaces
    // and has to put back.
    VkPipeline ComputePipelineOf(VkCommandBuffer cb) const;

    // The queue family of the pool the command buffer came from.
    bool FamilyOf(VkCommandBuffer cb, uint32_t* family) const;

    size_t TrackedImages() const;

  private:
    struct Image {
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        TargetKind kind = TargetKind::kOutput;
    };
    struct SetTarget {
        uint32_t binding = 0;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Recording {
        VkImageView bound = VK_NULL_HANDLE;
        VkPipeline compute = VK_NULL_HANDLE;
        bool claimed = false;
    };

    bool TargetOfLocked(VkCommandBuffer cb, CompositeTarget* out) const;

    mutable std::mutex _lock;
    std::unordered_map<VkImage, Image> _images;
    std::unordered_map<VkDeviceMemory, VkImage> _memoryOwner;
    std::unordered_map<VkImageView, VkImage> _views;
    std::unordered_map<VkDescriptorSet, SetTarget> _sets;
    std::unordered_map<VkCommandBuffer, Recording> _recordings;
    std::unordered_map<VkCommandPool, uint32_t> _poolFamilies;
    std::unordered_map<VkCommandBuffer, VkCommandPool> _pools;
};

}  // namespace shaderglass
