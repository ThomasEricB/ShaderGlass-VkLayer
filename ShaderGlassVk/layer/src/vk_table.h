/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Derived from DLSS5VKLayer (relicensed to GPL-3.0, see RELICENSE.md).

The next layer's entry points, resolved once per instance and per device.

A layer must never call a Vulkan function through the loader's own exported symbol: that re-enters
the top of the chain and, for anything this layer hooks, recurses. Everything below therefore goes
through pfnNextGetInstanceProcAddr / pfnNextGetDeviceProcAddr, which is what these two tables hold.

The device list carries graphics-pipeline entry points as well as compute ones. Slang passes are
vertex+fragment pairs drawing a fullscreen quad, so the chain needs render passes, framebuffers and
draws -- where DLSS5VKLayer only ever dispatched compute.
*/

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace shaderglass {

#define SG_INSTANCE_FN_LIST(X)                                                                     \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceProperties2)                                                              \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceMemoryProperties2)                                                        \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkEnumerateDeviceExtensionProperties)

#define SG_DEVICE_FN_LIST(X)                                                                       \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkGetDeviceQueue2)                                                                           \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkQueuePresentKHR)                                                                           \
    X(vkQueueSubmit)                                                                               \
    X(vkQueueSubmit2)                                                                              \
    X(vkQueueWaitIdle)                                                                             \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkResetCommandPool)                                                                          \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkFreeCommandBuffers)                                                                        \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)                                                                               \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkBindBufferMemory)                                                                          \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkCmdCopyImage)                                                                              \
    X(vkCmdCopyBuffer)                                                                             \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCmdBlitImage)                                                                              \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkCmdFillBuffer)                                                                             \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreateComputePipelines)                                                                    \
    X(vkCreateGraphicsPipelines)                                                                   \
    X(vkDestroyPipeline)                                                                           \
    X(vkCreateRenderPass)                                                                          \
    X(vkDestroyRenderPass)                                                                         \
    X(vkCreateFramebuffer)                                                                         \
    X(vkDestroyFramebuffer)                                                                        \
    X(vkCmdBeginRenderPass)                                                                        \
    X(vkCmdEndRenderPass)                                                                          \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdBindVertexBuffers)                                                                      \
    X(vkCmdSetViewport)                                                                            \
    X(vkCmdSetScissor)                                                                             \
    X(vkCmdDraw)                                                                                   \
    X(vkCmdDispatch)                                                                               \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdClearColorImage)

struct InstanceTable {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
#define X(name) PFN_##name name = nullptr;
    SG_INSTANCE_FN_LIST(X)
#undef X

    void Load(VkInstance instance) {
#define X(name) name = (PFN_##name) next_gipa(instance, #name);
        SG_INSTANCE_FN_LIST(X)
#undef X
    }
};

struct DeviceTable {
    PFN_vkGetDeviceProcAddr next_dpa = nullptr;
#define X(name) PFN_##name name = nullptr;
    SG_DEVICE_FN_LIST(X)
#undef X

    void Load(VkDevice device) {
#define X(name) name = (PFN_##name) next_dpa(device, #name);
        SG_DEVICE_FN_LIST(X)
#undef X
    }
};

// What a shader chain needs to exist at all. Anything absent from this list is either optional
// (queue2, submit2, timestamps) or already checked by the caller.
inline bool DeviceTableComplete(const DeviceTable& t) {
    return t.vkCreateImage && t.vkCreateImageView && t.vkAllocateMemory && t.vkBindImageMemory &&
           t.vkCreateBuffer && t.vkBindBufferMemory && t.vkMapMemory && t.vkCreateSampler &&
           t.vkCreateShaderModule && t.vkCreateDescriptorSetLayout && t.vkCreatePipelineLayout &&
           t.vkCreateDescriptorPool && t.vkAllocateDescriptorSets && t.vkUpdateDescriptorSets &&
           t.vkCreateGraphicsPipelines && t.vkCreateRenderPass && t.vkCreateFramebuffer &&
           t.vkCmdBeginRenderPass && t.vkCmdEndRenderPass && t.vkCmdBindPipeline &&
           t.vkCmdBindDescriptorSets && t.vkCmdDraw && t.vkCmdPipelineBarrier && t.vkCmdCopyImage &&
           t.vkCmdBlitImage;
}

}  // namespace shaderglass
