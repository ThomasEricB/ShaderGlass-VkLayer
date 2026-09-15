/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Reflection through SPIRV-Cross -- the backend ShaderGC has always used, and the primary here when the
library is installed.

Two differences from ShaderGC's version, both deliberate:

  - It queries SPIRV-Cross's C++ API directly rather than asking CompilerReflection for JSON and
    parsing that. Same facts, one fewer dependency: the JSON reflection path is what dragged
    nlohmann/json into ShaderGC, and nothing else here needs it.

  - It produces the same Reflection struct as the built-in reader, so `shaderglass-gen
    --reflect-check` can run both over a preset and diff them.

NOTE: this file is only compiled when SPIRV-Cross is found. It has not been exercised on a machine
without the library installed -- if you have it, build and run `shaderglass-gen --reflect-check`
over a shader tree, which is exactly what that mode exists for.
*/

#include "reflect.h"

#include <spirv_cross/spirv_cross.hpp>

#include <stdexcept>

namespace shaderglass {

namespace {

// Spell a SPIR-V type the way the built-in reader and ShaderGC's sizing both expect.
std::string TypeName(const spirv_cross::SPIRType& t) {
    std::string base;
    switch (t.basetype) {
        case spirv_cross::SPIRType::Float: base = "float"; break;
        case spirv_cross::SPIRType::Int: base = "int"; break;
        case spirv_cross::SPIRType::UInt: base = "uint"; break;
        case spirv_cross::SPIRType::Boolean: base = "bool"; break;
        case spirv_cross::SPIRType::Struct: return "struct";
        default: return "unknown";
    }

    if (t.columns > 1) {
        // Square matrices are spelled matN, which is all a libretro uniform block holds.
        if (t.columns == t.vecsize) return "mat" + std::to_string(t.columns);
        return base + std::to_string(t.vecsize) + "x" + std::to_string(t.columns);
    }

    if (t.vecsize > 1) {
        const std::string prefix = base == "float"  ? "vec"
                                   : base == "int"  ? "ivec"
                                   : base == "uint" ? "uvec"
                                                    : base + "vec";
        return prefix + std::to_string(t.vecsize);
    }

    return base;
}

void AddBlock(Reflection& out, spirv_cross::Compiler& comp,
              const spirv_cross::Resource& resource, bool pushConstant, int binding) {
    const spirv_cross::SPIRType& type = comp.get_type(resource.base_type_id);

    ReflectedBlock block;
    block.pushConstant = pushConstant;
    block.binding = binding;

    for (uint32_t i = 0; i < uint32_t(type.member_types.size()); ++i) {
        ReflectedMember member;
        member.name = comp.get_member_name(resource.base_type_id, i);
        member.offset = comp.type_struct_member_offset(type, i);
        member.type = TypeName(comp.get_type(type.member_types[i]));
        block.members.push_back(member);
    }

    out.blocks.push_back(block);
}

}  // namespace

Reflection ReflectSpirvCross(const std::vector<uint32_t>& spirv) {
    try {
        spirv_cross::Compiler comp(spirv);
        spirv_cross::ShaderResources res = comp.get_shader_resources();

        Reflection out;

        for (const auto& ubo : res.uniform_buffers)
            AddBlock(out, comp, ubo, false,
                     int(comp.get_decoration(ubo.id, spv::DecorationBinding)));

        // Push-constant blocks have no binding, so they are numbered downwards from -1, matching
        // ShaderGC's convention and the built-in reader.
        int pushIndex = -1;
        for (const auto& pc : res.push_constant_buffers)
            AddBlock(out, comp, pc, true, pushIndex--);

        for (const auto& image : res.sampled_images) {
            ReflectedTexture tex;
            tex.name = image.name;
            tex.binding = int(comp.get_decoration(image.id, spv::DecorationBinding));
            out.textures.push_back(tex);
        }
        for (const auto& image : res.separate_images) {
            ReflectedTexture tex;
            tex.name = image.name;
            tex.binding = int(comp.get_decoration(image.id, spv::DecorationBinding));
            out.textures.push_back(tex);
        }

        return out;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("SPIRV-Cross reflection failed: ") + e.what());
    }
}

}  // namespace shaderglass
