/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Small Vulkan helpers shared by the chain and, from phase 4, by the real multi-pass executor.

Everything here goes through the layer's dispatch tables rather than the loader's exported symbols,
for the reason given in vk_table.h. Nothing here allocates on a hot path: images and buffers are
built when the chain is built and reused until the raster changes.
*/

#pragma once

#include "log.h"
#include "vk_table.h"

namespace shaderglass {

// A device-local image with its own view, tracking the layout it was last left in so callers do not
// have to thread that through by hand.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A host-visible buffer, mapped for the life of the chain.
struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

inline uint32_t FindMemoryType(const InstanceTable* instance, VkPhysicalDevice phys,
                               uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp {};
    instance->vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

inline bool FormatSupports(const InstanceTable* instance, VkPhysicalDevice phys, VkFormat format,
                           VkFormatFeatureFlags features) {
    VkFormatProperties props {};
    instance->vkGetPhysicalDeviceFormatProperties(phys, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

inline void DropImage(const DeviceTable* vk, VkDevice device, Image& img) {
    if (img.view) vk->vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vk->vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vk->vkFreeMemory(device, img.memory, nullptr);
    img = Image {};
}

inline bool MakeImage(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
                      VkPhysicalDevice phys, Image& img, uint32_t w, uint32_t h, VkFormat format,
                      VkImageUsageFlags usage) {
    DropImage(vk, device, img);

    VkImageCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vk->vkCreateImage(device, &ci, nullptr, &img.image) != VK_SUCCESS) {
        Log("[chain] could not create a %ux%u image (format %d)", w, h, (int) format);
        return false;
    }

    VkMemoryRequirements req {};
    vk->vkGetImageMemoryRequirements(device, img.image, &req);

    const uint32_t type =
        FindMemoryType(instance, phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        DropImage(vk, device, img);
        return false;
    }

    VkMemoryAllocateInfo mai {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vk->vkAllocateMemory(device, &mai, nullptr, &img.memory) != VK_SUCCESS ||
        vk->vkBindImageMemory(device, img.image, img.memory, 0) != VK_SUCCESS) {
        Log("[chain] out of device memory for a %ux%u image", w, h);
        DropImage(vk, device, img);
        return false;
    }

    VkImageViewCreateInfo vi {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk->vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        DropImage(vk, device, img);
        return false;
    }

    img.format = format;
    img.width = w;
    img.height = h;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

inline void DropHostBuffer(const DeviceTable* vk, VkDevice device, HostBuffer& buf) {
    if (buf.mapped) vk->vkUnmapMemory(device, buf.memory);
    if (buf.buffer) vk->vkDestroyBuffer(device, buf.buffer, nullptr);
    if (buf.memory) vk->vkFreeMemory(device, buf.memory, nullptr);
    buf = HostBuffer {};
}

inline bool MakeHostBuffer(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
                           VkPhysicalDevice phys, HostBuffer& buf, VkDeviceSize bytes,
                           VkBufferUsageFlags usage) {
    DropHostBuffer(vk, device, buf);

    VkBufferCreateInfo bci {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk->vkCreateBuffer(device, &bci, nullptr, &buf.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req {};
    vk->vkGetBufferMemoryRequirements(device, buf.buffer, &req);

    const uint32_t type = FindMemoryType(
        instance, phys, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        DropHostBuffer(vk, device, buf);
        return false;
    }

    VkMemoryAllocateInfo mai {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vk->vkAllocateMemory(device, &mai, nullptr, &buf.memory) != VK_SUCCESS ||
        vk->vkBindBufferMemory(device, buf.buffer, buf.memory, 0) != VK_SUCCESS ||
        vk->vkMapMemory(device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped) != VK_SUCCESS) {
        DropHostBuffer(vk, device, buf);
        return false;
    }

    buf.size = bytes;
    return true;
}

// The access mask and stage that go with a layout, for the conservative barriers this layer uses.
// Narrow enough to be correct, wide enough not to need a special case per call site.
inline void AccessForLayout(VkImageLayout layout, VkAccessFlags* access,
                            VkPipelineStageFlags* stage) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            *access = 0;
            *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *access = VK_ACCESS_TRANSFER_READ_BIT;
            *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *access = VK_ACCESS_TRANSFER_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            *access = VK_ACCESS_SHADER_READ_BIT;
            *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            *access = VK_ACCESS_MEMORY_READ_BIT;
            *stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
    }
}

// Move an image this layer owns, updating its tracked layout.
inline void Transition(const DeviceTable* vk, VkCommandBuffer cb, Image& img, VkImageLayout to) {
    if (img.layout == to) return;

    VkAccessFlags srcAccess, dstAccess;
    VkPipelineStageFlags srcStage, dstStage;
    AccessForLayout(img.layout, &srcAccess, &srcStage);
    AccessForLayout(to, &dstAccess, &dstStage);

    VkImageMemoryBarrier b {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;

    vk->vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = to;
}

// Move an image this layer does not own -- the swapchain's -- where the caller states both layouts
// because nothing here tracks them.
inline void TransitionForeign(const DeviceTable* vk, VkCommandBuffer cb, VkImage image,
                              VkImageLayout from, VkImageLayout to) {
    VkAccessFlags srcAccess, dstAccess;
    VkPipelineStageFlags srcStage, dstStage;
    AccessForLayout(from, &srcAccess, &srcStage);
    AccessForLayout(to, &dstAccess, &dstStage);

    VkImageMemoryBarrier b {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;

    vk->vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace shaderglass
