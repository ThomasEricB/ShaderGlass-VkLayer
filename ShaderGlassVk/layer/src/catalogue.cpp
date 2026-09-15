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
#include <cstdlib>

namespace shaderglass {

namespace {

const char* kLibrary = "libShaderGlassPresets.so";

}  // namespace

Catalogue& Catalogue::Instance() {
    static Catalogue instance;
    return instance;
}

void Catalogue::Open() {
    if (_tried) return;
    _tried = true;

    // SHADERGLASS_PRESETS names the library outright, which is what the tests and a development
    // tree use; otherwise the normal search runs, so a packaged install is found through the
    // usual loader path without any configuration.
    const char* override = getenv("SHADERGLASS_PRESETS");
    const char* path = (override && override[0]) ? override : kLibrary;

    // RTLD_LOCAL so 134 MB of preset symbols do not join the global namespace of a game that has
    // its own idea of what those names mean.
    _handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!_handle) {
        _reason = std::string("no preset catalogue: ") + (dlerror() ? dlerror() : "dlopen failed");
        Log("[catalogue] %s", _reason.c_str());
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

    Log("[catalogue] opened %s: %zu presets, ABI %u", path, _presetCount(), abi);
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
