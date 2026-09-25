/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "gamescope_output.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace shaderglass {

bool InGamescope() {
    static const bool in = [] {
        // SHADERGLASS_GAMESCOPE=0 keeps the composite hooks out even here, as a way back if a future
        // gamescope does something this does not expect. A path names a custom build, whatever its
        // file is called: the process running that binary is gamescope. Compared by what
        // /proc/self/exe resolves to rather than by name, because the variable is inherited by the
        // game gamescope starts, and the game must not take itself for gamescope.
        if (const char* v = getenv("SHADERGLASS_GAMESCOPE"); v && v[0]) {
            if (v[0] == '0' && !v[1]) return false;
            if (v[0] == '/') {
                char want[PATH_MAX], self[PATH_MAX];
                if (realpath(v, want) && realpath("/proc/self/exe", self))
                    return std::strcmp(want, self) == 0;
                return false;
            }
        }
        return program_invocation_short_name &&
               std::strcmp(program_invocation_short_name, "gamescope") == 0;
    }();
    return in;
}

namespace {

bool Plain2D(const VkImageCreateInfo& info) {
    return info.imageType == VK_IMAGE_TYPE_2D && info.mipLevels == 1 && info.arrayLayers == 1 &&
           info.extent.depth == 1;
}

}  // namespace

bool IsGamescopeOutputUsage(const VkImageCreateInfo& info) {
    const VkImageUsageFlags want =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return Plain2D(info) && info.usage == want;
}

bool IsGamescopeCaptureUsage(const VkImageCreateInfo& info) {
    const VkImageUsageFlags want = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return Plain2D(info) && info.usage == want;
}

void OutputTracker::OnCreateImage(VkImage image, const VkImageCreateInfo& info) {
    if (!image) return;
    TargetKind kind;
    if (IsGamescopeOutputUsage(info))
        kind = TargetKind::kOutput;
    else if (IsGamescopeCaptureUsage(info))
        kind = TargetKind::kCapture;
    else
        return;
    std::lock_guard<std::mutex> lk(_lock);
    _images[image] = {info.extent.width, info.extent.height, info.format, kind};
}

void OutputTracker::OnDestroyImage(VkImage image) {
    std::lock_guard<std::mutex> lk(_lock);
    _images.erase(image);
    for (auto it = _memoryOwner.begin(); it != _memoryOwner.end();)
        it = it->second == image ? _memoryOwner.erase(it) : std::next(it);
    for (auto it = _views.begin(); it != _views.end();)
        it = it->second == image ? _views.erase(it) : std::next(it);
}

void OutputTracker::OnBindMemory(VkImage image, VkDeviceMemory memory) {
    std::lock_guard<std::mutex> lk(_lock);
    if (!_images.count(image)) return;
    auto owner = _memoryOwner.find(memory);
    if (owner != _memoryOwner.end() && owner->second != image) {
        // Memory another tracked image already owns: a partial-composition overlay aliasing it.
        _images.erase(image);
        return;
    }
    _memoryOwner[memory] = image;
}

void OutputTracker::OnCreateView(VkImageView view, VkImage image) {
    std::lock_guard<std::mutex> lk(_lock);
    if (_images.count(image)) _views[view] = image;
}

void OutputTracker::OnDestroyView(VkImageView view) {
    std::lock_guard<std::mutex> lk(_lock);
    _views.erase(view);
}

void OutputTracker::OnUpdateSets(uint32_t count, const VkWriteDescriptorSet* writes) {
    std::lock_guard<std::mutex> lk(_lock);
    for (uint32_t i = 0; i < count; ++i) {
        const VkWriteDescriptorSet& w = writes[i];
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || !w.pImageInfo) continue;

        VkImageView tracked = VK_NULL_HANDLE;
        for (uint32_t k = 0; k < w.descriptorCount; ++k)
            if (_views.count(w.pImageInfo[k].imageView)) tracked = w.pImageInfo[k].imageView;

        auto it = _sets.find(w.dstSet);
        if (tracked) {
            _sets[w.dstSet] = {w.dstBinding, tracked};
        } else if (it != _sets.end() && it->second.binding == w.dstBinding) {
            // The same binding rewritten with something else: this set no longer targets a tracked
            // image. gamescope reuses its descriptor sets, so this is the ordinary case.
            _sets.erase(it);
        }
    }
}

void OutputTracker::OnBindSets(VkCommandBuffer cb, uint32_t count, const VkDescriptorSet* sets) {
    std::lock_guard<std::mutex> lk(_lock);
    // What is bound now replaces what was bound before: a dispatch into the upscaler's scratch after
    // one into the target must not look like another into the target.
    VkImageView bound = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i)
        if (auto it = _sets.find(sets[i]); it != _sets.end()) bound = it->second.view;
    _recordings[cb].bound = bound;
}

void OutputTracker::OnBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point,
                                   VkPipeline pipeline) {
    if (point != VK_PIPELINE_BIND_POINT_COMPUTE) return;
    std::lock_guard<std::mutex> lk(_lock);
    _recordings[cb].compute = pipeline;
}

void OutputTracker::OnCreateCommandPool(VkCommandPool pool, uint32_t family) {
    std::lock_guard<std::mutex> lk(_lock);
    _poolFamilies[pool] = family;
}

void OutputTracker::OnDestroyCommandPool(VkCommandPool pool) {
    std::lock_guard<std::mutex> lk(_lock);
    _poolFamilies.erase(pool);
    for (auto it = _pools.begin(); it != _pools.end();) {
        if (it->second == pool) {
            _recordings.erase(it->first);
            it = _pools.erase(it);
        } else {
            ++it;
        }
    }
}

void OutputTracker::OnAllocateCommandBuffers(VkCommandPool pool, uint32_t count,
                                             const VkCommandBuffer* cbs) {
    std::lock_guard<std::mutex> lk(_lock);
    for (uint32_t i = 0; i < count; ++i) _pools[cbs[i]] = pool;
}

void OutputTracker::OnFreeCommandBuffers(uint32_t count, const VkCommandBuffer* cbs) {
    std::lock_guard<std::mutex> lk(_lock);
    for (uint32_t i = 0; i < count; ++i) {
        _pools.erase(cbs[i]);
        _recordings.erase(cbs[i]);
    }
}

void OutputTracker::OnBeginCommandBuffer(VkCommandBuffer cb) {
    // Beginning resets what a command buffer did last time it was recorded.
    std::lock_guard<std::mutex> lk(_lock);
    _recordings.erase(cb);
}

bool OutputTracker::TargetOfLocked(VkCommandBuffer cb, CompositeTarget* out) const {
    auto r = _recordings.find(cb);
    if (r == _recordings.end() || !r->second.bound) return false;
    auto v = _views.find(r->second.bound);
    if (v == _views.end()) return false;
    auto img = _images.find(v->second);
    if (img == _images.end()) return false;
    *out = {v->second, r->second.bound, img->second.width, img->second.height, img->second.format,
            img->second.kind};
    return true;
}

bool OutputTracker::TargetOf(VkCommandBuffer cb, CompositeTarget* out) const {
    std::lock_guard<std::mutex> lk(_lock);
    return TargetOfLocked(cb, out);
}

bool OutputTracker::ClaimDispatch(VkCommandBuffer cb, CompositeTarget* out) {
    std::lock_guard<std::mutex> lk(_lock);
    auto r = _recordings.find(cb);
    if (r == _recordings.end() || r->second.claimed) return false;
    if (!TargetOfLocked(cb, out)) return false;
    r->second.claimed = true;
    return true;
}

VkPipeline OutputTracker::ComputePipelineOf(VkCommandBuffer cb) const {
    std::lock_guard<std::mutex> lk(_lock);
    auto r = _recordings.find(cb);
    return r == _recordings.end() ? VK_NULL_HANDLE : r->second.compute;
}

bool OutputTracker::FamilyOf(VkCommandBuffer cb, uint32_t* family) const {
    std::lock_guard<std::mutex> lk(_lock);
    auto p = _pools.find(cb);
    if (p == _pools.end()) return false;
    auto f = _poolFamilies.find(p->second);
    if (f == _poolFamilies.end()) return false;
    *family = f->second;
    return true;
}

size_t OutputTracker::TrackedImages() const {
    std::lock_guard<std::mutex> lk(_lock);
    return _images.size();
}

}  // namespace shaderglass
