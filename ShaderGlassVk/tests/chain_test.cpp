/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The pass model, over the whole catalogue, with no game and no window.

Driving presets through a real game proves a handful of them a few seconds at a time, which is not
enough to find the long tail: a preset whose eleventh pass wants a format the device will not render
to, or whose shader declares a push constant block larger than the device allows, or whose feedback
pass samples an image nothing has written. Those are build-time and record-time failures, and none
of them need a swapchain.

So this creates a device, hands the chain an ordinary image where the swapchain's would go -- the
reason Chain::Record takes the external layout rather than assuming PRESENT_SRC_KHR -- and builds
and records every preset in turn. Run it under the validation layer and it checks the recording too:

    VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation \
    SHADERGLASS_PRESETS=build/catalog/cm/libShaderGlassPresets.so build/native/tools/chain_test

    chain_test [--isolate] [--dump DIR] [--stride N] [--limit N] [--width W] [--height H]
               [preset-id ...]

--dump puts a test pattern through each preset and writes the result as a PPM, which is the only
way to tell a working shader from one that merely ran: "the bytes changed" is satisfied just as
well by a black screen.

Some presets do not fail, they take the process down: a GPU driver's shader compiler segfaulting on
SPIR-V that spirv-val accepts is a driver bug, but it is one this layer has to know about, because
the pipeline is built inside someone else's game. --isolate runs the sweep in a forked child and
restarts it past whatever killed it, so one such preset does not hide the rest.
*/

#include "../layer/src/catalogue.h"
#include "../layer/src/chain.h"
#include "../layer/src/pixel_grid.h"
#include "../layer/src/vk_table.h"

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace shaderglass;

namespace {

// The layer is built with VK_NO_PROTOTYPES: it never calls a Vulkan function by name, only through
// the tables it is handed. That leaves four bootstrap entry points to find here, exactly the ones
// a loader starts from. Everything after that comes out of the same tables the layer uses, which
// is the point -- the chain is exercised through the interface it actually has.
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
PFN_vkCreateInstance g_createInstance = nullptr;
PFN_vkCreateDevice g_createDevice = nullptr;
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;

bool LoadLoader() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "could not load the Vulkan loader: %s\n", dlerror());
        return false;
    }

    g_gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
    if (!g_gipa) {
        std::fprintf(stderr, "the Vulkan loader has no vkGetInstanceProcAddr\n");
        return false;
    }

    g_createInstance = (PFN_vkCreateInstance) g_gipa(nullptr, "vkCreateInstance");
    return g_createInstance != nullptr;
}

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    InstanceTable instanceTable;
    DeviceTable deviceTable;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

bool CreateDevice(Device& d) {
    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "shaderglass chain_test";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (g_createInstance(&ici, nullptr, &d.instance) != VK_SUCCESS) {
        std::fprintf(stderr, "could not create a Vulkan instance\n");
        return false;
    }

    // The instance table can be filled now, and covers everything else at instance level.
    d.instanceTable.next_gipa = g_gipa;
    d.instanceTable.Load(d.instance);
    g_createDevice = (PFN_vkCreateDevice) g_gipa(d.instance, "vkCreateDevice");
    g_gdpa = (PFN_vkGetDeviceProcAddr) g_gipa(d.instance, "vkGetDeviceProcAddr");
    if (!g_createDevice || !g_gdpa) {
        std::fprintf(stderr, "the loader is missing vkCreateDevice or vkGetDeviceProcAddr\n");
        return false;
    }

    uint32_t count = 0;
    d.instanceTable.vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
    if (!count) {
        std::fprintf(stderr, "no Vulkan devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    d.instanceTable.vkEnumeratePhysicalDevices(d.instance, &count, devices.data());
    d.physical = devices[0];

    VkPhysicalDeviceProperties props {};
    d.instanceTable.vkGetPhysicalDeviceProperties(d.physical, &props);
    std::printf("device: %s\n", props.deviceName);

    uint32_t families = 0;
    d.instanceTable.vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> familyProps(families);
    d.instanceTable.vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families,
                                                              familyProps.data());

    bool found = false;
    for (uint32_t i = 0; i < families; ++i) {
        if (familyProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            d.family = i;
            found = true;
            break;
        }
    }
    if (!found) {
        std::fprintf(stderr, "no graphics queue\n");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (g_createDevice(d.physical, &dci, nullptr, &d.device) != VK_SUCCESS) {
        std::fprintf(stderr, "could not create a device\n");
        return false;
    }
    d.deviceTable.next_dpa = g_gdpa;
    d.deviceTable.Load(d.device);
    d.deviceTable.vkGetDeviceQueue(d.device, d.family, 0, &d.queue);

    VkCommandPoolCreateInfo pci {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.family;
    if (d.deviceTable.vkCreateCommandPool(d.device, &pci, nullptr, &d.pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo cbai {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = d.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (d.deviceTable.vkAllocateCommandBuffers(d.device, &cbai, &d.cb) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (d.deviceTable.vkCreateFence(d.device, &fci, nullptr, &d.fence) != VK_SUCCESS) return false;

    return true;
}

// Build and record presets[from..], creating a device of its own. Every preset it gets through is
// reported by index down `progress`, so a caller that is watching knows exactly where it stopped if
// it stops the hard way.
// A test pattern with enough structure to judge a shader by: a grey ramp, saturated colour bars,
// a fine checkerboard for anything that resamples, and a white border. A CRT shader over this
// should still look like this, scanlines and mask aside -- which is the point, since "the bytes
// changed" says nothing about whether the result is an image.
void FillPattern(uint8_t* bgra, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0;

            const uint32_t band = (y * 4) / h;
            if (band == 0) {
                r = g = b = uint8_t((x * 255) / (w ? w : 1));         // grey ramp
            } else if (band == 1) {
                const uint32_t bar = (x * 6) / (w ? w : 1);           // colour bars
                r = (bar == 0 || bar == 3 || bar == 5) ? 255 : 0;
                g = (bar == 1 || bar == 3 || bar == 4) ? 255 : 0;
                b = (bar == 2 || bar == 4 || bar == 5) ? 255 : 0;
            } else if (band == 2) {
                const bool on = ((x / 4) + (y / 4)) % 2 == 0;         // checkerboard
                r = g = b = on ? 230 : 25;
            } else {
                r = uint8_t((x * 255) / (w ? w : 1));                 // colour ramp
                g = uint8_t((y * 255) / (h ? h : 1));
                b = 128;
            }

            if (x < 2 || y < 2 || x + 2 >= w || y + 2 >= h) r = g = b = 255;  // border

            uint8_t* px = bgra + (size_t(y) * w + x) * 4;
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = 255;
        }
    }
}

// Decode one texel of the formats a preset's passes actually use, to float RGBA. Enough to tell a
// signal from a black screen, which is all this is for.
bool DecodeTexel(VkFormat f, const uint8_t* p, float* rgba, uint32_t* texelBytes) {
    const auto half = [](uint16_t h) {
        const uint32_t sign = uint32_t(h >> 15) << 31;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t man = h & 0x3FF;
        uint32_t bits;
        if (exp == 0) {
            if (man == 0) { bits = sign; }
            else {  // subnormal: renormalise
                exp = 127 - 15 + 1;
                while ((man & 0x400) == 0) { man <<= 1; --exp; }
                man &= 0x3FF;
                bits = sign | (exp << 23) | (man << 13);
            }
        } else if (exp == 0x1F) {
            bits = sign | 0x7F800000u | (man << 13);  // inf / NaN
        } else {
            bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
        }
        float out;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    };

    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            rgba[0] = p[2] / 255.0f; rgba[1] = p[1] / 255.0f;
            rgba[2] = p[0] / 255.0f; rgba[3] = p[3] / 255.0f;
            *texelBytes = 4;
            return true;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (int i = 0; i < 4; ++i) rgba[i] = p[i] / 255.0f;
            *texelBytes = 4;
            return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
            uint32_t v;
            std::memcpy(&v, p, sizeof(v));
            rgba[0] = float(v & 0x3FF) / 1023.0f;
            rgba[1] = float((v >> 10) & 0x3FF) / 1023.0f;
            rgba[2] = float((v >> 20) & 0x3FF) / 1023.0f;
            rgba[3] = float((v >> 30) & 0x3) / 3.0f;
            *texelBytes = 4;
            return true;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            for (int i = 0; i < 4; ++i) {
                uint16_t h;
                std::memcpy(&h, p + i * 2, sizeof(h));
                rgba[i] = half(h);
            }
            *texelBytes = 8;
            return true;
        }
        case VK_FORMAT_R16G16B16A16_UNORM: {
            for (int i = 0; i < 4; ++i) {
                uint16_t v;
                std::memcpy(&v, p + i * 2, sizeof(v));
                rgba[i] = v / 65535.0f;
            }
            *texelBytes = 8;
            return true;
        }
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            std::memcpy(rgba, p, sizeof(float) * 4);
            *texelBytes = 16;
            return true;
        default:
            return false;
    }
}

const char* FormatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8_SRGB";
        case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
        case VK_FORMAT_R16G16B16A16_UNORM: return "RGBA16";
        case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
        default: return "?";
    }
}

void WritePpm(const char* path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const uint8_t rgb[3] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4 + 0]};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int RunRange(const std::vector<std::string>& ids, size_t from, uint32_t width, uint32_t height,
             bool verbose, int progress, size_t* built, size_t* failed,
             const std::string& dumpDir, bool stats, bool probe) {
    Device d;
    if (!CreateDevice(d)) return 2;

    // Where the swapchain image would be. GENERAL rather than PRESENT_SRC_KHR, because this image
    // did not come from a swapchain and validation is right to object if it is told otherwise.
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    Image external {};
    // The chain only ever copies to and from this, but MakeImage gives every image a view, and a
    // view needs a usage that permits one. COLOR_ATTACHMENT | SAMPLED is also what a real swapchain
    // image carries, so the stand-in is the shape of the thing it stands in for.
    if (!MakeImage(&d.deviceTable, &d.instanceTable, d.device, d.physical, external, width, height,
                   format,
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        std::fprintf(stderr, "could not create the stand-in image\n");
        return 2;
    }

    // Host-visible scratch the size of the image, for putting the pattern in and reading the
    // result back out. Only needed when dumping.
    // Both --dump and --stats need a real picture going in: measuring what a chain does to an
    // image it was never given is measuring nothing, and every pass dutifully reports black.
    // The detector needs a real picture too -- there is no grid in an image nobody drew.
    const bool wantPattern = !dumpDir.empty() || stats || probe;

    HostBuffer scratch {};
    const VkDeviceSize imageBytes = VkDeviceSize(width) * height * 4;
    if (wantPattern &&
        !MakeHostBuffer(&d.deviceTable, &d.instanceTable, d.device, d.physical, scratch, imageBytes,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "could not allocate the readback buffer\n");
        return 2;
    }

    const auto submitAndWait = [&d]() {
        VkSubmitInfo si {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &d.cb;
        d.deviceTable.vkQueueSubmit(d.queue, 1, &si, d.fence);
        d.deviceTable.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
        d.deviceTable.vkResetFences(d.device, 1, &d.fence);
    };

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    {
        d.deviceTable.vkBeginCommandBuffer(d.cb, &begin);
        Transition(&d.deviceTable, d.cb, external, VK_IMAGE_LAYOUT_GENERAL);
        d.deviceTable.vkEndCommandBuffer(d.cb);
        submitAndWait();
    }

    if (!dumpDir.empty()) {
        FillPattern((uint8_t*) scratch.mapped, width, height);
        WritePpm((dumpDir + "/input.ppm").c_str(), (const uint8_t*) scratch.mapped, width, height);
    }

    for (size_t i = from; i < ids.size(); ++i) {
        const std::string& id = ids[i];

        // Named before it is built, and flushed, so that when something goes wrong inside the
        // driver the last line printed is the preset that did it.
        if (verbose) {
            std::printf("  %s\n", id.c_str());
            std::fflush(stdout);
        }

        Chain chain(&d.deviceTable, &d.instanceTable, d.device, d.physical);
        if (!chain.Prepare(width, height, format, width, height, id)) {
            std::printf("FAIL  %-60s %s\n", id.c_str(), chain.Reason());
            std::fflush(stdout);
            ++*failed;
        } else {
            // Two frames, submitted and waited on separately, because that is the contract: the
            // chain may only release what a frame was using once that frame's fence has retired,
            // and FrameCompleted is where it does it. Two frames rather than one so a feedback
            // pass has a previous frame to read and the history ring turns over.
            bool ok = true;
            for (uint64_t frame = 1; frame <= 2 && ok; ++frame) {
                // Before *every* frame, not merely every preset. The chain copies its result back
                // into this image, exactly as it does to a real swapchain -- but a game renders a
                // fresh frame each time and nothing here does, so without re-laying the pattern the
                // second frame is fed the first frame's output. A long chain then looks like it
                // fades to black, and the fade is the harness, not the chain.
                if (wantPattern) {
                    FillPattern((uint8_t*) scratch.mapped, width, height);
                    d.deviceTable.vkBeginCommandBuffer(d.cb, &begin);
                    Transition(&d.deviceTable, d.cb, external, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkBufferImageCopy up {};
                    up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    up.imageExtent = {width, height, 1};
                    d.deviceTable.vkCmdCopyBufferToImage(d.cb, scratch.buffer, external.image,
                                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                                         &up);
                    Transition(&d.deviceTable, d.cb, external, VK_IMAGE_LAYOUT_GENERAL);
                    d.deviceTable.vkEndCommandBuffer(d.cb);
                    submitAndWait();
                }

                VkCommandBufferBeginInfo bi {};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (d.deviceTable.vkBeginCommandBuffer(d.cb, &bi) != VK_SUCCESS) {
                    std::printf("FAIL  %-60s could not begin a command buffer\n", id.c_str());
                    std::fflush(stdout);
                    ok = false;
                    break;
                }

                // The source detector's readback, exercised on a real device. It packs a couple
                // of hundred copy regions out of the frame image and reads them back on the host,
                // and none of that is reachable from the pure unit test -- so without this the
                // first thing to run it is a game.
                if (probe) chain.RequestGridProbe();

                ok = chain.Record(d.cb, external.image, frame, VK_IMAGE_LAYOUT_GENERAL);
                d.deviceTable.vkEndCommandBuffer(d.cb);
                if (!ok) {
                    std::printf("FAIL  %-60s could not record\n", id.c_str());
                    std::fflush(stdout);
                    break;
                }

                VkSubmitInfo si {};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &d.cb;
                if (d.deviceTable.vkQueueSubmit(d.queue, 1, &si, d.fence) != VK_SUCCESS) {
                    std::printf("FAIL  %-60s submit rejected\n", id.c_str());
                    std::fflush(stdout);
                    ok = false;
                    break;
                }
                d.deviceTable.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
                d.deviceTable.vkResetFences(d.device, 1, &d.fence);
                chain.FrameCompleted();

                if (probe) {
                    GridEstimate g {};
                    if (chain.TakeGridEstimate(&g))
                        std::printf("  probe %-40s %s x%.2f y%.2f conf %.2f\n", id.c_str(),
                                    g.Valid() ? "measured" : "no grid ", double(g.scaleX),
                                    double(g.scaleY), double(g.confidence));
                }
            }

            // Per-pass statistics, which is what tells you where a chain went wrong. A signal
            // chain's intermediate passes are not images, so this reports ranges and NaN counts
            // rather than writing pictures nobody can read.
            if (ok && stats) {
                std::printf("  %s\n", id.c_str());
                for (uint32_t pi = 0; pi < chain.PassCount(); ++pi) {
                    const Chain::PassInfo info = chain.PassAt(pi);
                    if (!info.image) continue;

                    const VkDeviceSize need = VkDeviceSize(info.width) * info.height * 16;
                    HostBuffer rb {};
                    if (!MakeHostBuffer(&d.deviceTable, &d.instanceTable, d.device, d.physical, rb,
                                        need, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
                        continue;

                    d.deviceTable.vkBeginCommandBuffer(d.cb, &begin);
                    VkImageMemoryBarrier b {};
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.oldLayout = info.layout;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = info.image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    d.deviceTable.vkCmdPipelineBarrier(d.cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                                       0, nullptr, 1, &b);

                    VkBufferImageCopy r {};
                    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    r.imageExtent = {info.width, info.height, 1};
                    d.deviceTable.vkCmdCopyImageToBuffer(d.cb, info.image,
                                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                         rb.buffer, 1, &r);

                    // Straight back, so the next frame finds it where the chain left it.
                    std::swap(b.oldLayout, b.newLayout);
                    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    d.deviceTable.vkCmdPipelineBarrier(d.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                                       nullptr, 0, nullptr, 1, &b);
                    d.deviceTable.vkEndCommandBuffer(d.cb);
                    submitAndWait();

                    float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f};
                    float hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                    double sum[4] = {0, 0, 0, 0};
                    size_t nan = 0, nanCh[4] = {0, 0, 0, 0}, texels = 0, texelBytes = 0;
                    const uint8_t* px = (const uint8_t*) rb.mapped;
                    bool known = true;

                    for (size_t t = 0; t < size_t(info.width) * info.height && known; ++t) {
                        float c[4];
                        uint32_t stride = 0;
                        if (!DecodeTexel(info.format, px + t * (texelBytes ? texelBytes : 16), c,
                                         &stride)) {
                            known = false;
                            break;
                        }
                        texelBytes = stride;
                        for (int i = 0; i < 4; ++i) {
                            if (c[i] != c[i]) { ++nan; ++nanCh[i]; }
                            else {
                                lo[i] = std::min(lo[i], c[i]);
                                hi[i] = std::max(hi[i], c[i]);
                                sum[i] += c[i];
                            }
                        }
                        ++texels;
                    }

                    if (!known) {
                        std::printf("    %2u %-34s %4ux%-4u %-12s (not decoded here)\n", pi,
                                    info.name, info.width, info.height, FormatName(info.format));
                    } else {
                        char nanNote[64] = "";
                        if (nan)
                            snprintf(nanNote, sizeof(nanNote),
                                     "  *** NaN r%zu g%zu b%zu a%zu of %zu ***", nanCh[0], nanCh[1],
                                     nanCh[2], nanCh[3], texels);
                        char flags[64] = "";
                        snprintf(flags, sizeof(flags), "%s%s%s%s",
                                 info.feedback ? " fb" : "", info.levels > 1 ? " mip" : "",
                                 info.alias && info.alias[0] ? " @" : "",
                                 info.alias && info.alias[0] ? info.alias : "");
                        std::printf("    %2u [img %p] %-34s %4ux%-4u %-12s rgb [%.3f..%.3f] mean %.3f%s%s\n",
                                    pi, (void*) info.image, info.name, info.width, info.height,
                                    FormatName(info.format), std::min({lo[0], lo[1], lo[2]}),
                                    std::max({hi[0], hi[1], hi[2]}),
                                    texels ? (sum[0] + sum[1] + sum[2]) / (3.0 * double(texels)) : 0.0,
                                    nan ? nanNote : "", flags);
                    }
                    DropHostBuffer(&d.deviceTable, d.device, rb);
                }
            }

            if (ok && !dumpDir.empty()) {
                d.deviceTable.vkBeginCommandBuffer(d.cb, &begin);
                Transition(&d.deviceTable, d.cb, external, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy r {};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {width, height, 1};
                d.deviceTable.vkCmdCopyImageToBuffer(d.cb, external.image,
                                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                     scratch.buffer, 1, &r);
                Transition(&d.deviceTable, d.cb, external, VK_IMAGE_LAYOUT_GENERAL);
                d.deviceTable.vkEndCommandBuffer(d.cb);
                submitAndWait();

                std::string name = id;
                for (char& c : name)
                    if (c == '/' || c == ' ') c = '_';
                WritePpm((dumpDir + "/" + name + ".ppm").c_str(),
                         (const uint8_t*) scratch.mapped, width, height);
            }

            if (ok) ++*built; else ++*failed;
        }

        // Written after the preset is finished with, so a reader that sees index i knows i is safe.
        if (progress >= 0) {
            const size_t done = i;
            if (write(progress, &done, sizeof(done)) != (ssize_t) sizeof(done)) {
                // Nothing useful to do about it; the parent will simply resume further back.
            }
        }
        if ((*built + *failed) % 250 == 0) {
            std::printf("  %zu / %zu ...\n", *built + *failed, ids.size());
            std::fflush(stdout);
        }
    }

    DropHostBuffer(&d.deviceTable, d.device, scratch);
    DropImage(&d.deviceTable, d.device, external);
    d.deviceTable.vkDestroyFence(d.device, d.fence, nullptr);
    d.deviceTable.vkDestroyCommandPool(d.device, d.pool, nullptr);
    d.deviceTable.vkDestroyDevice(d.device, nullptr);
    d.instanceTable.vkDestroyInstance(d.instance, nullptr);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t width = 640, height = 480, stride = 1, limit = 0;
    bool verbose = false, isolate = false, stats = false, probe = false;
    std::string dumpDir;
    std::vector<std::string> only;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stride" && i + 1 < argc) stride = uint32_t(atoi(argv[++i]));
        else if (arg == "--limit" && i + 1 < argc) limit = uint32_t(atoi(argv[++i]));
        else if (arg == "--width" && i + 1 < argc) width = uint32_t(atoi(argv[++i]));
        else if (arg == "--height" && i + 1 < argc) height = uint32_t(atoi(argv[++i]));
        else if (arg == "--verbose") verbose = true;
        else if (arg == "--isolate") isolate = true;
        else if (arg == "--dump" && i + 1 < argc) dumpDir = argv[++i];
        else if (arg == "--stats") stats = true;
        else if (arg == "--probe") probe = true;
        else only.push_back(arg);
    }
    if (!stride) stride = 1;

    if (!LoadLoader()) return 2;

    std::vector<std::string> ids;
    if (!only.empty()) {
        ids = only;
    } else {
        const size_t count = Catalogue::Instance().Count();
        if (!count) {
            std::fprintf(stderr,
                         "no catalogue: set SHADERGLASS_PRESETS to a libShaderGlassPresets.so\n");
            return 2;
        }
        for (size_t i = 0; i < count; i += stride) {
            const SgPreset* p = Catalogue::Instance().At(i);
            if (p && p->id) ids.push_back(p->id);
            if (limit && ids.size() >= limit) break;
        }
    }

    std::printf("running %zu preset%s at %ux%u\n\n", ids.size(), ids.size() == 1 ? "" : "s", width,
                height);

    size_t built = 0, failed = 0;

    if (!isolate) {
        const int rc =
            RunRange(ids, 0, width, height, verbose, -1, &built, &failed, dumpDir, stats, probe);
        if (rc) return rc;
        std::printf("\n%zu built and recorded, %zu failed\n", built, failed);
        return failed ? 1 : 0;
    }

    // Isolated: each attempt runs in a child, and anything that kills the child is attributed to
    // the preset after the last one the child reported finishing.
    std::vector<std::string> crashed;
    size_t next = 0;
    while (next < ids.size()) {
        int fds[2];
        if (pipe(fds) != 0) {
            std::fprintf(stderr, "could not create a pipe\n");
            return 2;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            std::fprintf(stderr, "could not fork\n");
            return 2;
        }
        if (pid == 0) {
            close(fds[0]);
            size_t childBuilt = 0, childFailed = 0;
            const int rc =
                RunRange(ids, next, width, height, verbose, fds[1], &childBuilt,
                         &childFailed, dumpDir, stats, probe);
            // Counts travel back through the pipe's last two words rather than a second channel.
            const size_t tally[2] = {childBuilt, childFailed};
            ssize_t ignored = write(fds[1], tally, sizeof(tally));
            (void) ignored;
            _exit(rc);
        }

        close(fds[1]);
        size_t lastDone = SIZE_MAX, word = 0, tail[2] = {0, 0};
        size_t words = 0;
        while (read(fds[0], &word, sizeof(word)) == (ssize_t) sizeof(word)) {
            tail[0] = tail[1];
            tail[1] = word;
            ++words;
            if (word < ids.size()) lastDone = word;
        }
        close(fds[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
            // The child finished the list. Its last two words are the tally.
            if (words >= 2) {
                built += tail[0];
                failed += tail[1];
            }
            break;
        }

        const size_t at = (lastDone == SIZE_MAX) ? next : lastDone + 1;
        if (at < ids.size()) {
            std::printf("CRASH %-60s killed the process (signal %d)\n", ids[at].c_str(),
                        WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            std::fflush(stdout);
            crashed.push_back(ids[at]);
        }
        // Everything the child got through counts, and the run resumes past what killed it.
        built += (at > next) ? (at - next) : 0;
        next = at + 1;
    }

    std::printf("\n%zu built and recorded, %zu failed, %zu crashed the driver\n", built, failed,
                crashed.size());
    for (const auto& id : crashed) std::printf("  crashed: %s\n", id.c_str());
    return (failed || !crashed.empty()) ? 1 : 0;
}
