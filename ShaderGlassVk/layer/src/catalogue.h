/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

The generated preset catalogue, as the layer sees it.

libShaderGlassPresets.so is ~134 MB of SPIR-V and texture bytes. The layer is mapped into every
process that has ShaderGlass armed, so it is not linked: it is dlopened the first time a preset is
actually named, and a game that never selects one never pays for it. Decision 2 in DESIGN.md.

Opening is attempted once per process. A catalogue that is missing, unreadable, or built against a
different ABI leaves the chain with no preset, which presents the game's own frame -- the same
fail-open the rest of the layer uses.
*/

#pragma once

#include "../../presets/preset_api.h"

#include <string>

namespace shaderglass {

class Catalogue {
  public:
    // Resolves the library on first use and remembers the outcome, good or bad. Safe to call every
    // frame.
    static Catalogue& Instance();

    bool Usable() const { return _presetAt != nullptr; }
    const char* Reason() const { return _reason.c_str(); }

    // nullptr when the id is not in the catalogue, or the catalogue could not be opened.
    const SgPreset* Find(const std::string& id);

    // Opens the library if it is not open yet, like Find and At, so any of the three can be the
    // first call made.
    size_t Count();

    // By position, for walking the whole catalogue -- what tests/chain_test.cpp does. Opens the
    // library if it is not open yet, so it can be called first.
    const SgPreset* At(size_t index);

  private:
    Catalogue() = default;
    void Open();

    void* _handle = nullptr;
    bool _tried = false;
    std::string _reason;

    uint32_t (*_abiVersion)() = nullptr;
    size_t (*_presetCount)() = nullptr;
    const SgPreset* (*_presetAt)(size_t) = nullptr;
    const SgPreset* (*_findPreset)(const char*) = nullptr;
};

}  // namespace shaderglass
