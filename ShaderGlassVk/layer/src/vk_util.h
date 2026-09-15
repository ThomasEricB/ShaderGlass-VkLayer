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

    // More than one when a pass declares mipmap_input over what it reads, or a preset declares
    // <texture>_mipmap. The tracked layout covers every level: the one place levels are moved
    // independently is GenerateMipmaps, which puts them all back the same way before it returns.
    uint32_t levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// How many mip levels an image of this size can have, down to 1x1.
inline uint32_t MipLevelsFor(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        ++levels;
    }
    return levels;
}

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
                      VkImageUsageFlags usage, uint32_t levels = 1) {
    DropImage(vk, device, img);
    if (levels < 1) levels = 1;

    // Every level is filled by blitting from the one above, so a mipmapped image is both a transfer
    // source and a transfer destination whatever else it is for.
    if (levels > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo ci {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
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
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (vk->vkCreateImageView(device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        DropImage(vk, device, img);
        return false;
    }

    img.format = format;
    img.width = w;
    img.height = h;
    img.levels = levels;
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img.levels, 0, 1};
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

// Fill an image's mip chain from level 0 by successive halving blits, leaving every level in
// SHADER_READ_ONLY_OPTIMAL. The caller supplies level 0's contents and its current layout; nothing
// else about the image is assumed.
//
// This is what makes mipmap_input work, and roughly half the libretro presets ask for it: a glow or
// bloom pass that samples a mip chain reads level 0 everywhere without it, which does not fail --
// it just quietly looks wrong.
inline void GenerateMipmaps(const DeviceTable* vk, VkCommandBuffer cb, Image& img) {
    if (img.levels <= 1) {
        Transition(vk, cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }

    VkImageMemoryBarrier b {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Level 0 holds what the caller produced, in whatever layout that left it; it becomes the
    // source of the first blit.
    VkAccessFlags srcAccess;
    VkPipelineStageFlags srcStage;
    AccessForLayout(img.layout, &srcAccess, &srcStage);

    b.subresourceRange.baseMipLevel = 0;
    b.oldLayout = img.layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vk->vkCmdPipelineBarrier(cb, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &b);

    // Every other level is about to be written and has never held anything, so it comes from
    // UNDEFINED. Saying otherwise -- claiming they share level 0's layout -- is the mistake that
    // leaves them undefined at draw time.
    b.subresourceRange.baseMipLevel = 1;
    b.subresourceRange.levelCount = img.levels - 1;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    b.subresourceRange.levelCount = 1;

    int32_t w = int32_t(img.width), h = int32_t(img.height);
    for (uint32_t level = 1; level < img.levels; ++level) {
        const int32_t nw = w > 1 ? w / 2 : 1;
        const int32_t nh = h > 1 ? h / 2 : 1;

        VkImageBlit blit {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vk->vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        // Written; now it is the next blit's source, unless it is the last level.
        if (level + 1 < img.levels) {
            b.subresourceRange.baseMipLevel = level;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &b);
        }

        w = nw;
        h = nh;
    }

    // Levels 0..n-2 ended as blit sources, the last as a blit destination.
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = img.levels - 1;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    b.subresourceRange.baseMipLevel = img.levels - 1;
    b.subresourceRange.levelCount = 1;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

}  // namespace shaderglass
