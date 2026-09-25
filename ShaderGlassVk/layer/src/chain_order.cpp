/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "chain_order.h"

#include <dlfcn.h>
#include <fstream>

namespace shaderglass {
namespace {

// The object's name, not the layer's. A layer is identified here by the file it lives in because
// that is what a function pointer can be traced back to; the manifest name never reaches us.
const char* kDlssObject = "libVkLayer_NV_dlssnr";

bool PathNamesDlss(const char* path) {
    if (!path) return false;
    const std::string s = path;
    return s.find(kDlssObject) != std::string::npos;
}

// Whether DLSS5VKLayer is mapped into this process at all. The loader loads every layer's library
// before calling any of them, so this answers "is it in the chain" without answering "where".
bool DlssLoaded() {
    std::ifstream maps("/proc/self/maps");
    if (!maps) return false;
    std::string line;
    while (std::getline(maps, line))
        if (line.find(kDlssObject) != std::string::npos) return true;
    return false;
}

// Whether the shared object at `path` is a Vulkan layer. RTLD_NOLOAD, so this only looks at what is
// already mapped and never loads anything new into the game.
bool IsLayerObject(const char* path) {
    void* h = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    const bool layer = dlsym(h, "vkNegotiateLoaderLayerInterfaceVersion") != nullptr;
    dlclose(h);  // drops the reference NOLOAD took; the object stays mapped for its real owner
    return layer;
}

}  // namespace

DlssPosition FindDlssPosition(void* nextPresent, std::string* nextOwner) {
    // Whose code is the next link in the chain? Resolved first, and reported whatever the verdict,
    // because this is the evidence -- and an unexpected owner here is the difference between a
    // chain that is merely ordered oddly and one nobody has understood yet.
    Dl_info info {};
    const bool resolved = nextPresent && dladdr(nextPresent, &info) && info.dli_fname;
    if (nextOwner) *nextOwner = resolved ? info.dli_fname : "(unresolved)";

    if (!DlssLoaded()) return DlssPosition::Absent;

    // No pointer at all is no evidence: a device created without VK_KHR_swapchain has no present to
    // look at, and says nothing about the chain.
    if (!nextPresent) return DlssPosition::Unknown;

    // A pointer the dynamic linker cannot put a name to is not a layer -- every layer is a shared
    // object and resolves. It is the driver's or the loader's own code, which means nothing sits
    // below this layer at all, so DLSS, being loaded, must be above.
    if (!resolved) return DlssPosition::Above;

    {
        if (PathNamesDlss(info.dli_fname)) return DlssPosition::Below;

        // Somebody's code, and not DLSS's. Whether that makes DLSS above depends on what the
        // somebody is: if it is the driver, nothing sits below this layer at all and DLSS must be
        // above; if it is another layer, DLSS could still be further down, and the question stays
        // open.
        //
        // Asked of the object rather than guessed from its name. An earlier version kept a list of
        // driver library names and missed NVIDIA's actual one, libnvidia-glcore, reporting a
        // correctly ordered chain as undecided. Every layer exports the loader's negotiation entry
        // point, because the loader will not load it otherwise; a driver exports its own instead.
        return IsLayerObject(info.dli_fname) ? DlssPosition::Unknown : DlssPosition::Above;
    }
}

const char* DescribeDlssPosition(DlssPosition p) {
    switch (p) {
        case DlssPosition::Absent: return "DLSS5VKLayer is not in the chain";
        case DlssPosition::Above:
            return "DLSS5VKLayer runs first; the shader lands on its output";
        case DlssPosition::Below:
            return "DLSS5VKLayer runs after ShaderGlass and will reconstruct an already-shadered "
                   "image";
        case DlssPosition::Unknown:
        default: return "DLSS5VKLayer is loaded, but another layer sits between us";
    }
}

}  // namespace shaderglass
