/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The composite-target tracker, fed the calls gamescope 3.16 makes, in the order it makes them.

What matters is as much what it must NOT catch as what it must: gamescope creates several storage
images, and shading the wrong one -- the blur or upscale scratch, a partial-composition overlay -- or
the right one twice would put a CRT mask on something that is not the frame. Each of those is created
here next to the real targets and has to be passed over.
*/

#include "../layer/src/gamescope_output.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace shaderglass;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

template <typename T>
T H(uintptr_t n) {
    return reinterpret_cast<T>(n);
}

VkImageCreateInfo Info(VkImageUsageFlags usage, uint32_t w = 2560, uint32_t h = 1080) {
    VkImageCreateInfo i {};
    i.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i.imageType = VK_IMAGE_TYPE_2D;
    i.format = VK_FORMAT_B8G8R8A8_UNORM;
    i.extent = {w, h, 1};
    i.mipLevels = 1;
    i.arrayLayers = 1;
    i.usage = usage;
    return i;
}

void StorageWrite(OutputTracker& t, VkDescriptorSet set, uint32_t binding, VkImageView view) {
    VkDescriptorImageInfo ii {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    t.OnUpdateSets(1, &w);
}

}  // namespace

int main() {
    const VkImageUsageFlags output =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    OutputTracker t;

    // vulkan_make_output_images: three output images, each on its own memory ...
    for (uintptr_t i = 0; i < 3; ++i) {
        t.OnCreateImage(H<VkImage>(0x100 + i), Info(output));
        t.OnBindMemory(H<VkImage>(0x100 + i), H<VkDeviceMemory>(0x900 + i));
        t.OnCreateView(H<VkImageView>(0x200 + i), H<VkImage>(0x100 + i));
    }
    // ... then three partial-composition overlays, same usage, aliasing that same memory.
    for (uintptr_t i = 0; i < 3; ++i) {
        t.OnCreateImage(H<VkImage>(0x110 + i), Info(output));
        t.OnBindMemory(H<VkImage>(0x110 + i), H<VkDeviceMemory>(0x900 + i));
        t.OnCreateView(H<VkImageView>(0x210 + i), H<VkImage>(0x110 + i));
    }
    // A screenshot/PipeWire texture, a target of its own; and the look-alike, the blur and upscale
    // scratch (no TransferSrc, no TransferDst).
    t.OnCreateImage(H<VkImage>(0x121),
                    Info(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1280, 540));
    t.OnBindMemory(H<VkImage>(0x121), H<VkDeviceMemory>(0x921));
    t.OnCreateView(H<VkImageView>(0x221), H<VkImage>(0x121));
    t.OnCreateImage(H<VkImage>(0x120), Info(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT));
    t.OnCreateView(H<VkImageView>(0x220), H<VkImage>(0x120));

    std::printf("recognising the targets\n");
    Check(t.TrackedImages() == 4, "the three output images and the capture texture are tracked");

    std::printf("\nfollowing the composite\n");
    const VkDescriptorSet set = H<VkDescriptorSet>(0x300);
    const VkCommandBuffer a = H<VkCommandBuffer>(0x400), b = H<VkCommandBuffer>(0x401);
    CompositeTarget got;

    t.OnBeginCommandBuffer(a);
    StorageWrite(t, set, 1, H<VkImageView>(0x201));
    t.OnBindSets(a, 1, &set);
    Check(t.TargetOf(a, &got) && got.image == H<VkImage>(0x101) &&
              got.view == H<VkImageView>(0x201) && got.width == 2560 && got.height == 1080 &&
              got.kind == TargetKind::kOutput,
          "a composite into output image 1 is found, with the view it wrote through");

    // gamescope reuses its descriptor sets. The next dispatch rebinds the same set to the blur
    // scratch; a command buffer binding it now composites into nothing this cares about.
    t.OnBeginCommandBuffer(b);
    StorageWrite(t, set, 1, H<VkImageView>(0x220));
    t.OnBindSets(b, 1, &set);
    Check(!t.TargetOf(b, &got), "the same set rebound to the blur scratch targets nothing");
    Check(t.TargetOf(a, &got) && got.image == H<VkImage>(0x101),
          "what another command buffer has bound is its own");

    t.OnBeginCommandBuffer(a);
    Check(!t.TargetOf(a, &got), "re-recording a command buffer forgets its old target");

    std::printf("\nshading behind the right dispatch, once\n");
    const VkDescriptorSet scratch = H<VkDescriptorSet>(0x301);
    t.OnBeginCommandBuffer(a);
    // An upscaler: its first pass writes the scratch, the second the output image.
    StorageWrite(t, scratch, 1, H<VkImageView>(0x220));
    t.OnBindSets(a, 1, &scratch);
    Check(!t.ClaimDispatch(a, &got), "an upscaler's first pass, into its scratch, is not claimed");
    StorageWrite(t, set, 1, H<VkImageView>(0x200));
    t.OnBindSets(a, 1, &set);
    Check(t.ClaimDispatch(a, &got) && got.image == H<VkImage>(0x100),
          "its second pass, into the output image, is");
    // Then the same frame copied on into a PipeWire texture: already shaded, not again.
    StorageWrite(t, set, 1, H<VkImageView>(0x221));
    t.OnBindSets(a, 1, &set);
    Check(t.TargetOf(a, &got) && got.kind == TargetKind::kCapture,
          "the PipeWire blit's target is seen for what it is");
    Check(!t.ClaimDispatch(a, &got), "but a recording is shaded once, so it is not claimed");
    StorageWrite(t, scratch, 1, H<VkImageView>(0x220));
    t.OnBindSets(a, 1, &scratch);
    Check(!t.TargetOf(a, &got), "binding something untracked clears what the next dispatch writes");
    t.OnBeginCommandBuffer(a);
    StorageWrite(t, set, 1, H<VkImageView>(0x221));
    t.OnBindSets(a, 1, &set);
    Check(t.ClaimDispatch(a, &got) && got.kind == TargetKind::kCapture && got.width == 1280,
          "a recording that composites straight into a capture texture claims it");

    std::printf("\nwhat the layer needs to record there\n");
    const VkCommandPool pool = H<VkCommandPool>(0x500);
    t.OnCreateCommandPool(pool, 2);
    t.OnAllocateCommandBuffers(pool, 1, &a);
    uint32_t family = 99;
    Check(t.FamilyOf(a, &family) && family == 2, "a command buffer's queue family is its pool's");
    Check(!t.FamilyOf(b, &family), "one from an unseen pool has none");
    t.OnBindPipeline(a, VK_PIPELINE_BIND_POINT_COMPUTE, H<VkPipeline>(0x600));
    t.OnBindPipeline(a, VK_PIPELINE_BIND_POINT_GRAPHICS, H<VkPipeline>(0x601));
    Check(t.ComputePipelineOf(a) == H<VkPipeline>(0x600),
          "the compute pipeline to put back is remembered, graphics ignored");
    t.OnBeginCommandBuffer(a);
    Check(t.ComputePipelineOf(a) == VK_NULL_HANDLE && t.FamilyOf(a, &family),
          "a new recording forgets the pipeline but not the family");
    t.OnDestroyCommandPool(pool);
    Check(!t.FamilyOf(a, &family), "a destroyed pool takes its command buffers with it");

    std::printf("\nnot shading the wrong thing\n");
    t.OnBeginCommandBuffer(a);
    StorageWrite(t, set, 1, H<VkImageView>(0x211));  // a partial-composition overlay
    t.OnBindSets(a, 1, &set);
    Check(!t.TargetOf(a, &got), "a composite into a partial-composition overlay is not a target");

    std::printf("\nlifetimes\n");
    t.OnBeginCommandBuffer(a);
    StorageWrite(t, set, 1, H<VkImageView>(0x202));
    t.OnBindSets(a, 1, &set);
    t.OnDestroyImage(H<VkImage>(0x102));
    Check(!t.TargetOf(a, &got), "a destroyed output image is no longer a target");
    Check(t.TrackedImages() == 3, "and is no longer tracked");

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all gamescope-output checks passed");
    return g_failures ? 1 : 0;
}
