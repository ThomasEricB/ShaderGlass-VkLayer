/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "catalogue.h"
#include "log.h"

#include <dlfcn.h>
#include <string>
#include <vector>
#include <cstdlib>

namespace shaderglass {

namespace {

// The catalogue is a shared object, so it has an architecture, and a 32-bit game cannot load a
// 64-bit one. A 32-bit game is not a corner case here -- GoldSrc, the Source engine and a good
// share of what CRT shaders are wanted for are 32-bit -- so the catalogue is named per
// architecture exactly as the layer itself is (decision 10), and each layer looks for its own.
//
// Without this a 32-bit game silently ran the passthrough: the dlopen failed, the chain fell back,
// and "1 pass" in the status looked identical to a working one-pass preset.
#ifdef SHADERGLASS_LAYER_32
const char* kLibrary = "libShaderGlassPresets_32.so";
const char* kOverrideVar = "SHADERGLASS_PRESETS_32";
const char* kCatalogDir = "cm32";
#else
const char* kLibrary = "libShaderGlassPresets.so";
const char* kOverrideVar = "SHADERGLASS_PRESETS";
const char* kCatalogDir = "cm";
#endif

}  // namespace

Catalogue& Catalogue::Instance() {
    static Catalogue instance;
    return instance;
}

// Where this layer's own library sits, which is the anchor for finding everything shipped with it.
// A layer cannot be told its path and has no argv to consult, but it can ask the dynamic linker
// about any address inside itself.
std::string LayerDirectory() {
    Dl_info info {};
    if (!dladdr((const void*) &LayerDirectory, &info) || !info.dli_fname) return std::string();
    const std::string path = info.dli_fname;
    const size_t cut = path.find_last_of('/');
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

// Everywhere the catalogue is looked for, in order, so that running a game needs no configuration.
//
// The anchor is the layer's own directory: an installed tree keeps the catalogue beside the layer,
// and a build tree keeps it a couple of levels up in build/catalog, so both are found without being
// told. After those come the usual installed locations, and last the bare name, which lets the
// dynamic linker's own search finish the job for a packaged build.
//
// This is the difference between one environment variable and four. The override is kept for the
// tests and for pointing a game at a catalogue that is not the installed one.
std::vector<std::string> CandidatePaths() {
    std::vector<std::string> out;
    if (const char* override = getenv(kOverrideVar); override && override[0]) {
        out.emplace_back(override);
        return out;  // asked for by name: do not quietly use a different one
    }

    const std::string dir = LayerDirectory();
    if (!dir.empty()) {
        out.push_back(dir + "/" + kLibrary);                   // installed beside the layer
        out.push_back(dir + "/../presets/" + kLibrary);        // installed, one directory over
        out.push_back(dir + "/../../catalog/" + kCatalogDir + "/" + kLibrary);  // build tree
    }

    const char* home = getenv("HOME");
    if (home && home[0])
        out.push_back(std::string(home) + "/.local/lib/shaderglass/" + kLibrary);
    out.push_back(std::string("/usr/local/lib/shaderglass/") + kLibrary);
    out.push_back(std::string("/usr/lib/shaderglass/") + kLibrary);

    out.emplace_back(kLibrary);  // whatever the dynamic linker's own search turns up
    return out;
}

void Catalogue::Open() {
    if (_tried) return;
    _tried = true;

    const std::vector<std::string> candidates = CandidatePaths();
    std::string path = candidates.front();
    const char* why = nullptr;

    // RTLD_LOCAL so 134 MB of preset symbols do not join the global namespace of a game that has
    // its own idea of what those names mean.
    for (const std::string& candidate : candidates) {
        _handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (_handle) {
            path = candidate;
            break;
        }
        // Kept from the first attempt, which is the one the user most likely meant.
        if (!why) why = dlerror();
        else dlerror();
    }

    if (!_handle) {
        // `why` was taken once, from the first attempt. dlerror() reports the last error and
        // clears it, so calling it in a condition and again for the value returns null the second
        // time -- and std::string from a null pointer is undefined behaviour, which here meant a
        // segfault inside the game.

        // Short, because the header's reason field is 192 bytes and a truncated instruction is
        // worse than none. The variable to set is the part worth the room: which architecture a
        // game is is not something anyone knows off hand -- Chrono Trigger's Steam build is 32-bit,
        // so is Half-Life, while most modern titles are not -- and "cannot open shared object file"
        // does not say that the catalogue beside it, under the other name, is the wrong one here.
        const bool named = getenv(kOverrideVar) != nullptr;
        _reason = std::string("no catalogue for this ") + (sizeof(void*) == 4 ? "32-bit" : "64-bit") +
                  " game";
        // Only mention the variable when it is the thing that failed. Everywhere it looks is
        // searched without configuration now, so naming an environment variable as the remedy when
        // nobody set one sends the reader after a setting they do not need.
        _reason += named ? std::string(" (") + kOverrideVar + " is set, and wrong)"
                         : std::string("; install it, or set ") + kOverrideVar;

        // The dlopen error goes to the log, where there is room for it.
        Log("[catalogue] %s -- %s: %s", _reason.c_str(), path.c_str(),
            why ? why : "dlopen failed");
        return;
    }

    _abiVersion = (uint32_t (*)()) dlsym(_handle, "SgPresetAbiVersion");
    _presetCount = (size_t (*)()) dlsym(_handle, "SgPresetCount");
    _presetAt = (const SgPreset* (*) (size_t)) dlsym(_handle, "SgPresetAt");
    _findPreset = (const SgPreset* (*) (const char*)) dlsym(_handle, "SgFindPreset");

    if (!_abiVersion || !_presetCount || !_presetAt || !_findPreset) {
        _reason = "the preset catalogue is missing one of the four ABI symbols";
        Log("[catalogue] %s", _reason.c_str());
        _presetAt = nullptr;
        return;
    }

    const uint32_t abi = _abiVersion();
    if (abi != SG_PRESET_ABI_VERSION) {
        // Reading a struct laid out differently would be worse than having no presets at all.
        _reason = "the preset catalogue was built against a different ABI";
        Log("[catalogue] %s (catalogue %u, layer %u)", _reason.c_str(), abi,
            (unsigned) SG_PRESET_ABI_VERSION);
        _presetAt = nullptr;
        return;
    }

    Log("[catalogue] opened %s: %zu presets, ABI %u", path.c_str(), _presetCount(), abi);
#ifdef SHADERGLASS_LAYER_32
    Log("[catalogue] (32-bit)");
#endif
    _reason.clear();
}

size_t Catalogue::Count() {
    Open();
    return Usable() ? _presetCount() : 0;
}

const SgPreset* Catalogue::At(size_t index) {
    Open();
    if (!Usable() || index >= _presetCount()) return nullptr;
    return _presetAt(index);
}

const SgPreset* Catalogue::Find(const std::string& id) {
    if (id.empty()) return nullptr;
    Open();
    if (!Usable()) return nullptr;
    return _findPreset(id.c_str());
}

}  // namespace shaderglass
