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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "../../common/shm_protocol.h"
#include "chain_order.h"
#include "gamescope_output.h"
#include "chain.h"
#include "crash_trace.h"
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

    // Source auto-detection. The candidate is what the last measurement said; it only becomes the
    // published raster once a second measurement agrees with it, because one frame is a bad witness
    // -- a fade to black, a full-screen menu or a title card all measure differently from the game.
    double lastProbeMs = 0.0;
    uint32_t probeCandidateW = 0, probeCandidateH = 0;
    // Frame skip counts presents, and composes one in every N. A pause freezes FrameCount as well as
    // the picture, or a shader that animates over time would keep moving on a still image.
    uint64_t skipCounter = 0;
    bool wasPaused = false;
    uint64_t heldFrameCount = 0;

    bool autoSourceWasOn = false;
    uint32_t autoSourceRefreshSeen = 0;
    bool autoSourceRefreshSeeded = false;

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
    //
    // Seeded from whatever the mapping already holds rather than from zero. The counter outlives
    // any one game -- it sits in a file -- so starting at zero made every game that launched after
    // a capture take one of its own, unasked, on its first composed frame.
    uint32_t captureSeen = 0;
    bool captureSeeded = false;

    // What was last published as the layer's reason, so an unchanged one is not rewritten every
    // frame -- the string is guarded by a sequence a reader retries on.
    std::string lastNotice;

    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 vkGetDeviceQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkAcquireNextImage2KHR vkAcquireNextImage2KHR = nullptr;
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

    // Where DLSS5VKLayer sits relative to this layer, if it is in the chain at all.
    DlssPosition dlss = DlssPosition::Absent;

    ShmMap shm;
    uint64_t framesSeen = 0;
    double lastFpsSampleMs = 0.0;
    uint64_t lastFpsFrames = 0;

    // Why the last present did not composite, as a string literal compared by identity. The present
    // hook has several early-outs that skip a frame without a word, so when the picture stops the
    // log simply goes quiet and says nothing about which one was taken. Latching the reason turns
    // that silence into one line per transition.
    std::atomic<const char*> lastSkip {nullptr};
    std::atomic<uint64_t> presentCalls {0};

    // The idle repaint (see RepaintLoop). A game that stops presenting -- paused, in a debugger, a
    // menu that stops drawing -- would otherwise show a settings change only once it presents again.
    //
    // It uses the game's queue and swapchain from a thread of its own, and Vulkan leaves both to the
    // application to synchronise. Locks cannot do it: the game's acquire and present hooks were once
    // locked and deadlocked against each other, and would again against a thread that acquires. So
    // a handshake instead: appCalls counts the game's calls in flight, repainting says a repaint is,
    // and each side raises its own before looking at the other's -- sequentially consistent, so at
    // least one of them sees the other. The repaint backs off; the game waits, briefly, for one that
    // is already under way, whose every step is bounded.
    std::atomic<int> appCalls {0};
    std::atomic<bool> repainting {false};
    std::atomic<double> lastPresentMs {0.0};
    // controlSeq as of the last frame composed, by the game's present or by a repaint. A different
    // value while the game is silent is a setting it has not yet seen.
    std::atomic<uint32_t> composedSeq {0};
    std::atomic<bool> composedAny {false};
    std::thread repaintThread;
    std::mutex repaintWakeLock;
    std::condition_variable repaintWake;
    bool repaintStop = false;
    VkFence repaintFence = VK_NULL_HANDLE;
    bool repaintBroken = false;  // a repaint's acquire never completed; its fence is still in use

    // Which queue families can run graphics work. The chain draws, so it can only be recorded for one
    // of these -- gamescope's compute-only queue is not.
    std::vector<bool> graphicsFamily;

    // Only used inside gamescope, on backends that never present: which of its images are output
    // images, which command buffer composites into which, and the state that shades them. See
    // gamescope_output.h.
    OutputTracker gamescope;
    // gamescope's composite targets, one chain each -- its output images, and the screenshot and
    // PipeWire textures, which are sized and formatted by whoever asked for them. Keyed by what a
    // chain is built for, so a stream and the display do not rebuild each other every frame.
    std::map<std::tuple<uint32_t, uint32_t, VkFormat, int>, SwapchainState> gamescopeTargets;

    // Command buffers of gamescope's that the chain was recorded into and that have not been
    // submitted yet, and whose chain they carry: their submission takes that chain's fence.
    std::unordered_map<VkCommandBuffer, SwapchainState*> gamescopeCarriers;
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
    double lastPresentMs = 0.0;
};
PrimarySwap g_primary;

// How long the swapchain holding the effect may go without presenting before an equally large one
// is allowed to take over.
//
// A game is not always one swapchain. Wine ports in particular play their movies on one graphics
// API and the game itself on another, which is two swapchains of identical size taking turns, and
// the claim used to be first-come-and-keep-forever: whichever presented first held the effect, and
// the other was refused every frame for the rest of the run. Handing over on silence follows the
// one that is actually on screen.
//
// Long enough that two swapchains presenting side by side cannot flip-flop -- a holder that keeps
// presenting is never silent, so it is never challenged -- and short enough that the gap at a
// movie-to-game cut is a few frames rather than something to notice.
constexpr double kPrimaryHandoverMs = 250.0;

// Its own mutex, never nested with dc->lock or g_stateMutex, so the lock order in the present hook
// cannot invert against the device hooks.
std::mutex g_primaryMutex;

bool ClaimPrimary(VkDevice device, VkSwapchainKHR swapchain, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    const double now = NowMs();
    const uint64_t area = uint64_t(w) * h;

    // The holder renewing its own claim, which is the common case and the only one that keeps the
    // handover clock from running.
    if (g_primary.swapchain == swapchain && g_primary.device == device) {
        g_primary.lastPresentMs = now;
        return true;
    }

    const bool vacant = g_primary.swapchain == VK_NULL_HANDLE;
    const bool silent = !vacant && (now - g_primary.lastPresentMs) > kPrimaryHandoverMs;

    // Bigger always wins, as before -- that is what keeps a one-pixel or overlay swapchain from
    // taking the effect off the game. Equal only wins once the holder has stopped presenting.
    if (!vacant && !silent && area <= g_primary.area) return false;

    g_primary.device = device;
    g_primary.swapchain = swapchain;
    g_primary.area = area;
    g_primary.lastPresentMs = now;
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
        // Checked here as well as in the manifest, because the manifest's disable_environment only
        // governs implicit enabling. Named in VK_INSTANCE_LAYERS -- which is how the layer is placed
        // below DLSS5VKLayer -- it is loaded regardless, and without this the documented way to keep
        // it out of one game would quietly stop doing anything.
        if (const char* off = getenv("SHADERGLASS_DISABLE"); off && off[0] == '1') return false;
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

// The device a command buffer belongs to. A dispatchable handle's first word is the loader's dispatch
// table, shared by a device and everything allocated from it, so it names the device without this
// layer having seen the command buffer allocated.
DeviceChain* DeviceForDispatch(const void* handle) {
    if (!handle) return nullptr;
    const void* key = *static_cast<const void* const*>(handle);
    std::lock_guard<std::mutex> lk(g_stateMutex);
    if (g_devices.size() == 1) return g_devices.begin()->second;
    for (auto& kv : g_devices)
        if (*reinterpret_cast<const void* const*>(kv.first) == key) return kv.second;
    return nullptr;
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

// Held across each of the game's calls that touch a queue or a swapchain, for the idle repaint's
// handshake (DeviceChain::appCalls). Waiting here happens only when a repaint is already under way,
// and lasts until it finishes -- a few milliseconds, bounded by its timeouts.
struct AppCall {
    explicit AppCall(DeviceChain* d) : dc(d) {
        dc->appCalls.fetch_add(1);
        while (dc->repainting.load()) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ~AppCall() { dc->appCalls.fetch_sub(1); }
    AppCall(const AppCall&) = delete;
    AppCall& operator=(const AppCall&) = delete;
    DeviceChain* dc;
};

void RememberQueue(DeviceChain* dc, VkQueue queue, uint32_t family) {
    if (!queue) return;
    std::lock_guard<std::mutex> lk(dc->lock);
    dc->queueFamilies[queue] = family;
}

// ---------------------------------------------------------------------------
// Chain resources
// ---------------------------------------------------------------------------
// How long the layer will wait for its own work before deciding the device is not going to finish
// it. Generous next to any frame a game presents, and finite -- which is the point. This code runs
// on the game's present thread, so an unbounded wait is not a slow layer, it is a hung game with no
// explanation and nothing on screen to say why.
constexpr uint64_t kFenceTimeoutNs = 2ull * 1000 * 1000 * 1000;

void DestroySwapchainResources(DeviceChain* dc, SwapchainState& sc) {
    // Nothing of ours may be in flight against the surfaces the chain is about to free.
    bool retired = true;
    if (sc.fencePending && dc->table.vkWaitForFences)
        retired = dc->table.vkWaitForFences(dc->self, 1, &sc.fence, VK_TRUE, kFenceTimeoutNs) ==
                  VK_SUCCESS;
    sc.fencePending = false;

    if (!retired) {
        // The work never finished, so the images and the command buffer it referenced may still be
        // read by the device. Leaking them is wrong; freeing them is worse -- it is a use-after-free
        // inside someone's game. The chain is dropped without its resources being reclaimed, and
        // the process is on its way out of using this layer anyway.
        Log("[layer] the device did not retire our work; leaving this chain's resources alone");
        (void) sc.chain.release();
        sc.fence = VK_NULL_HANDLE;
        sc.pool = VK_NULL_HANDLE;
        sc.cb = VK_NULL_HANDLE;
        sc.resourcesReady = false;
        return;
    }

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
// How a present is composed.
enum class Compose {
    kFresh,   // the game's new frame, through every pass
    kReplay,  // the last result placed again, no pass run -- the frames frame skip skips
    kHold,    // the passes run again on the frame the chain already holds -- a paused chain, whose
              // picture stays still while a parameter can still be adjusted against it
};

// Every output setting, read once, as the placement it adds up to, with the source raster resolved
// against the target area -- which is the crop when there is one, so a divisor divides the crop.
Placement ResolvePlacement(ShmHeader* h, const SwapchainState& sc, uint32_t* sourceW,
                           uint32_t* sourceH) {
    PlacementInput in;
    in.swapW = sc.width;
    in.swapH = sc.height;
    in.cropEnabled = h->cropEnabled.load() != 0;
    in.crop = {int32_t(h->cropX.load()), int32_t(h->cropY.load()), h->cropWidth.load(),
               h->cropHeight.load()};

    const IRect t = TargetArea(in.swapW, in.swapH, in.cropEnabled, in.crop);
    ShmSourceExtent(h, t.w, t.h, sourceW, sourceH);
    in.sourceW = *sourceW;
    in.sourceH = *sourceH;

    in.pixelSize = BitsToFloat(h->pixelSizeBits.load());
    in.policy = h->outputPolicy.load();
    in.pixelHeight = BitsToFloat(h->aspectRatioBits.load());
    in.flipH = h->flipHorizontal.load() != 0;
    in.flipV = h->flipVertical.load() != 0;
    in.rotation = h->rotation.load();
    return ComputePlacement(in);
}

// Wait out the previous frame, prepare the chain, and record it -- everything but the submit. Into
// sc.cb, normally; into `into` for gamescope's composite, which the chain is recorded straight into,
// right behind the dispatch that wrote the target. `layout` is the one the target arrives in and must
// be left in.
bool RecordFrame(DeviceChain* dc, SwapchainState& sc, VkImage image, uint32_t sourceW,
                 uint32_t sourceH, const Placement& placement, Compose mode, uint64_t frameCount,
                 const std::string& presetId, VkImageLayout layout,
                 VkCommandBuffer into = VK_NULL_HANDLE, bool repaint = false) {
    // The previous frame's chain, if it is still running, must finish before anything here touches
    // the surfaces it reads or the command buffer it was recorded into.
    if (sc.fencePending) {
        const VkResult waited =
            dc->table.vkWaitForFences(dc->self, 1, &sc.fence, VK_TRUE, kFenceTimeoutNs);
        if (waited == VK_TIMEOUT) {
            // Two seconds is not a slow frame, it is work that is never going to complete. Stop
            // rather than wait again next frame -- retrying would hold the game's present thread
            // for two seconds per frame, which is a frozen game either way.
            Log("[layer] the device did not finish a composed frame within %llu ms; "
                "giving up on this swapchain",
                (unsigned long long) (kFenceTimeoutNs / 1000000));
            if (dc->shm.hdr)
                ShmStoreString(dc->shm.hdr->layerReasonSeq, dc->shm.hdr->layerReason, kReasonBytes,
                               "the GPU did not finish a composed frame; chain stopped");
            sc.resourcesFailed = true;
            return false;
        }
        if (!NoteVk(dc, waited, "vkWaitForFences")) return false;
        dc->table.vkResetFences(dc->self, 1, &sc.fence);
        sc.fencePending = false;

        // That fence retiring is what lets the chain release what the previous frame was still
        // using, and read back what it recorded.
        sc.chain->FrameCompleted();
    }

    if (!sc.chain->Prepare(sc.width, sc.height, sc.format, sourceW, sourceH, placement, presetId))
        return false;

    // A repaint has no new frame, only the one the chain holds. Without it, a hold would read the
    // swapchain image instead -- which holds a picture already shaded, and shading it again stacks
    // the effect on itself.
    if (repaint && !sc.chain->HoldsInput()) return false;

    // Parameter values are read every frame rather than watched, because they are cheap to read and
    // the alternative is a second sequence number to get wrong. Prepare has already folded in the
    // preset's own defaults, so this only moves what the interface has changed.
    if (dc->shm.hdr) {
        const uint32_t count =
            std::min(dc->shm.hdr->paramCount.load(std::memory_order_relaxed), kMaxParams);
        sc.chain->ApplyParameters(dc->shm.hdr->params, count);
    }

    const VkCommandBuffer cb = into ? into : sc.cb;
    if (into) {
        // gamescope records its barriers lazily, ahead of the next dispatch that needs one, so the
        // composite's writes have none behind them yet. This one is the chain's.
        VkImageMemoryBarrier b {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = layout;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dc->table.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                       1, &b);
    } else {
        VkCommandBufferBeginInfo bi {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!NoteVk(dc, dc->table.vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer"))
            return false;
    }

    // A replay needs something composed to show; until then it is an ordinary frame. Hold falls back
    // the same way inside Record, so a pause that begins before the first frame still draws one.
    bool recorded = false;
    if (mode == Compose::kReplay && sc.chain->HasComposed())
        recorded = sc.chain->RecordReplay(cb, image, layout);
    else
        recorded = sc.chain->Record(cb, image, frameCount, layout, mode == Compose::kHold);

    if (into) return recorded;
    if (!recorded) {
        dc->table.vkEndCommandBuffer(cb);
        return false;
    }
    return NoteVk(dc, dc->table.vkEndCommandBuffer(cb), "vkEndCommandBuffer");
}

// Submit what RecordFrame recorded, waiting on the caller's semaphores and signalling sc.fence.
bool SubmitFrame(DeviceChain* dc, SwapchainState& sc, VkQueue queue, uint32_t waitCount,
                 const VkSemaphore* waits) {
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

    // gamescope composites on a compute-only queue when the GPU has one, and the chain draws with
    // graphics pipelines, which cannot run there: appended to such a batch they hang it, and gamescope
    // waits for that frame forever. GAMESCOPE_FORCE_GENERAL_QUEUE is gamescope's own switch to
    // composite on the graphics+compute queue instead, read when it picks a queue -- which is after
    // this, since picking a queue needs an instance. Only when the layer is switched on here, and
    // never over a value somebody set.
    if (InGamescope() && LayerEnabled() && !getenv("GAMESCOPE_FORCE_GENERAL_QUEUE")) {
        setenv("GAMESCOPE_FORCE_GENERAL_QUEUE", "1", 0);
        Log("[gamescope] asked gamescope to composite on its general queue, where the chain can run");
    }

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
    if (Verbose()) Log("[chain] instance %p registered (%zu total)", (void*) *pInstance,
                       g_instances.size());
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
    // VK_INCOMPLETE too: the caller asked for fewer devices than exist and got a short answer, and
    // the ones it did get are still this instance's. Registering only on VK_SUCCESS left those
    // unattributed, and an unattributed physical device used to take the layer inert.
    if ((res == VK_SUCCESS || res == VK_INCOMPLETE) && pPhysicalDevices && pCount) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        for (uint32_t i = 0; i < *pCount; ++i) g_phys[pPhysicalDevices[i]] = chain;
        if (Verbose())
            Log("[chain] enumerated %u physical device(s) (res=%d)", *pCount, (int) res);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
void StartRepaint(DeviceChain* dc);
void StopRepaint(DeviceChain* dc);

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
    bool attributed = true;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_phys.find(physicalDevice);
        if (it != g_phys.end()) {
            ic = it->second;
        } else if (g_instances.size() == 1) {
            // A physical device this layer never saw enumerated. That happens when something above
            // it in the chain obtained the handle by a route this layer does not hook -- device
            // groups, or a cached handle from before -- and it is routine when another layer sits
            // above: DLSS5VKLayer produces exactly this.
            //
            // With one instance the attribution is not in doubt, so use it. The alternative, which
            // is what this did before, was to go inert and shade nothing at all, which is a bad
            // trade for a bookkeeping miss.
            ic = &g_instances.begin()->second;
            attributed = false;
        }
    }

    // On the way down, and the only unambiguous marker of chain position there is. Every other line
    // this layer writes during device creation is written on the way back up, so their order is the
    // reverse of the chain's -- which is exactly how the order here was misread once already. A
    // layer above this one has already printed its own entry line by the time this runs.
    if (Verbose()) Log("[chain] entering vkCreateDevice");

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
    SG_LOAD(vkAcquireNextImage2KHR)
    SG_LOAD(vkQueuePresentKHR)
    SG_LOAD(vkQueueSubmit)
    SG_LOAD(vkQueueSubmit2)
    SG_LOAD(vkQueueWaitIdle)
    SG_LOAD(vkDeviceWaitIdle)
#undef SG_LOAD

    dc->table.next_dpa = next_dpa;
    dc->table.Load(*pDevice);

    if (ic && ic->table.vkGetPhysicalDeviceQueueFamilyProperties) {
        uint32_t n = 0;
        ic->table.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, nullptr);
        std::vector<VkQueueFamilyProperties> props(n);
        ic->table.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, props.data());
        dc->graphicsFamily.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            dc->graphicsFamily[i] = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    }

    // No swapchain means nothing to shade -- except in gamescope, whose Wayland, DRM and headless
    // backends never create one, and whose composite is shaded instead.
    const bool noPresent = !dc->vkQueuePresentKHR || !dc->vkCreateSwapchainKHR;
    if (!ic || (noPresent && !InGamescope())) dc->inert = true;

    // No vendor check. DLSS5VKLayer went inert on non-NVIDIA devices because NGX is NVIDIA-only;
    // shader chains are not, so this runs on AMD, Intel and NVIDIA alike.
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "?";
    if (ic && ic->vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props {};
        ic->vkGetPhysicalDeviceProperties(physicalDevice, &props);
        snprintf(deviceName, sizeof(deviceName), "%s", props.deviceName);
    }

    // Where we landed relative to DLSS5VKLayer, answered from the pointer the chain below us just
    // gave for presenting. Worked out here because this is the first moment that pointer exists,
    // and reported once per device rather than per frame.
    std::string nextOwner;
    if (LayerEnabled()) dc->dlss = FindDlssPosition((void*) dc->vkQueuePresentKHR, &nextOwner);

    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_devices[*pDevice] = dc;
    }
    StartRepaint(dc);
    // Nothing from here on is anyone's business unless the layer is switched on in this process.
    if (!LayerEnabled()) return VK_SUCCESS;
    Log("[layer] device %p on %s (inert=%d enabled=%d)", (void*) *pDevice, deviceName,
        (int) dc->inert.load(), (int) LayerEnabled());
    if (!attributed)
        Log("[layer] physical device %p was not enumerated through this layer; attributed to the "
            "only instance",
            (void*) physicalDevice);
    if (Verbose()) Log("[layer] next present belongs to %s", nextOwner.c_str());
    if (dc->dlss != DlssPosition::Absent)
        Log("[layer] %s", DescribeDlssPosition(dc->dlss));
    if (DlssOrderIsWrong(dc->dlss))
        Log("[layer] the shader will be reconstructed by DLSS rather than finishing the picture; "
            "see docs/DLSS5VKLayer.md");
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

    StopRepaint(dc);
    if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
    if (dc->repaintFence) dc->table.vkDestroyFence(device, dc->repaintFence, nullptr);
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) {
            ReleasePrimary(device, kv.first);
            DestroySwapchainResources(dc, kv.second);
        }
        dc->swapchains.clear();
        for (auto& kv : dc->gamescopeTargets) DestroySwapchainResources(dc, kv.second);
        dc->gamescopeTargets.clear();
        dc->gamescopeCarriers.clear();
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

    VkResult res;
    {
        // Creating one retires the old swapchain, which the idle repaint may be presenting to.
        AppCall call(dc);
        res = dc->vkCreateSwapchainKHR(device, &m, pAllocator, pSwapchain);
        if (res != VK_SUCCESS && wantUsage) {
            // Nothing we add is worth failing a swapchain creation over.
            Log("[layer] swapchain refused the added transfer usage (%d); retrying with the "
                "game's own",
                (int) res);
            res = dc->vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        }
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
    // The whole of it: it waits for the device, which is a use of every queue, and destroys a
    // swapchain the idle repaint may be presenting to.
    AppCall call(dc);

    ReleasePrimary(device, swapchain);
    {
        std::unique_lock<std::mutex> lk(dc->lock);
        if (dc->swapchains.count(swapchain)) {
            // Outside the lock: a device-wide wait must not be taken while holding a lock the
            // present path also wants.
            lk.unlock();
            if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
            lk.lock();

            // Found again, not carried across the gap. swapchains is an unordered_map, so any
            // insert rehashes it and invalidates every iterator -- and the insert that does it is
            // the *new* swapchain being created, which is precisely what happens either side of a
            // resolution or mode change. Holding the iterator over the unlock made recreating a
            // swapchain a use-after-free, which is what took games down when they changed mode.
            auto it = dc->swapchains.find(swapchain);
            if (it != dc->swapchains.end()) {
                Log("[layer] swapchain %p destroyed", (void*) swapchain);
                DestroySwapchainResources(dc, it->second);
                dc->swapchains.erase(it);
            }
        }
    }
    if (dc->vkDestroySwapchainKHR) dc->vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t timeout, VkSemaphore semaphore,
                                                        VkFence fence, uint32_t* pImageIndex) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkAcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;

    // No lock here, and never one again. This call blocks until the presentation engine hands back
    // an image, and the only call that hands one back is vkQueuePresentKHR -- which wanted the same
    // device-wide lock. Acquiring it here deadlocked the two against each other: the game thread
    // sat in acquire holding the lock, the presenter thread sat on the lock unable to present, and
    // neither ever moved. It survived ordinary play because acquire returned immediately while the
    // pool had spare images; it closed the moment anything put one more frame in flight, such as a
    // game opening its menu.
    //
    // Nothing here touches layer state, so there is nothing for a lock to protect. The same is true
    // of the queue hooks below. What is counted is only that the call is in flight, for the idle
    // repaint, which acquires from the same swapchain.
    AppCall call(dc);
    return dc->vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImage2KHR(VkDevice device,
                                                         const VkAcquireNextImageInfoKHR* info,
                                                         uint32_t* pImageIndex) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkAcquireNextImage2KHR) return VK_ERROR_INITIALIZATION_FAILED;
    AppCall call(dc);
    return dc->vkAcquireNextImage2KHR(device, info, pImageIndex);
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------
// Two measured rasters this close are the same raster. Proportional rather than absolute: a frame of
// real game art does not measure to the same hundredth twice running, and every accepted change
// rebuilds the whole chain -- at a 19-pass preset that is a visible hitch, so drift inside the
// measurement's own noise must not buy one.
bool Near(uint32_t a, uint32_t b) {
    const uint32_t hi = a > b ? a : b;
    const uint32_t diff = a > b ? a - b : b - a;
    const uint32_t slack = hi / 32;  // about 3%
    return diff <= (slack < 2 ? 2 : slack);
}


// Drop the measured raster, so ShmSourceExtent falls back to the mode the user chose.
void ForgetMeasuredSource(ShmHeader* hdr) {
    hdr->autoSourceWidth.store(0);
    hdr->autoSourceHeight.store(0);
    hdr->autoSourceScaleXBits.store(0);
    hdr->autoSourceScaleYBits.store(0);
    hdr->autoSourceConfidenceBits.store(0);
    hdr->autoSourceSeq.fetch_add(1);
}

// Says why a frame went through untouched, once per change of answer rather than once per frame.
// Every early-out below used to be silent, so "the shader stopped applying" and "the hook stopped
// being called" produced identical logs -- nothing at all. Reasons are string literals and compared
// by identity, which is what makes the once-per-transition test cheap enough to sit in the hot path.
void NoteSkip(DeviceChain* dc, const char* why) {
    if (dc->lastSkip.exchange(why) == why) return;
    Log("[present] not compositing: %s (after %llu calls)", why,
        (unsigned long long) dc->presentCalls.load());
}

void NoteComposited(DeviceChain* dc) {
    const char* was = dc->lastSkip.exchange(nullptr);
    if (was) Log("[present] compositing again (was: %s)", was);
}

// One frame of shading for one target, whichever way the target reached the layer: a swapchain image
// being presented, or gamescope's composite on a backend that never presents it. Everything that
// belongs to "a frame" is here -- the capture request, the source measurement, pause and frame skip,
// the notices, the status -- so the two routes cannot drift apart into two slightly different
// layers.
//
// `submit` is the one difference that matters. A presented image is shaded by a submission of the
// layer's own, which waits on the caller's semaphores. gamescope's composite is not: the chain is
// recorded into gamescope's command buffer (`into`), which gamescope submits itself.
struct FrameResult {
    bool composed = false;
    uint32_t state = kLayerIdle;
};

FrameResult ShadeFrame(DeviceChain* dc, SwapchainState& sc, VkQueue queue, VkImage image,
                       uint32_t family, bool wantChain, bool paused, bool submit,
                       uint32_t waitCount, const VkSemaphore* waits, VkImageView storageView,
                       VkImageLayout layout, const void* tag, bool* waitsUsed,
                       VkCommandBuffer into = VK_NULL_HANDLE) {
    sc.queue = queue;
    ++dc->framesSeen;

    bool composed = false;
    if (wantChain && !sc.resourcesFailed) {
        if (!sc.resourcesReady && !CreateSwapchainResources(dc, sc, family)) {
            Log("[layer] could not stage the chain for %p; passing it through", tag);
            DestroySwapchainResources(dc, sc);
            sc.resourcesFailed = true;
        }

        if (sc.resourcesReady) {
            uint32_t sourceW = 0, sourceH = 0;
            const Placement placement = ResolvePlacement(dc->shm.hdr, sc, &sourceW, &sourceH);
            const std::string presetId = ShmPresetId(dc->shm.hdr);

            // Which of the three ways to compose this present.
            Compose mode = Compose::kFresh;
            uint64_t frameCount = dc->framesSeen;
            if (paused) {
                if (!sc.wasPaused) sc.heldFrameCount = dc->framesSeen;
                mode = Compose::kHold;
                frameCount = sc.heldFrameCount;
            } else {
                const uint32_t skip = dc->shm.hdr->frameSkip.load();
                // 0 and 1 both mean every frame.
                if (skip > 1 && (sc.skipCounter++ % skip) != 0) mode = Compose::kReplay;
            }
            sc.wasPaused = paused;

            // A capture is asked for by bumping a counter, and answered by the next frame the
            // chain composes -- which is the only frame that has both halves of the pair.
            if (dc->shm.hdr) {
                const uint32_t want =
                    dc->shm.hdr->captureRequest.load(std::memory_order_acquire);
                if (!dc->captureSeeded) {
                    // Whatever is in there was asked for before this game existed.
                    dc->captureSeeded = true;
                    dc->captureSeen = want;
                } else if (want != dc->captureSeen) {
                    dc->captureSeen = want;
                    if (sc.chain) sc.chain->RequestCapture();
                }
            }

            // A measurement is asked for on a timer and collected whenever it is ready, which
            // is the frame after. Neither costs anything on the frames in between.
            if (dc->shm.hdr) {
                ShmHeader* hdr = dc->shm.hdr;
                const bool on = hdr->autoSourceEnabled.load() != 0;

                // Two ways to be told to start again: the interface bumping the refresh
                // counter, and the toggle changing under a layer that was running to see it.
                // The counter is the reliable one -- a toggle flipped while no game was
                // attached leaves no edge for anyone to notice -- and the edge is kept because
                // shaderglass-ctl writes the toggle without bumping anything.
                const uint32_t refresh = hdr->autoSourceRefresh.load();
                if (!sc.autoSourceRefreshSeeded) {
                    sc.autoSourceRefreshSeeded = true;
                    sc.autoSourceRefreshSeen = refresh;
                }
                const bool asked = refresh != sc.autoSourceRefreshSeen;
                if (asked || on != sc.autoSourceWasOn) {
                    sc.autoSourceRefreshSeen = refresh;
                    sc.autoSourceWasOn = on;
                    sc.probeCandidateW = sc.probeCandidateH = 0;
                    sc.lastProbeMs = 0.0;  // measure on the next frame, not in two seconds
                    ForgetMeasuredSource(hdr);
                }

                if (on && sc.chain) {
                    uint32_t every = hdr->autoSourceIntervalMs.load();
                    if (!every) every = kAutoSourceDefaultMs;
                    const double now = NowMs();
                    if (now - sc.lastProbeMs >= double(every)) {
                        sc.lastProbeMs = now;
                        sc.chain->RequestGridProbe();
                    }

                    GridEstimate g {};
                    if (sc.chain->TakeGridEstimate(&g)) {
                        uint32_t aw = 0, ah = 0;
                        if (GridToSourceExtent(g, sc.width, sc.height, &aw, &ah)) {
                            const uint32_t curW = hdr->autoSourceWidth.load();
                            const uint32_t curH = hdr->autoSourceHeight.load();
                            // Applied on the strength of one measurement. An earlier version
                            // waited for two in a row to agree, which was standing in for the
                            // filtering the span guard and the raster floor now do properly --
                            // and all it bought was a scene change taking two intervals to be
                            // noticed, which is the opposite of what this is for.
                            const bool changed = !Near(aw, curW) || !Near(ah, curH);

                            if (changed) {
                                hdr->autoSourceScaleXBits.store(FloatToBits(g.scaleX));
                                hdr->autoSourceScaleYBits.store(FloatToBits(g.scaleY));
                                hdr->autoSourceConfidenceBits.store(FloatToBits(g.confidence));
                                hdr->autoSourceWidth.store(aw);
                                hdr->autoSourceHeight.store(ah);
                                hdr->autoSourceSeq.fetch_add(1);
                                Log("[source] measured %ux%u (x%.2f, y%.2f, confidence %.2f)",
                                    aw, ah, double(g.scaleX), double(g.scaleY),
                                    double(g.confidence));
                            }
                            sc.probeCandidateW = aw;
                            sc.probeCandidateH = ah;
                        } else {
                            // Nothing measurable on this frame, and that is not news about the
                            // game. A dark room, a movie, a fade, a menu over black -- none of
                            // them mean the raster changed, because the raster is a property of
                            // the game and not of the scene. The last answer stands.
                            //
                            // An earlier version dropped it after a run of such frames, on the
                            // theory that a stale answer was worse than none. In a game it is
                            // the other way round: walking into an unlit cave made the source
                            // raster change underfoot, which is both visible and wrong. The way
                            // out of a bad answer is the refresh request -- the toggle, or the
                            // button -- which did not exist when that rule was written.
                            sc.probeCandidateW = sc.probeCandidateH = 0;
                        }
                    }
                }
            }

            // The storage route when the target cannot be copied into; the copy route otherwise.
            sc.chain->SetStorageTarget(storageView);
            composed = RecordFrame(dc, sc, image, sourceW, sourceH, placement, mode, frameCount,
                                   presetId, layout, into) &&
                       (!submit || SubmitFrame(dc, sc, queue, waitCount, waits));

            // Set whether or not the chain succeeded: the submit waits on them before anything
            // can fail, so they are consumed either way.
            if (waitCount && waitsUsed) *waitsUsed = true;

            // Published every frame, so it appears as soon as there is something to say and
            // disappears once there is not.
            if (sc.chain && sc.chain->Usable()) {
                const char* notice = sc.chain->Notice();
                if (dc->lastNotice != notice) {
                    dc->lastNotice = notice;
                    ShmStoreString(dc->shm.hdr->layerReasonSeq, dc->shm.hdr->layerReason,
                                   kReasonBytes, notice);
                }
            }

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

    FrameResult r;
    r.composed = composed;
    r.state = sc.resourcesFailed ? kLayerFailed : composed ? kLayerActive : kLayerIdle;
    PublishStatus(dc, sc, r.state);
    return r;
}

// ---------------------------------------------------------------------------
// Idle repaint (decision 14)
// ---------------------------------------------------------------------------
// A game that stops presenting -- paused, sitting in a menu that no longer draws, stopped in a
// debugger -- gives a settings change no frame to land on. This composes the frame the chain holds
// again, into an image of the game's own swapchain, and presents it.
//
// It never reads the screen: the image it acquires holds a picture already shaded, and composing
// from that stacks each edit on the last. It composes from what the chain kept (Chain::HoldsInput),
// which survives a preset change, and does nothing when there is none.

// Long enough that a game between two frames is not taken for one that has stopped, short enough to
// answer a slider while the hand is still on it.
constexpr double kRepaintSilenceMs = 250.0;
constexpr auto kRepaintTick = std::chrono::milliseconds(50);
// With the game stopped, no present is coming to hand an image back; do not wait for one.
constexpr uint64_t kRepaintAcquireNs = 150ull * 1000 * 1000;

bool RepaintEnabled() {
    static const bool on = [] {
        const char* v = getenv("SHADERGLASS_REPAINT");
        return !(v && v[0] == '0');
    }();
    return on;
}

// One repaint, if one is due. Returns whether it presented.
bool RepaintOnce(DeviceChain* dc) {
    if (dc->inert || dc->repaintBroken || !dc->composedAny.load()) return false;
    if (NowMs() - dc->lastPresentMs.load() < kRepaintSilenceMs) return false;

    // The handshake: raise ours, then look at theirs.
    dc->repainting.store(true);
    struct Lower {
        DeviceChain* dc;
        ~Lower() { dc->repainting.store(false); }
    } lower {dc};
    if (dc->appCalls.load() != 0) return false;

    // Tried, never waited for: whoever holds it may be about to wait on this thread's flag.
    std::unique_lock<std::mutex> lk(dc->lock, std::try_to_lock);
    if (!lk.owns_lock()) return false;

    // The mapping is the present hook's to open, under this lock.
    ShmHeader* hdr = dc->shm.hdr;
    if (!hdr) return false;
    const uint32_t seq = hdr->controlSeq.load();
    if (seq == dc->composedSeq.load()) return false;

    // Settled before an image is taken: an acquired image goes back only by being presented.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> pl(g_primaryMutex);
        if (g_primary.device == dc->self) swapchain = g_primary.swapchain;
    }
    auto it = dc->swapchains.find(swapchain);
    if (it == dc->swapchains.end()) return false;
    SwapchainState& sc = it->second;
    if (sc.passThrough || sc.resourcesFailed || !sc.resourcesReady || !sc.chain || !sc.queue ||
        !sc.chain->HoldsInput())
        return false;
    if (!ShmEnabled(hdr) || ShmPresetId(hdr).empty()) {
        // Switched off, or the preset cleared, while the game is stopped. Nothing to compose; the
        // game's next frame goes through untouched.
        dc->composedSeq.store(seq);
        return false;
    }

    if (!dc->repaintFence) {
        VkFenceCreateInfo fi {};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (dc->table.vkCreateFence(dc->self, &fi, nullptr, &dc->repaintFence) != VK_SUCCESS)
            return false;
    }

    uint32_t index = 0;
    const VkResult acquired = dc->vkAcquireNextImageKHR(dc->self, swapchain, kRepaintAcquireNs,
                                                        VK_NULL_HANDLE, dc->repaintFence, &index);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return false;  // try next tick
    const VkResult ready =
        dc->table.vkWaitForFences(dc->self, 1, &dc->repaintFence, VK_TRUE, kRepaintAcquireNs);
    if (ready == VK_SUCCESS) {
        dc->table.vkResetFences(dc->self, 1, &dc->repaintFence);
    } else {
        // The image is ours but the fence is still in use. Leak the fence rather than destroy what
        // is pending, stop repainting, and hand the image back untouched below.
        Log("[repaint] an acquired image never became ready; idle repaint stopped");
        dc->repaintFence = VK_NULL_HANDLE;
        dc->repaintBroken = true;
    }

    bool composed = false;
    if (ready == VK_SUCCESS && index < sc.images.size()) {
        uint32_t sourceW = 0, sourceH = 0;
        const Placement placement = ResolvePlacement(hdr, sc, &sourceW, &sourceH);
        const uint64_t frameCount = sc.wasPaused ? sc.heldFrameCount : dc->framesSeen;
        sc.chain->SetStorageTarget(VK_NULL_HANDLE);
        composed = RecordFrame(dc, sc, sc.images[index], sourceW, sourceH, placement,
                               Compose::kHold, frameCount, ShmPresetId(hdr),
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_NULL_HANDLE,
                               /*repaint=*/true) &&
                   SubmitFrame(dc, sc, sc.queue, 0, nullptr);
    }

    // Presented either way -- untouched, it is the picture that was on screen a frame or two ago.
    VkPresentInfoKHR pi {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    const VkResult presented = dc->vkQueuePresentKHR(sc.queue, &pi);

    // Composed or not, this setting has had its answer; a failure would only fail again.
    dc->composedSeq.store(seq);
    if (Verbose())
        Log("[repaint] image %u %s, present %d", index, composed ? "recomposed" : "untouched",
            (int) presented);
    return composed;
}

void RepaintLoop(DeviceChain* dc) {
    std::unique_lock<std::mutex> lk(dc->repaintWakeLock);
    while (!dc->repaintStop) {
        dc->repaintWake.wait_for(lk, kRepaintTick, [dc] { return dc->repaintStop; });
        if (dc->repaintStop) break;
        lk.unlock();
        RepaintOnce(dc);
        lk.lock();
    }
}

void StartRepaint(DeviceChain* dc) {
    if (dc->inert || !LayerEnabled() || !RepaintEnabled()) return;
    dc->repaintThread = std::thread(RepaintLoop, dc);
}

// Before anything the thread touches is destroyed, and outside every lock it takes.
void StopRepaint(DeviceChain* dc) {
    if (!dc->repaintThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(dc->repaintWakeLock);
        dc->repaintStop = true;
    }
    dc->repaintWake.notify_all();
    dc->repaintThread.join();
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue,
                                                    const VkPresentInfoKHR* pPresentInfo) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueuePresentKHR) {
        // The last silent path in the hook, and the worst one: it fails the present outright
        // without calling down, so the application loses the frame and the log says nothing. Once
        // is enough to know it happened.
        static std::once_flag once;
        std::call_once(once, [queue] {
            Log("[present] no device chain for queue %p -- presenting is being refused",
                (void*) queue);
        });
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // For the idle repaint: this call is in flight, and the game is presenting.
    AppCall inFlight(dc);
    dc->lastPresentMs.store(NowMs());

    // Counted before any early-out, so the log can tell a hook that stopped being called from one
    // that is still called and declining to do anything.
    const uint64_t call = dc->presentCalls.fetch_add(1) + 1;
    if (Verbose() && call % 600 == 0)
        Log("[present] %llu calls seen", (unsigned long long) call);

    if (dc->inert || !LayerEnabled()) {
        // Only worth a line when the layer was switched on. Switched off is the normal state of
        // every other Vulkan program in the session -- the layer is loaded into all of them so that
        // it can sit below DLSS5VKLayer -- and reporting that would put a line in each one's stderr.
        if (LayerEnabled()) NoteSkip(dc, "device went inert");
        return dc->vkQueuePresentKHR(queue, pPresentInfo);
    }

    // Held across the layer's own work -- which is what it is for, since that work walks
    // dc->swapchains and the chain objects hanging off it -- and released before the call goes down
    // the chain. Presenting is not layer state, and in FIFO it blocks until the display is ready:
    // holding a device-wide lock across it would stall every other thread on the device for a frame
    // at a time, which is the same mistake the acquire hook used to make.
    std::unique_lock<std::mutex> lk(dc->lock);

    if (!ShmOpen(dc->shm)) {
        NoteSkip(dc, "no shared-memory mapping");
        lk.unlock();
        return dc->vkQueuePresentKHR(queue, pPresentInfo);
    }

    static std::once_flag namePublished;
    std::call_once(namePublished, [dc] { PublishGameName(dc); });

    // Read before any setting is, so a change made while this frame composes is still news after it.
    const uint32_t seq = dc->shm.hdr->controlSeq.load();
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
    // Paused is not off. It used to be: a pause switched the chain off and let the game's raw frames
    // through, when every description of it -- shaderglass-ctl's help, the interface's tooltip -- says
    // it freezes the chain on the last composed frame. It now does, and see Compose::kHold.
    const bool wantChain = enabled && (!preset.empty() || selfTest);

    uint32_t family = 0;
    if (auto qit = dc->queueFamilies.find(queue); qit != dc->queueFamilies.end())
        family = qit->second;

    // The caller's wait semaphores are handed to the first swapchain we actually process, and every
    // path after that presents with none.
    bool waitsConsumed = false;

    // Why nothing was composited, reported only if nothing was. A present may carry swapchains the
    // layer does not drive alongside the one it does, so a per-swapchain complaint would cry wolf
    // every frame; the answer is only interesting once the whole present has come up empty.
    const char* skip = nullptr;
    bool handledAny = false;

    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        auto sit = dc->swapchains.find(pPresentInfo->pSwapchains[i]);
        if (sit == dc->swapchains.end()) {
            if (!skip) skip = "swapchain not known to the layer";
            continue;
        }
        SwapchainState& sc = sit->second;
        if (sc.passThrough || pPresentInfo->pImageIndices[i] >= sc.images.size()) {
            if (!skip)
                skip = sc.passThrough ? "swapchain marked pass-through"
                                      : "image index outside the swapchain";
            continue;
        }

        // One swapchain drives the effect; the rest present raw.
        if (!ClaimPrimary(dc->self, pPresentInfo->pSwapchains[i], sc.width, sc.height)) {
            if (!skip) skip = "another swapchain holds the effect";
            continue;
        }
        handledAny = true;

        const uint32_t waitCount = waitsConsumed ? 0u : pPresentInfo->waitSemaphoreCount;
        const FrameResult fr = ShadeFrame(
            dc, sc, queue, sc.images[pPresentInfo->pImageIndices[i]], family, wantChain, paused,
            /*submit=*/true, waitCount, pPresentInfo->pWaitSemaphores, VK_NULL_HANDLE,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, (const void*) pPresentInfo->pSwapchains[i],
            &waitsConsumed);
        const bool composed = fr.composed;
        const uint32_t state = fr.state;

        if (Verbose()) {
            Log("[present] swapchain=%p image=%u %ux%u state=%u composed=%d preset=%s",
                (void*) pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i], sc.width,
                sc.height, state, (int) composed, preset.empty() ? "(none)" : preset.c_str());
        }
    }

    if (handledAny) {
        NoteComposited(dc);
        dc->composedSeq.store(seq);
        dc->composedAny.store(true);
    } else if (skip) {
        NoteSkip(dc, skip);
    }

    if (TimeEnabled()) {
        static int frameNo = 0;
        if (++frameNo % TimeInterval() == 0)
            Log("[time] frames seen=%llu", (unsigned long long) dc->framesSeen);
    }

    if (!waitsConsumed) {
        lk.unlock();
        return dc->vkQueuePresentKHR(queue, pPresentInfo);
    }

    // pNext is carried through untouched: present ids, present timing and the rest belong to the
    // caller, and none of them are about semaphores.
    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 0;
    pi.pWaitSemaphores = nullptr;
    lk.unlock();
    return dc->vkQueuePresentKHR(queue, &pi);
}

// Pass-throughs, and deliberately lock-free. These once took the device lock, on the theory that
// the application's submissions must not overlap the layer's own. They must not -- but Vulkan
// already requires the application to externally synchronize a queue, and the layer only ever
// submits from inside vkQueuePresentKHR, during which the application guarantees it is not
// submitting on that queue itself. The lock bought nothing and cost a deadlock: all three of these
// can block in the driver, and a device-wide lock held across a call that blocks on the GPU stops
// every other thread on the device.
// ---------------------------------------------------------------------------
// gamescope's composite, on the backends that never present it
// ---------------------------------------------------------------------------
// These hooks are only handed out inside gamescope (see LookupHook). Everywhere else several of them
// are hot paths -- vkCmdBindDescriptorSets runs thousands of times a frame in a game -- and a layer
// that is loaded into every Vulkan program in the session has no business sitting in them.
VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImage(VkDevice device, const VkImageCreateInfo* info,
                                                const VkAllocationCallbacks* alloc, VkImage* image) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkCreateImage) return VK_ERROR_INITIALIZATION_FAILED;

    // A screenshot or PipeWire texture is written by the composite but never read back on the GPU,
    // so it is made without TransferSrc -- and the chain reads its target with a blit. Asked for here,
    // and only for a format the chain can shade; if the driver will not have it, the texture is made
    // as gamescope asked and simply not shaded.
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    bool widened = false;
    if (info && LayerEnabled() && IsGamescopeCaptureUsage(*info) && SupportedFormat(info->format)) {
        VkImageCreateInfo wide = *info;
        wide.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        r = dc->table.vkCreateImage(device, &wide, alloc, image);
        widened = (r == VK_SUCCESS);
    }
    if (!widened) r = dc->table.vkCreateImage(device, info, alloc, image);
    if (r == VK_SUCCESS && info) {
        if (widened || !IsGamescopeCaptureUsage(*info)) dc->gamescope.OnCreateImage(*image, *info);
        // Every storage image, kept or not, so that when gamescope changes how it makes its output
        // images the log says what it makes now instead of simply finding nothing.
        if (LayerEnabled() && Verbose() && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT))
            Log("[gamescope] image %p %ux%u fmt=%d usage=%#x %s", (void*) *image,
                info->extent.width, info->extent.height, (int) info->format, info->usage,
                IsGamescopeOutputUsage(*info) ? "output image"
                : widened                     ? "capture texture"
                                              : "not a target");
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyImage(VkDevice device, VkImage image,
                                             const VkAllocationCallbacks* alloc) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkDestroyImage) return;
    dc->gamescope.OnDestroyImage(image);
    dc->table.vkDestroyImage(device, image, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BindImageMemory(VkDevice device, VkImage image,
                                                    VkDeviceMemory memory, VkDeviceSize offset) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkBindImageMemory) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = dc->table.vkBindImageMemory(device, image, memory, offset);
    if (r == VK_SUCCESS) dc->gamescope.OnBindMemory(image, memory);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateImageView(VkDevice device,
                                                    const VkImageViewCreateInfo* info,
                                                    const VkAllocationCallbacks* alloc,
                                                    VkImageView* view) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkCreateImageView) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = dc->table.vkCreateImageView(device, info, alloc, view);
    if (r == VK_SUCCESS && info) dc->gamescope.OnCreateView(*view, info->image);
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyImageView(VkDevice device, VkImageView view,
                                                 const VkAllocationCallbacks* alloc) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkDestroyImageView) return;
    dc->gamescope.OnDestroyView(view);
    dc->table.vkDestroyImageView(device, view, alloc);
}

VKAPI_ATTR void VKAPI_CALL Hook_UpdateDescriptorSets(VkDevice device, uint32_t writeCount,
                                                     const VkWriteDescriptorSet* writes,
                                                     uint32_t copyCount,
                                                     const VkCopyDescriptorSet* copies) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkUpdateDescriptorSets) return;
    if (writes) dc->gamescope.OnUpdateSets(writeCount, writes);
    dc->table.vkUpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint point,
                                                      VkPipelineLayout layout, uint32_t first,
                                                      uint32_t count, const VkDescriptorSet* sets,
                                                      uint32_t dynamicCount,
                                                      const uint32_t* dynamicOffsets) {
    DeviceChain* dc = DeviceForDispatch(cb);
    if (!dc || !dc->table.vkCmdBindDescriptorSets) return;
    if (sets) dc->gamescope.OnBindSets(cb, count, sets);
    dc->table.vkCmdBindDescriptorSets(cb, point, layout, first, count, sets, dynamicCount,
                                      dynamicOffsets);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_BeginCommandBuffer(VkCommandBuffer cb,
                                                       const VkCommandBufferBeginInfo* info) {
    DeviceChain* dc = DeviceForDispatch(cb);
    if (!dc || !dc->table.vkBeginCommandBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    dc->gamescope.OnBeginCommandBuffer(cb);
    {
        // Recorded again without having been submitted: whatever it carried never ran.
        std::lock_guard<std::mutex> lk(dc->lock);
        dc->gamescopeCarriers.erase(cb);
    }
    return dc->table.vkBeginCommandBuffer(cb, info);
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point,
                                                VkPipeline pipeline) {
    DeviceChain* dc = DeviceForDispatch(cb);
    if (!dc || !dc->table.vkCmdBindPipeline) return;
    dc->gamescope.OnBindPipeline(cb, point, pipeline);
    dc->table.vkCmdBindPipeline(cb, point, pipeline);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateCommandPool(VkDevice device,
                                                      const VkCommandPoolCreateInfo* info,
                                                      const VkAllocationCallbacks* alloc,
                                                      VkCommandPool* pool) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkCreateCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = dc->table.vkCreateCommandPool(device, info, alloc, pool);
    if (r == VK_SUCCESS && info) dc->gamescope.OnCreateCommandPool(*pool, info->queueFamilyIndex);
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                                                   const VkAllocationCallbacks* alloc) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkDestroyCommandPool) return;
    dc->gamescope.OnDestroyCommandPool(pool);
    dc->table.vkDestroyCommandPool(device, pool, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_AllocateCommandBuffers(VkDevice device,
                                                           const VkCommandBufferAllocateInfo* info,
                                                           VkCommandBuffer* cbs) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkAllocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = dc->table.vkAllocateCommandBuffers(device, info, cbs);
    if (r == VK_SUCCESS && info)
        dc->gamescope.OnAllocateCommandBuffers(info->commandPool, info->commandBufferCount, cbs);
    return r;
}

VKAPI_ATTR void VKAPI_CALL Hook_FreeCommandBuffers(VkDevice device, VkCommandPool pool,
                                                   uint32_t count, const VkCommandBuffer* cbs) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->table.vkFreeCommandBuffers) return;
    if (cbs) {
        dc->gamescope.OnFreeCommandBuffers(count, cbs);
        std::lock_guard<std::mutex> lk(dc->lock);
        for (uint32_t i = 0; i < count; ++i) dc->gamescopeCarriers.erase(cbs[i]);
    }
    dc->table.vkFreeCommandBuffers(device, pool, count, cbs);
}

// gamescope's composite, shaded in place: the chain is recorded into gamescope's own command buffer,
// right behind the dispatch that wrote the target. On gamescope's SDL backend the output images are
// swapchain images, which are not tracked, and the present hook shades them instead -- so no frame is
// shaded twice. Its screenshot and PipeWire textures are tracked on every backend.
bool ShadeGamescopeDispatch(DeviceChain* dc, VkCommandBuffer cb) {
    CompositeTarget target;
    if (!dc->gamescope.ClaimDispatch(cb, &target)) return false;

    uint32_t family = 0;
    if (!dc->gamescope.FamilyOf(cb, &family)) return false;

    std::lock_guard<std::mutex> lk(dc->lock);
    if (!ShmOpen(dc->shm)) return false;
    static std::once_flag namePublished;
    std::call_once(namePublished, [dc] { PublishGameName(dc); });

    // Never record graphics work for a queue that cannot run it. It would not fail -- it would hang
    // the batch, and gamescope with it, waiting on a frame that never finishes. That happens when
    // gamescope composites on a compute-only queue, which the environment variable set at instance
    // creation normally prevents; if somebody set it to 0, this is where it is caught.
    if (family >= dc->graphicsFamily.size() || !dc->graphicsFamily[family]) {
        static std::once_flag said;
        std::call_once(said, [] {
            Log("[gamescope] composite runs on a compute-only queue, where the chain cannot; "
                "passing frames through (set GAMESCOPE_FORCE_GENERAL_QUEUE=1)");
        });
        ShmStoreString(dc->shm.hdr->layerReasonSeq, dc->shm.hdr->layerReason, kReasonBytes,
                       "gamescope composites on a compute-only queue; set "
                       "GAMESCOPE_FORCE_GENERAL_QUEUE=1");
        return false;
    }

    const auto key = std::make_tuple(target.width, target.height, target.format, int(target.kind));
    auto [it, fresh] = dc->gamescopeTargets.try_emplace(key);
    SwapchainState& sc = it->second;
    if (fresh) {
        // Screenshots come and go at sizes of their own. Rather than let every one leave a chain
        // behind, a new capture size retires the other capture chains -- the display's is kept.
        if (target.kind == TargetKind::kCapture)
            for (auto o = dc->gamescopeTargets.begin(); o != dc->gamescopeTargets.end();) {
                const bool other = &o->second != &sc && std::get<3>(o->first) == int(target.kind);
                bool carried = false;
                for (auto& c : dc->gamescopeCarriers) carried |= c.second == &o->second;
                if (other && !carried) {
                    if (o->second.fencePending)
                        dc->table.vkWaitForFences(dc->self, 1, &o->second.fence, VK_TRUE,
                                                  kFenceTimeoutNs);
                    DestroySwapchainResources(dc, o->second);
                    o = dc->gamescopeTargets.erase(o);
                } else {
                    ++o;
                }
            }
        sc.width = target.width;
        sc.height = target.height;
        sc.format = target.format;
        sc.passThrough = !SupportedFormat(target.format);
        Log("[gamescope] shading its %s, %ux%u fmt=%d%s",
            target.kind == TargetKind::kOutput ? "composite" : "capture composite", sc.width,
            sc.height, (int) sc.format,
            sc.passThrough ? " (unsupported format, passing through)" : "");
    }
    if (sc.passThrough) return false;

    // Recorded on the render thread, with no queue in sight yet; the one it will be submitted to
    // is only known at the submit. Any queue of the family serves for what ShadeFrame keeps.
    const bool enabled = ShmEnabled(dc->shm.hdr);
    const bool paused = dc->shm.hdr->paused.load() != 0;
    const bool wantChain = enabled && !ShmPresetId(dc->shm.hdr).empty();
    const FrameResult fr = ShadeFrame(dc, sc, VK_NULL_HANDLE, target.image, family, wantChain,
                                      paused, /*submit=*/false, 0, nullptr, target.view,
                                      VK_IMAGE_LAYOUT_GENERAL, (const void*) target.image, nullptr,
                                      cb);

    // The chain bound a compute pipeline of its own for the write-back. gamescope binds its
    // descriptor sets and uniforms on every dispatch, but its pipeline only when it changes, so the
    // next dispatch would run the layer's -- put gamescope's back.
    if (const VkPipeline mine = dc->gamescope.ComputePipelineOf(cb))
        dc->table.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mine);

    if (!fr.composed) return false;
    dc->gamescopeCarriers[cb] = &sc;
    return true;
}

VKAPI_ATTR void VKAPI_CALL Hook_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y,
                                            uint32_t z) {
    DeviceChain* dc = DeviceForDispatch(cb);
    if (!dc || !dc->table.vkCmdDispatch) return;
    dc->table.vkCmdDispatch(cb, x, y, z);
    if (dc->inert || !LayerEnabled()) return;
    const bool shaded = ShadeGamescopeDispatch(dc, cb);
    if (Verbose()) {
        static std::atomic<uint64_t> seen {0}, hits {0};
        const uint64_t n = seen.fetch_add(1) + 1;
        if (shaded) hits.fetch_add(1);
        if (n == 1 || n % 60 == 0)
            Log("[gamescope] %llu dispatches, %llu shaded, %zu targets tracked",
                (unsigned long long) n, (unsigned long long) hits.load(),
                dc->gamescope.TrackedImages());
    }
}

// The command buffers the chain was recorded into are gamescope's, submitted without a fence -- it
// tracks completion with a timeline semaphore. The chain's fence, which says when its surfaces are
// free to reuse, is put on the submission here; if gamescope passed a fence, an empty submission
// after it carries the chain's instead, since a fence covers every earlier submission on the queue.
template <typename Submit>
VkResult SubmitCarrying(DeviceChain* dc, VkQueue queue, uint32_t count, const Submit* submits,
                        VkFence fence, VkResult (*down)(DeviceChain*, VkQueue, uint32_t,
                                                        const Submit*, VkFence),
                        const std::vector<SwapchainState*>& carried) {
    if (Verbose()) {
        static std::atomic<uint64_t> n {0};
        if (n.fetch_add(1) % 60 == 0)
            Log("[gamescope] submission carries the chain (%zu), fence %s", carried.size(),
                fence ? "gamescope's + ours" : "ours");
    }
    VkFence first = fence ? fence : carried[0]->fence;
    VkResult r = down(dc, queue, count, submits, first);
    if (r != VK_SUCCESS) return r;
    for (size_t i = 0; i < carried.size(); ++i) {
        if (!fence && i == 0) {
            carried[i]->fencePending = true;
            continue;
        }
        const VkResult e = dc->vkQueueSubmit(queue, 0, nullptr, carried[i]->fence);
        if (e == VK_SUCCESS) carried[i]->fencePending = true;
    }
    return r;
}

std::vector<SwapchainState*> TakeCarried(DeviceChain* dc, const VkCommandBuffer* cbs,
                                         uint32_t n, std::vector<SwapchainState*> carried) {
    for (uint32_t c = 0; c < n; ++c) {
        auto it = dc->gamescopeCarriers.find(cbs[c]);
        if (it == dc->gamescopeCarriers.end()) continue;
        if (std::find(carried.begin(), carried.end(), it->second) == carried.end())
            carried.push_back(it->second);
        dc->gamescopeCarriers.erase(it);
    }
    return carried;
}

VkResult DownSubmit(DeviceChain* dc, VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    return dc->vkQueueSubmit(q, n, s, f);
}

VkResult DownSubmit2(DeviceChain* dc, VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    return dc->vkQueueSubmit2(q, n, s, f);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    AppCall call(dc);
    if (InGamescope() && pSubmits) {
        std::lock_guard<std::mutex> lk(dc->lock);
        if (!dc->gamescopeCarriers.empty()) {
            std::vector<SwapchainState*> carried;
            for (uint32_t b = 0; b < submitCount; ++b)
                carried = TakeCarried(dc, pSubmits[b].pCommandBuffers,
                                      pSubmits[b].commandBufferCount, std::move(carried));
            if (!carried.empty())
                return SubmitCarrying(dc, queue, submitCount, pSubmits, fence, DownSubmit,
                                      carried);
        }
    }
    return dc->vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                 const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    AppCall call(dc);
    if (InGamescope() && pSubmits) {
        std::lock_guard<std::mutex> lk(dc->lock);
        if (!dc->gamescopeCarriers.empty()) {
            std::vector<SwapchainState*> carried;
            for (uint32_t b = 0; b < submitCount; ++b) {
                std::vector<VkCommandBuffer> cbs;
                for (uint32_t c = 0; c < pSubmits[b].commandBufferInfoCount; ++c)
                    cbs.push_back(pSubmits[b].pCommandBufferInfos[c].commandBuffer);
                carried = TakeCarried(dc, cbs.data(), uint32_t(cbs.size()), std::move(carried));
            }
            if (!carried.empty())
                return SubmitCarrying(dc, queue, submitCount, pSubmits, fence, DownSubmit2,
                                      carried);
        }
    }
    return dc->vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    AppCall call(dc);
    return dc->vkQueueWaitIdle(queue);
}

// Waiting for the device is a use of every one of its queues, as far as synchronisation goes.
VKAPI_ATTR VkResult VKAPI_CALL Hook_DeviceWaitIdle(VkDevice device) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkDeviceWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    AppCall call(dc);
    return dc->vkDeviceWaitIdle(device);
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
    if (!strcmp(n, "vkAcquireNextImage2KHR"))
        return (PFN_vkVoidFunction) Hook_AcquireNextImage2KHR;
    if (!strcmp(n, "vkDeviceWaitIdle")) return (PFN_vkVoidFunction) Hook_DeviceWaitIdle;
    if (!strcmp(n, "vkQueueSubmit")) return (PFN_vkVoidFunction) Hook_QueueSubmit;
    if (!strcmp(n, "vkQueueSubmit2")) return (PFN_vkVoidFunction) Hook_QueueSubmit2;
    if (!strcmp(n, "vkQueueWaitIdle")) return (PFN_vkVoidFunction) Hook_QueueWaitIdle;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction) Hook_QueuePresentKHR;

    // Only inside gamescope. Anywhere else these are not in the chain at all, which is the only way to
    // be sure they cost a game nothing.
    if (InGamescope()) {
        if (!strcmp(n, "vkCreateImage")) return (PFN_vkVoidFunction) Hook_CreateImage;
        if (!strcmp(n, "vkDestroyImage")) return (PFN_vkVoidFunction) Hook_DestroyImage;
        if (!strcmp(n, "vkBindImageMemory")) return (PFN_vkVoidFunction) Hook_BindImageMemory;
        if (!strcmp(n, "vkCreateImageView")) return (PFN_vkVoidFunction) Hook_CreateImageView;
        if (!strcmp(n, "vkDestroyImageView")) return (PFN_vkVoidFunction) Hook_DestroyImageView;
        if (!strcmp(n, "vkUpdateDescriptorSets"))
            return (PFN_vkVoidFunction) Hook_UpdateDescriptorSets;
        if (!strcmp(n, "vkCmdBindDescriptorSets"))
            return (PFN_vkVoidFunction) Hook_CmdBindDescriptorSets;
        if (!strcmp(n, "vkBeginCommandBuffer")) return (PFN_vkVoidFunction) Hook_BeginCommandBuffer;
        if (!strcmp(n, "vkCmdBindPipeline")) return (PFN_vkVoidFunction) Hook_CmdBindPipeline;
        if (!strcmp(n, "vkCmdDispatch")) return (PFN_vkVoidFunction) Hook_CmdDispatch;
        if (!strcmp(n, "vkCreateCommandPool")) return (PFN_vkVoidFunction) Hook_CreateCommandPool;
        if (!strcmp(n, "vkDestroyCommandPool")) return (PFN_vkVoidFunction) Hook_DestroyCommandPool;
        if (!strcmp(n, "vkAllocateCommandBuffers"))
            return (PFN_vkVoidFunction) Hook_AllocateCommandBuffers;
        if (!strcmp(n, "vkFreeCommandBuffers")) return (PFN_vkVoidFunction) Hook_FreeCommandBuffers;
    }
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
    //
    // Silent when SHADERGLASS is not set at all. The layer is named in the session's VK_INSTANCE_LAYERS
    // so that it lands below DLSS5VKLayer (see docs/DLSS5VKLayer.md), and that loads it into every
    // Vulkan program the user runs -- a banner in each of their stderr streams would be noise from a
    // layer nobody asked to turn on. Set to anything else, it still announces itself: that is somebody
    // who meant to enable it and wrote 0, or true, and the banner is how they find out.
    static std::once_flag announced;
    std::call_once(announced, [] {
        const char* e = getenv("SHADERGLASS");
        if (!e) return;
        Log("=== %s loaded (SHADERGLASS=%s) ===", VK_LAYER_NAME, e);
        if (LayerEnabled()) InstallCrashTrace();
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
