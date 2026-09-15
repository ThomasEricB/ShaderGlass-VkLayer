/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

What the generator needs to know about a compiled shader.

ShaderGC gets this from SPIRV-Cross's reflection JSON. The surface it actually consumes is small and
fully specified -- for each uniform block, the members' names, byte offsets and types; for each
texture, its name and binding -- so there are two implementations behind this interface:

  reflect_cross.cpp     SPIRV-Cross, when it is installed. Proven against real presets.
  reflect_builtin.cpp   the same facts read straight out of the SPIR-V. Always available, so the
                        generator builds on a machine with nothing but glslang.

Both are built when SPIRV-Cross is present, and `shaderglass-gen --reflect-check` runs them over the
same shader and diffs the result. A fallback nobody exercises is a fallback nobody can trust; this
one is checked against the implementation it stands in for.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shaderglass {

// A member of a uniform block or a push-constant block.
struct ReflectedMember {
    std::string name;
    uint32_t offset = 0;

    // One of: float, int, uint, vec2, vec3, vec4, mat4 -- the set libretro's uniform blocks use and
    // the only ones ShaderGC's sizing understands. Anything else is reported as-is so the caller can
    // fail loudly rather than silently mis-size a parameter.
    std::string type;
};

// A uniform block. `binding` is the descriptor binding for a UBO; push-constant blocks have none and
// are numbered negatively, matching ShaderGC's convention (-1 for the first, then downwards).
struct ReflectedBlock {
    int binding = 0;
    bool pushConstant = false;
    std::vector<ReflectedMember> members;
};

struct ReflectedTexture {
    std::string name;
    int binding = 0;
};

struct Reflection {
    std::vector<ReflectedBlock> blocks;
    std::vector<ReflectedTexture> textures;
};

// Read a compiled SPIR-V module. Throws std::runtime_error on a module it cannot make sense of.
Reflection ReflectBuiltin(const std::vector<uint32_t>& spirv);

#ifdef SG_HAVE_SPIRV_CROSS
Reflection ReflectSpirvCross(const std::vector<uint32_t>& spirv);
#endif

// Whichever is the primary on this build.
Reflection Reflect(const std::vector<uint32_t>& spirv);

// True when the two implementations agree. Used by --reflect-check; always true when there is only
// one implementation to compare.
bool ReflectionsAgree(const Reflection& a, const Reflection& b, std::string* difference);

const char* ReflectBackendName();

// One stage's user-defined interface: the (location, component) slots it declares, and what each is
// called. Fragment inputs must be covered by vertex outputs -- a pair that does not line up is what
// the Vulkan runtime reports as VUID-RuntimeSpirv-OpEntryPoint-08743, and the fragment stage then
// reads undefined values. Fifteen shaders in the libretro tree are written that way, so the
// generator says which rather than leaving it to show up inside somebody's game.
struct InterfaceSlot {
    uint32_t location = 0;
    uint32_t component = 0;
    std::string name;
};

std::vector<InterfaceSlot> StageInputs(const std::vector<uint32_t>& spirv);
std::vector<InterfaceSlot> StageOutputs(const std::vector<uint32_t>& spirv);

// Fragment inputs with no matching vertex output. Empty when the pair lines up.
std::vector<InterfaceSlot> UnmatchedInputs(const std::vector<uint32_t>& vertex,
                                           const std::vector<uint32_t>& fragment);

}  // namespace shaderglass
