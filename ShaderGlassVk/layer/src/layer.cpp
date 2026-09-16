/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Derived from DLSS5VKLayer (relicensed to GPL-3.0, see RELICENSE.md).

VK_LAYER_SHADERGLASS -- the Linux side of ShaderGlass.

The layer is loaded into the game's own process and intercepts vkQueuePresentKHR, where it already
holds the finished swapchain image. That is the whole reason this exists: no window capture, no
overlay, no compositing, and no second process.

Phase 1 is the skeleton. It attaches to the shared mapping, chooses which swapchain drives the
effect, publishes status the interface can read, and presents every frame untouched. The shader chain
arrives in phase 2; nothing here decides what a frame looks like yet.

Enabled by SHADERGLASS=1, through the manifest's enable_environment, so the layer is inert in every
process that does not ask for it.
*/

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../common/shm_protocol.h"
#include "chain.h"
#include "log.h"
#include "vk_table.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace shaderglass;

// The layer's name has to differ per architecture.
//
// The loader keys implicit layers by name, so two manifests claiming the same name are one layer to
// it: it keeps whichever it read first and then rejects it for the process's word size, reporting
// only "was wrong bit-type" with the manifest that would have worked sitting unread beside it. Steam
// answered this the same way, and so does gamescope's own WSI layer (VK_LAYER_FROG_gamescope_wsi_x86_64).
#ifdef SHADERGLASS_LAYER_32
#define VK_LAYER_NAME "VK_LAYER_SHADERGLASS_32"
#else
#define VK_LAYER_NAME "VK_LAYER_SHADERGLASS"
#endif

namespace {

// ---------------------------------------------------------------------------
// Shared memory
// ---------------------------------------------------------------------------
struct ShmMap {
    int fd = -1;
    ShmHeader* hdr = nullptr;
    std::string path;
    uint32_t lastControlSeq = 0;
};

// /tmp is world-writable, so it is worth checking that what we are about to open really is ours: a
// directory, owned by this uid, with nothing granted to anyone else. Anything else and we refuse
// rather than create the file inside it.
bool EnsureParentDir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string dir = path.substr(0, slash);

    size_t pos = 1;
    while ((pos = dir.find('/', pos)) != std::string::npos) {
        mkdir(dir.substr(0, pos).c_str(), 0700);
        pos += 1;
    }
    mkdir(dir.c_str(), 0700);

    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) {
        Log("[shm] %s is missing", dir.c_str());
        return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        Log("[shm] refusing %s: it is not a private directory owned by this user", dir.c_str());
        return false;
    }
    return true;
}

bool ShmOpen(ShmMap& s) {
    if (s.hdr) return true;

    const char* env = getenv("SHADERGLASS_SHM");
    const std::string p = (env && *env) ? env : ShmDefaultPath();
    if (!EnsureParentDir(p)) return false;

    const int fd = open(p.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) {
        Log("[shm] could not open %s", p.c_str());
        return false;
    }

    struct stat st {};
    if (fstat(fd, &st) != 0 || (size_t) st.st_size < ShmTotalBytes()) {
        if (ftruncate(fd, (off_t) ShmTotalBytes()) != 0) {
            close(fd);
            return false;
        }
    }

    void* m = mmap(nullptr, ShmTotalBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        Log("[shm] mmap of %s failed", p.c_str());
        close(fd);
        return false;
    }

    s.fd = fd;
    s.hdr = (ShmHeader*) m;
    s.path = p;

    // A mapping left by an older build has a different magic or version; re-initialising is the only
    // safe reading of either. Say so loudly, because a live process on the other side of a mismatch
    // keeps re-initialising it the other way and the two then silently reset each other forever --
    // which is indistinguishable from "the interface does nothing".
    if (s.hdr->magic.load() != kShmMagic || s.hdr->version.load() != kShmVersion) {
        if (s.hdr->magic.load() == kShmMagic)
            Log("[shm] header is version %u but this layer is v%u -- another process is out of date, "
                "re-initialising it; update the layer and the interface together",
                s.hdr->version.load(), kShmVersion);
        ShmInitDefaults(s.hdr);
    }

    s.lastControlSeq = s.hdr->controlSeq.load();
    Log("[shm] attached %s", p.c_str());
    return true;
}

void ShmClose(ShmMap& s) {
    if (s.hdr) munmap((void*) s.hdr, ShmTotalBytes());
    if (s.fd >= 0) close(s.fd);
    s.hdr = nullptr;
    s.fd = -1;
}

// ---------------------------------------------------------------------------
// Which formats the chain can work in
// ---------------------------------------------------------------------------
// Every internal surface uses the format's UNORM twin rather than the swapchain's own: sampling an
// _SRGB view decodes to linear on the way in and re-encodes on the way out, and the chain wants
// exactly the numbers the game wrote.
VkFormat ChainFormat(VkFormat swapchainFormat) {
    switch (swapchainFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

bool SupportedFormat(VkFormat f) { return ChainFormat(f) != VK_FORMAT_UNDEFINED; }

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------
struct InstanceChain {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    InstanceTable table;

    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
};

struct SwapchainState {
    std::vector<VkImage> images;
    VkQueue queue = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t width = 0, height = 0;

    // This swapchain is not a candidate for the effect at all: an unsupported format, or a raster
    // outside what the chain will build for. It still presents, untouched.
    bool passThrough = false;

    // The chain and the resources it is recorded into. Built on the first frame this swapchain is
    // chosen to drive the effect, so a process that never composes pays for none of it.
    std::unique_ptr<Chain> chain;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // The command buffer and every surface it reads are reused next frame, so the fence has to be
    // collected before re-recording. Waited on at the top of the next frame rather than after the
    // submit, which keeps the game's thread out of the GPU's way for the whole of the chain.
    bool fencePending = false;

    bool resourcesReady = false;
    bool resourcesFailed = false;
};

struct DeviceChain {
    InstanceChain* instance = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice self = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr next_dpa = nullptr;
    DeviceTable table;

    // The loader's hook for installing a dispatch table on a dispatchable object a layer creates.
    PFN_vkSetDeviceLoaderData setDeviceLoaderData = nullptr;

    // The last captureRequest seen. The interface only ever increments it, so any difference is a
    // request; storing the count rather than a flag means a second request while the first is still
    // in flight is not lost.
    uint32_t captureSeen = 0;

    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 vkGetDeviceQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueSubmit2 vkQueueSubmit2 = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;

    // Latched when the device is lost, or when the chain proves unusable here. After that the
    // fail-open path is the only correct one and it costs nothing to take it directly.
    std::atomic<bool> inert {false};

    std::mutex lock;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
    std::unordered_map<VkQueue, uint32_t> queueFamilies;

    ShmMap shm;
    uint64_t framesSeen = 0;
    double lastFpsSampleMs = 0.0;
    uint64_t lastFpsFrames = 0;
};

std::unordered_map<VkInstance, InstanceChain> g_instances;
std::unordered_map<VkPhysicalDevice, InstanceChain*> g_phys;
std::unordered_map<VkDevice, DeviceChain*> g_devices;
std::mutex g_stateMutex;

// The one swapchain allowed to drive the effect, chosen as the largest in the process.
//
// A process can present more than one swapchain -- the game window and an overlay, or, mid-resize,
// the old and new windows at once. Only one of them is the game. The record is global rather than
// per-device because an overlay may build its own VkDevice.
struct PrimarySwap {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t area = 0;
};
PrimarySwap g_primary;

// Its own mutex, never nested with dc->lock or g_stateMutex, so the lock order in the present hook
// cannot invert against the device hooks.
std::mutex g_primaryMutex;

bool ClaimPrimary(VkDevice device, VkSwapchainKHR swapchain, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    const uint64_t area = uint64_t(w) * h;
    if (g_primary.swapchain == swapchain && g_primary.device == device) return true;
    if (g_primary.swapchain != VK_NULL_HANDLE && area <= g_primary.area) return false;
    g_primary.device = device;
    g_primary.swapchain = swapchain;
    g_primary.area = area;
    return true;
}

void ReleasePrimary(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    if (g_primary.swapchain == swapchain && g_primary.device == device) g_primary = PrimarySwap {};
}

// Where this copy of the layer was loaded from, for the duplicate check below.
std::string LayerObjectPath() {
    Dl_info info {};
    if (dladdr((const void*) &LayerObjectPath, &info) && info.dli_fname && *info.dli_fname)
        return info.dli_fname;
    return std::string();
}

// True when a *different* copy of this layer is already in the chain.
//
// A build-tree manifest and an installed manifest both being honoured means two copies of the layer,
// two present hooks and two writers racing on one mapping. Only the first copy stays live; the rest
// declare themselves inert and pass everything through, which turns a corrupted picture or a hang
// into one warning line. The claim is the object's own path rather than a bare flag, so a second call
// into the same copy -- legal, the loader may negotiate more than once -- is told apart from a second
// copy.
bool DuplicateLayerCopy() {
    static const bool dup = [] {
        const std::string self = LayerObjectPath();
        const char* claimed = getenv("SHADERGLASS_LAYER_OBJECT");
        if (claimed && *claimed) {
            if (self.empty() || self == claimed) return false;
            Log("[layer] another copy is already loaded from %s; this copy (%s) stays inert. "
                "Remove one of the implicit-layer manifests.",
                claimed, self.c_str());
            return true;
        }
        if (!self.empty()) setenv("SHADERGLASS_LAYER_OBJECT", self.c_str(), 0);
        return false;
    }();
    return dup;
}

bool LayerEnabled() {
    static const bool e = [] {
        if (DuplicateLayerCopy()) return false;
        const char* v = getenv("SHADERGLASS");
        return v && v[0] == '1';
    }();
    return e;
}

// Every Vulkan result on the present path, looked at rather than collapsed into a bool.
// Unused until phase 2 gives the layer calls of its own to check.
//
// A failure here would otherwise be indistinguishable from "nothing to do": the call returns false,
// the caller presents the original frame, and the next frame tries exactly the same thing again. That
// is right for a transient failure and wrong for VK_ERROR_DEVICE_LOST, where the device is gone,
// every subsequent call fails the same way, and the log fills with it.
[[maybe_unused]] bool NoteVk(DeviceChain* dc, VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    if (r == VK_ERROR_DEVICE_LOST) {
        if (!dc->inert.exchange(true))
            Log("[layer] %s -> DEVICE_LOST; layer inert for this device", what);
        return false;
    }
    static std::atomic<uint32_t> reported {0};
    if (reported.fetch_add(1) < 8) Log("[layer] %s -> %d", what, (int) r);
    return false;
}

// Give a dispatchable object this layer allocated the dispatch table the loader expects on it.
// Unused until phase 2 allocates a command buffer.
//
// VkCommandBuffer and VkQueue are dispatchable: their first word points at a dispatch table, and
// every layer below reads it to find its own state for that object. The loader fills that word in for
// objects the application allocates through the trampoline -- but a layer that allocates one by
// calling straight down the chain bypasses the trampoline, so the word keeps whatever the ICD left
// there. Calling this is mandatory, not advisory: skipping it is invisible with no other layer
// present and an abort the moment any second layer reads the word. On a machine carrying gamescope's
// WSI layer, MangoHud and obs_vkcapture, that is not a hypothetical.
[[maybe_unused]] bool SetLoaderData(DeviceChain* dc, void* object) {
    if (!dc->setDeviceLoaderData) return true;  // no loader in the chain; nothing to fill in
    return dc->setDeviceLoaderData(dc->self, object) == VK_SUCCESS;
}

DeviceChain* FindDevice(VkDevice device) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

DeviceChain* DeviceForQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    if (g_devices.size() == 1) return g_devices.begin()->second;
    for (auto& kv : g_devices) {
        std::lock_guard<std::mutex> dl(kv.second->lock);
        if (kv.second->queueFamilies.count(queue)) return kv.second;
    }
    return nullptr;
}

void RememberQueue(DeviceChain* dc, VkQueue queue, uint32_t family) {
    if (!queue) return;
    std::lock_guard<std::mutex> lk(dc->lock);
    dc->queueFamilies[queue] = family;
}

// ---------------------------------------------------------------------------
// Chain resources
// ---------------------------------------------------------------------------
void DestroySwapchainResources(DeviceChain* dc, SwapchainState& sc) {
    // Nothing of ours may be in flight against the surfaces the chain is about to free.
    if (sc.fencePending && dc->table.vkWaitForFences)
        dc->table.vkWaitForFences(dc->self, 1, &sc.fence, VK_TRUE, UINT64_MAX);
    sc.fencePending = false;

    sc.chain.reset();
    if (sc.fence) dc->table.vkDestroyFence(dc->self, sc.fence, nullptr);
    if (sc.pool) dc->table.vkDestroyCommandPool(dc->self, sc.pool, nullptr);
    sc.fence = VK_NULL_HANDLE;
    sc.pool = VK_NULL_HANDLE;
    sc.cb = VK_NULL_HANDLE;
    sc.resourcesReady = false;
}

bool CreateSwapchainResources(DeviceChain* dc, SwapchainState& sc, uint32_t family) {
    VkCommandPoolCreateInfo cpci {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    if (dc->table.vkCreateCommandPool(dc->self, &cpci, nullptr, &sc.pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo cbai {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = sc.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (dc->table.vkAllocateCommandBuffers(dc->self, &cbai, &sc.cb) != VK_SUCCESS) return false;

    // Mandatory for a dispatchable object a layer allocated; see SetLoaderData.
    if (!SetLoaderData(dc, sc.cb)) {
        Log("[layer] vkSetDeviceLoaderData failed for the chain's command buffer");
        return false;
    }

    VkFenceCreateInfo fci {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (dc->table.vkCreateFence(dc->self, &fci, nullptr, &sc.fence) != VK_SUCCESS) return false;

    sc.chain = std::make_unique<Chain>(&dc->table, &dc->instance->table, dc->self, dc->physical);
    if (!sc.chain->Usable()) {
        Log("[layer] the chain cannot run here: %s", sc.chain->Reason());
        sc.chain.reset();
        return false;
    }

    sc.resourcesReady = true;
    return true;
}

// Run the chain over one presented image.
//
// The caller's present semaphores are consumed by this submit, because this submit is the first
// thing to touch the image. They are therefore unsignalled by the time this returns and must not be
// handed to vkQueuePresentKHR again -- a binary semaphore may be waited on once per signal, and
// presenting with it a second time is a wait that never completes.
//
// Every path out leaves the swapchain image in PRESENT_SRC_KHR, including the ones that give up.
bool ProcessPresent(DeviceChain* dc, SwapchainState& sc, VkQueue queue, VkImage image,
                    uint32_t waitCount, const VkSemaphore* waits, uint32_t sourceW,
                    uint32_t sourceH, const std::string& presetId) {
    // The previous frame's chain, if it is still running, must finish before anything here touches
    // the surfaces it reads or the command buffer it was recorded into.
    if (sc.fencePending) {
        if (!NoteVk(dc, dc->table.vkWaitForFences(dc->self, 1, &sc.fence, VK_TRUE, UINT64_MAX),
                    "vkWaitForFences"))
            return false;
        dc->table.vkResetFences(dc->self, 1, &sc.fence);
        sc.fencePending = false;

        // That fence retiring is what lets the chain release what the previous frame was still
        // using, and read back what it recorded.
        sc.chain->FrameCompleted();
    }

    if (!sc.chain->Prepare(sc.width, sc.height, sc.format, sourceW, sourceH, presetId))
        return false;

    // Parameter values are read every frame rather than watched, because they are cheap to read and
    // the alternative is a second sequence number to get wrong. Prepare has already folded in the
    // preset's own defaults, so this only moves what the interface has changed.
    if (dc->shm.hdr) {
        const uint32_t count =
            std::min(dc->shm.hdr->paramCount.load(std::memory_order_relaxed), kMaxParams);
        sc.chain->ApplyParameters(dc->shm.hdr->params, count);
    }

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!NoteVk(dc, dc->table.vkBeginCommandBuffer(sc.cb, &bi), "vkBeginCommandBuffer")) return false;

    if (!sc.chain->Record(sc.cb, image, dc->framesSeen)) {
        dc->table.vkEndCommandBuffer(sc.cb);
        return false;
    }
    if (!NoteVk(dc, dc->table.vkEndCommandBuffer(sc.cb), "vkEndCommandBuffer")) return false;

    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc.cb;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitCount ? waits : nullptr;
    si.pWaitDstStageMask = waitCount ? stages.data() : nullptr;

    // No wait here. The present that follows runs on this same queue behind these commands, so the
    // GPU orders them without the CPU ever parking; the fence is collected at the top of the next
    // frame, where the reused surfaces actually need it.
    if (!NoteVk(dc, dc->table.vkQueueSubmit(queue, 1, &si, sc.fence), "vkQueueSubmit")) return false;

    sc.fencePending = true;
    return true;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
// What the interface reads. Restated every present rather than announced once, because the mapping
// can be re-initialised by the interface or the control tool at any moment and a layer that announced
// itself once would be erased and never correct the record.
void PublishStatus(DeviceChain* dc, const SwapchainState& sc, uint32_t state) {
    ShmHeader* h = dc->shm.hdr;
    if (!h) return;

    h->layerPid.store((uint32_t) getpid());
    h->layerState.store(state);
    ShmStore64(h->layerFramesLo, h->layerFramesHi, dc->framesSeen);
    h->layerHeartbeat.fetch_add(1);

    h->swapWidth.store(sc.width);
    h->swapHeight.store(sc.height);
    h->swapFormat.store((uint32_t) sc.format);

    // The rasters the chain actually resolved, when there is one; otherwise what it would resolve,
    // from the same helper the interface uses, so both sides always agree on the number.
    if (sc.chain && sc.chain->PassCount() > 0) {
        h->sourceActualWidth.store(sc.chain->SourceWidth());
        h->sourceActualHeight.store(sc.chain->SourceHeight());
        h->outputActualWidth.store(sc.chain->OutputWidth());
        h->outputActualHeight.store(sc.chain->OutputHeight());
        h->passCount.store(sc.chain->PassCount());
    } else {
        uint32_t sw = 0, sh = 0;
        ShmSourceExtent(h, sc.width, sc.height, &sw, &sh);
        h->sourceActualWidth.store(sw);
        h->sourceActualHeight.store(sh);
        h->outputActualWidth.store(sc.width);
        h->outputActualHeight.store(sc.height);
        h->passCount.store(0);
    }

    const double now = NowMs();
    if (dc->lastFpsSampleMs == 0.0) {
        dc->lastFpsSampleMs = now;
        dc->lastFpsFrames = dc->framesSeen;
    } else if (now - dc->lastFpsSampleMs >= 500.0) {
        const double fps =
            double(dc->framesSeen - dc->lastFpsFrames) * 1000.0 / (now - dc->lastFpsSampleMs);
        h->fpsBits.store(FloatToBits((float) fps));
        dc->lastFpsSampleMs = now;
        dc->lastFpsFrames = dc->framesSeen;
    }
}

// Publish the process name once, so the interface can say which game it is looking at.
void PublishGameName(DeviceChain* dc) {
    ShmHeader* h = dc->shm.hdr;
    if (!h) return;
    char buf[kNameBytes] = {};
    FILE* f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            char* nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
        }
        fclose(f);
    }
    if (buf[0]) ShmStoreString(h->gameNameSeq, h->gameName, kNameBytes, buf);
}

// ---------------------------------------------------------------------------
// Instance hooks
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                   const VkAllocationCallbacks* pAllocator,
                                                   VkInstance* pInstance) {
    auto* link =
        const_cast<VkLayerInstanceCreateInfo*>((const VkLayerInstanceCreateInfo*) pCreateInfo->pNext);
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo*) link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = (PFN_vkCreateInstance) next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // Documented pattern: keep the link node in pNext (layers below need it) and advance
    // u.pLayerInfo so the next layer resolves its own chain entry.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const VkResult res = create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceChain chain {};
    chain.next_gipa = next_gipa;
    chain.vkDestroyInstance = (PFN_vkDestroyInstance) next_gipa(*pInstance, "vkDestroyInstance");
    chain.vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices) next_gipa(*pInstance, "vkEnumeratePhysicalDevices");
    chain.vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties) next_gipa(*pInstance, "vkGetPhysicalDeviceProperties");
    chain.vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties) next_gipa(*pInstance,
                                                             "vkEnumerateDeviceExtensionProperties");
    chain.table.next_gipa = next_gipa;
    chain.table.Load(*pInstance);

    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_instances[*pInstance] = chain;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance,
                                                const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return;
    auto destroy = it->second.vkDestroyInstance;
    InstanceChain* chain = &it->second;
    g_instances.erase(it);
    for (auto pit = g_phys.begin(); pit != g_phys.end();)
        pit = (pit->second == chain) ? g_phys.erase(pit) : std::next(pit);
    if (destroy) destroy(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount,
                                                             VkPhysicalDevice* pPhysicalDevices) {
    InstanceChain* chain = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) chain = &it->second;
    }
    if (!chain || !chain->vkEnumeratePhysicalDevices) return VK_ERROR_INITIALIZATION_FAILED;

    const VkResult res = chain->vkEnumeratePhysicalDevices(instance, pCount, pPhysicalDevices);
    if (res == VK_SUCCESS && pPhysicalDevices) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        for (uint32_t i = 0; i < *pCount; ++i) g_phys[pPhysicalDevices[i]] = chain;
    }
    return res;
}

// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(VkPhysicalDevice physicalDevice,
                                                 const VkDeviceCreateInfo* pCreateInfo,
                                                 const VkAllocationCallbacks* pAllocator,
                                                 VkDevice* pDevice) {
    auto* link =
        const_cast<VkLayerDeviceCreateInfo*>((const VkLayerDeviceCreateInfo*) pCreateInfo->pNext);
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo*) link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_dpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create = (PFN_vkCreateDevice) next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // A second node in the same chain carries vkSetDeviceLoaderData; see SetLoaderData.
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (const auto* n = (const VkLayerDeviceCreateInfo*) pCreateInfo->pNext; n;
         n = (const VkLayerDeviceCreateInfo*) n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            n->function == VK_LOADER_DATA_CALLBACK) {
            setLoaderData = n->u.pfnSetDeviceLoaderData;
            break;
        }
    }

    InstanceChain* ic = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_phys.find(physicalDevice);
        if (it != g_phys.end()) ic = it->second;
    }

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const VkResult res = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    auto* dc = new DeviceChain();
    dc->instance = ic;
    dc->physical = physicalDevice;
    dc->self = *pDevice;
    dc->next_dpa = next_dpa;
    dc->setDeviceLoaderData = setLoaderData;

#define SG_LOAD(name) dc->name = (PFN_##name) next_dpa(*pDevice, #name);
    SG_LOAD(vkDestroyDevice)
    SG_LOAD(vkGetDeviceQueue)
    SG_LOAD(vkGetDeviceQueue2)
    SG_LOAD(vkCreateSwapchainKHR)
    SG_LOAD(vkDestroySwapchainKHR)
    SG_LOAD(vkGetSwapchainImagesKHR)
    SG_LOAD(vkAcquireNextImageKHR)
    SG_LOAD(vkQueuePresentKHR)
    SG_LOAD(vkQueueSubmit)
    SG_LOAD(vkQueueSubmit2)
    SG_LOAD(vkQueueWaitIdle)
    SG_LOAD(vkDeviceWaitIdle)
#undef SG_LOAD

    dc->table.next_dpa = next_dpa;
    dc->table.Load(*pDevice);

    if (!dc->vkQueuePresentKHR || !dc->vkCreateSwapchainKHR || !ic) dc->inert = true;

    // No vendor check. DLSS5VKLayer went inert on non-NVIDIA devices because NGX is NVIDIA-only;
    // shader chains are not, so this runs on AMD, Intel and NVIDIA alike.
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "?";
    if (ic && ic->vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props {};
        ic->vkGetPhysicalDeviceProperties(physicalDevice, &props);
        snprintf(deviceName, sizeof(deviceName), "%s", props.deviceName);
    }

    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_devices[*pDevice] = dc;
    Log("[layer] device %p on %s (inert=%d enabled=%d)", (void*) *pDevice, deviceName,
        (int) dc->inert.load(), (int) LayerEnabled());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device,
                                              const VkAllocationCallbacks* pAllocator) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) {
            dc = it->second;
            g_devices.erase(it);
        }
    }
    if (!dc) return;

    if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) {
            ReleasePrimary(device, kv.first);
            DestroySwapchainResources(dc, kv.second);
        }
        dc->swapchains.clear();
    }

    // Say the layer has gone. A reader checks the pid is alive, so a crash is caught too -- but an
    // orderly exit should not need anyone to go looking.
    if (dc->shm.hdr && dc->shm.hdr->layerPid.load() == (uint32_t) getpid()) {
        dc->shm.hdr->layerPid.store(0);
        dc->shm.hdr->layerState.store(kLayerDetached);
    }
    ShmClose(dc->shm);

    if (dc->vkDestroyDevice) dc->vkDestroyDevice(device, pAllocator);
    delete dc;
}

VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                               VkQueue* pQueue) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkGetDeviceQueue) return;
    dc->vkGetDeviceQueue(device, family, index, pQueue);
    RememberQueue(dc, *pQueue, family);
}

// The 1.1 way of asking for a queue, and the only way to reach one created with
// VkDeviceQueueCreateFlags. A game that uses it would otherwise never register its queue, and the
// present path could not tell which family the queue belongs to.
VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2* pQueueInfo,
                                                VkQueue* pQueue) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkGetDeviceQueue2) return;
    dc->vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueueInfo) RememberQueue(dc, *pQueue, pQueueInfo->queueFamilyIndex);
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(VkDevice device,
                                                       const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkSwapchainKHR* pSwapchain) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkCreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    // The chain has to read the presented image and write the result back, so the swapchain needs
    // transfer usage on both ends. Added on the way down, and only when the layer is armed and the
    // format is one we can work in; a driver that refuses is handled below.
    VkSwapchainCreateInfoKHR m = *pCreateInfo;
    const bool wantUsage =
        !dc->inert && LayerEnabled() && SupportedFormat(pCreateInfo->imageFormat);
    if (wantUsage) m.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = dc->vkCreateSwapchainKHR(device, &m, pAllocator, pSwapchain);
    if (res != VK_SUCCESS && wantUsage) {
        // Nothing we add is worth failing a swapchain creation over.
        Log("[layer] swapchain refused the added transfer usage (%d); retrying with the game's own",
            (int) res);
        res = dc->vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (res != VK_SUCCESS || dc->inert || !LayerEnabled()) return res;

    uint32_t count = 0;
    dc->vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    dc->vkGetSwapchainImagesKHR(device, *pSwapchain, &count, images.data());

    SwapchainState sc {};
    sc.images = std::move(images);
    sc.format = pCreateInfo->imageFormat;
    sc.colorSpace = pCreateInfo->imageColorSpace;
    sc.width = pCreateInfo->imageExtent.width;
    sc.height = pCreateInfo->imageExtent.height;

    const bool tooSmall = sc.width < kMinW || sc.height < kMinH;
    const bool tooLarge = sc.width > kMaxW || sc.height > kMaxH;
    sc.passThrough = !SupportedFormat(sc.format) || tooSmall || tooLarge;

    std::lock_guard<std::mutex> lk(dc->lock);
    Log("[layer] swapchain %p %ux%u fmt=%d%s", (void*) *pSwapchain, sc.width, sc.height,
        (int) sc.format,
        sc.passThrough ? (!SupportedFormat(sc.format) ? " (unsupported format, passing through)"
                          : tooSmall               ? " (too small, passing through)"
                                                   : " (too large, passing through)")
                       : "");
    dc->swapchains[*pSwapchain] = std::move(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                    const VkAllocationCallbacks* pAllocator) {
    DeviceChain* dc = FindDevice(device);
    if (!dc) return;

    ReleasePrimary(device, swapchain);
    {
        std::unique_lock<std::mutex> lk(dc->lock);
        auto it = dc->swapchains.find(swapchain);
        if (it != dc->swapchains.end()) {
            // Outside the lock: a device-wide wait must not be taken while holding a lock the
            // present path also wants.
            lk.unlock();
            if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
            lk.lock();
            DestroySwapchainResources(dc, it->second);
            dc->swapchains.erase(it);
        }
    }
    if (dc->vkDestroySwapchainKHR) dc->vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t timeout, VkSemaphore semaphore,
                                                        VkFence fence, uint32_t* pImageIndex) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkAcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* pPresentInfo) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;

    if (dc->inert || !LayerEnabled()) return dc->vkQueuePresentKHR(queue, pPresentInfo);

    // Held to the end, alongside the submit hooks. Vulkan requires external synchronization for every
    // operation on a queue, and phase 6's idle repaint will submit on the application's own queue.
    std::lock_guard<std::mutex> lk(dc->lock);

    if (!ShmOpen(dc->shm)) return dc->vkQueuePresentKHR(queue, pPresentInfo);

    static std::once_flag namePublished;
    std::call_once(namePublished, [dc] { PublishGameName(dc); });

    const bool enabled = ShmEnabled(dc->shm.hdr);
    const bool paused = dc->shm.hdr->paused.load() != 0;
    const std::string preset = ShmPresetId(dc->shm.hdr);

    // No preset means no effect -- the layer costs a hook and nothing else. That is the default, and
    // deliberately not "apply something so the user sees it work".
    //
    // Phase 2 has no catalogue yet, so any preset id selects the built-in passthrough. What is being
    // proved here is the path, not the picture: the frame goes swapchain -> source raster -> one
    // real graphics pass -> swapchain, through the same executor phase 4 will run presets on.
    // No preset means the frame goes through untouched (decision 13), so the chain is not run at
    // all. The self-test is the exception: what it proves is that the passthrough reproduces the
    // frame exactly, which is only observable when the chain runs without a preset.
    static const bool selfTest = [] {
        const char* p = getenv("SHADERGLASS_SELFTEST");
        return p && p[0] == '1';
    }();
    const bool wantChain = enabled && !paused && (!preset.empty() || selfTest);

    uint32_t family = 0;
    if (auto qit = dc->queueFamilies.find(queue); qit != dc->queueFamilies.end())
        family = qit->second;

    // The caller's wait semaphores are handed to the first swapchain we actually process, and every
    // path after that presents with none.
    bool waitsConsumed = false;

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        auto sit = dc->swapchains.find(pPresentInfo->pSwapchains[i]);
        if (sit == dc->swapchains.end()) continue;
        SwapchainState& sc = sit->second;
        if (sc.passThrough || pPresentInfo->pImageIndices[i] >= sc.images.size()) continue;

        // One swapchain drives the effect; the rest present raw.
        if (!ClaimPrimary(dc->self, pPresentInfo->pSwapchains[i], sc.width, sc.height)) continue;

        sc.queue = queue;
        ++dc->framesSeen;

        bool composed = false;
        if (wantChain && !sc.resourcesFailed) {
            if (!sc.resourcesReady && !CreateSwapchainResources(dc, sc, family)) {
                Log("[layer] could not stage the chain for swapchain %p; passing it through",
                    (void*) pPresentInfo->pSwapchains[i]);
                DestroySwapchainResources(dc, sc);
                sc.resourcesFailed = true;
            }

            if (sc.resourcesReady) {
                uint32_t sourceW = 0, sourceH = 0;
                ShmSourceExtent(dc->shm.hdr, sc.width, sc.height, &sourceW, &sourceH);
                const std::string presetId = ShmPresetId(dc->shm.hdr);

                // A capture is asked for by bumping a counter, and answered by the next frame the
                // chain composes -- which is the only frame that has both halves of the pair.
                if (dc->shm.hdr) {
                    const uint32_t want =
                        dc->shm.hdr->captureRequest.load(std::memory_order_acquire);
                    if (want != dc->captureSeen) {
                        dc->captureSeen = want;
                        if (sc.chain) sc.chain->RequestCapture();
                    }
                }

                const uint32_t waitCount =
                    waitsConsumed ? 0u : pPresentInfo->waitSemaphoreCount;
                composed = ProcessPresent(dc, sc, queue, sc.images[pPresentInfo->pImageIndices[i]],
                                          waitCount, pPresentInfo->pWaitSemaphores, sourceW,
                                          sourceH, presetId);

                // Set whether or not the chain succeeded: the submit waits on them before anything
                // can fail, so they are consumed either way.
                if (waitCount) waitsConsumed = true;

                if (!composed && sc.chain && !sc.chain->Usable()) {
                    // The chain declared itself unusable on this device. Stop trying rather than
                    // failing once a frame forever.
                    ShmStoreString(dc->shm.hdr->layerReasonSeq, dc->shm.hdr->layerReason,
                                   kReasonBytes, sc.chain->Reason());
                    DestroySwapchainResources(dc, sc);
                    sc.resourcesFailed = true;
                }
            }
        } else if (!wantChain && sc.resourcesReady) {
            // Switched off. Release the chain's memory rather than holding a frame's worth of
            // surfaces for a game that is no longer being shaded.
            DestroySwapchainResources(dc, sc);
        }

        const uint32_t state = sc.resourcesFailed ? kLayerFailed
                               : composed         ? kLayerActive
                                                  : kLayerIdle;
        PublishStatus(dc, sc, state);

        if (Verbose()) {
            Log("[present] swapchain=%p image=%u %ux%u state=%u composed=%d preset=%s",
                (void*) pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i], sc.width,
                sc.height, state, (int) composed, preset.empty() ? "(none)" : preset.c_str());
        }
    }

    if (TimeEnabled()) {
        static int frameNo = 0;
        if (++frameNo % TimeInterval() == 0)
            Log("[time] frames seen=%llu", (unsigned long long) dc->framesSeen);
    }

    if (!waitsConsumed) return dc->vkQueuePresentKHR(queue, pPresentInfo);

    // pNext is carried through untouched: present ids, present timing and the rest belong to the
    // caller, and none of them are about semaphores.
    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 0;
    pi.pWaitSemaphores = nullptr;
    return dc->vkQueuePresentKHR(queue, &pi);
}

// Every queue operation takes the device lock, so the application's submissions cannot overlap the
// layer's own once phase 2 starts submitting.
VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                 const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueWaitIdle(queue);
}

// ---------------------------------------------------------------------------
// Loader entry points
// ---------------------------------------------------------------------------
PFN_vkVoidFunction LookupHook(const char* n) {
    if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction) Hook_CreateInstance;
    if (!strcmp(n, "vkDestroyInstance")) return (PFN_vkVoidFunction) Hook_DestroyInstance;
    if (!strcmp(n, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction) Hook_EnumeratePhysicalDevices;
    if (!strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction) Hook_CreateDevice;
    if (!strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction) Hook_DestroyDevice;
    if (!strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction) Hook_GetDeviceQueue;
    if (!strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction) Hook_GetDeviceQueue2;
    if (!strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction) Hook_CreateSwapchainKHR;
    if (!strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction) Hook_DestroySwapchainKHR;
    if (!strcmp(n, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction) Hook_AcquireNextImageKHR;
    if (!strcmp(n, "vkQueueSubmit")) return (PFN_vkVoidFunction) Hook_QueueSubmit;
    if (!strcmp(n, "vkQueueSubmit2")) return (PFN_vkVoidFunction) Hook_QueueSubmit2;
    if (!strcmp(n, "vkQueueWaitIdle")) return (PFN_vkVoidFunction) Hook_QueueWaitIdle;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction) Hook_QueuePresentKHR;
    return nullptr;
}

PFN_vkVoidFunction LookupDeviceHook(const char* n) {
    if (!strcmp(n, "vkCreateInstance")) return nullptr;
    return LookupHook(n);
}

}  // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
    }

    // Once per process, not once per negotiate. The loader re-enumerates the implicit-layer directory
    // many times during a single instance creation and loads this library on each pass; announcing it
    // each time turns one line into hundreds in the user's log.
    static std::once_flag announced;
    std::call_once(announced, [] {
        const char* e = getenv("SHADERGLASS");
        Log("=== %s loaded (SHADERGLASS=%s) ===", VK_LAYER_NAME, e ? e : "(unset)");
    });
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pCount,
                                                                  VkLayerProperties* pProperties) {
    if (!pCount) return VK_SUCCESS;
    if (!pProperties) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1) {
        *pCount = 1;
        return VK_INCOMPLETE;
    }
    memset(pProperties, 0, sizeof(*pProperties));
    strncpy(pProperties->layerName, VK_LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
    strncpy(pProperties->description, "ShaderGlass shader effect layer", VK_MAX_DESCRIPTION_SIZE - 1);
    pProperties->specVersion = VK_MAKE_VERSION(1, 3, 0);
    pProperties->implementationVersion = 1;
    *pCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t* pCount,
                                                                      VkExtensionProperties*) {
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char* pName) {
    if (!pName) return nullptr;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction) vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion"))
        return (PFN_vkVoidFunction) vkNegotiateLoaderLayerInterfaceVersion;
    if (!strcmp(pName, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceLayerProperties;
    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceExtensionProperties;
    if (auto fn = LookupHook(pName)) return fn;
    if (instance) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end() && it->second.next_gipa)
            return it->second.next_gipa(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction) vkGetDeviceProcAddr;
    if (auto fn = LookupDeviceHook(pName)) return fn;
    if (device) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end() && it->second->next_dpa)
            return it->second->next_dpa(device, pName);
    }
    return nullptr;
}

}  // extern "C"
