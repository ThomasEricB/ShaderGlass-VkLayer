/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Reflection read straight out of the SPIR-V.

This is a deliberately narrow reader, not a general SPIR-V library. It answers exactly the questions
the generator asks -- what is in each uniform block, at what offset, of what type, and which binding
each texture sits at -- and nothing else. That is tractable because SPIR-V states all of it directly:

    OpName / OpMemberName        the names
    OpMemberDecorate ... Offset  the byte offsets
    OpDecorate ... Binding       the descriptor bindings
    OpTypeFloat / Vector / Matrix the types

so there is no inference involved and nothing to get subtly wrong in the way a heuristic would.
Anything it does not recognise is reported rather than guessed at, so a shader outside the shapes
libretro presets use fails loudly instead of producing a mis-sized parameter.
*/

#include "reflect.h"

#include <map>
#include <sstream>
#include <stdexcept>

namespace shaderglass {

namespace {

// The handful of the specification this reader needs.
constexpr uint32_t kMagic = 0x07230203;

constexpr uint16_t OpName = 5;
constexpr uint16_t OpMemberName = 6;
constexpr uint16_t OpTypeInt = 21;
constexpr uint16_t OpTypeFloat = 22;
constexpr uint16_t OpTypeVector = 23;
constexpr uint16_t OpTypeMatrix = 24;
constexpr uint16_t OpTypeImage = 25;
constexpr uint16_t OpTypeSampler = 26;
constexpr uint16_t OpTypeSampledImage = 27;
constexpr uint16_t OpTypeArray = 28;
constexpr uint16_t OpTypeRuntimeArray = 29;
constexpr uint16_t OpTypeStruct = 30;
constexpr uint16_t OpTypePointer = 32;
constexpr uint16_t OpVariable = 59;
constexpr uint16_t OpDecorate = 71;
constexpr uint16_t OpMemberDecorate = 72;

constexpr uint32_t DecorationBlock = 2;
constexpr uint32_t DecorationBufferBlock = 3;
constexpr uint32_t DecorationBinding = 33;
constexpr uint32_t DecorationDescriptorSet = 34;
constexpr uint32_t DecorationOffset = 35;

constexpr uint32_t DecorationLocation = 30;
constexpr uint32_t DecorationComponent = 31;

constexpr uint32_t StorageClassUniformConstant = 0;
constexpr uint32_t StorageClassInput = 1;
constexpr uint32_t StorageClassUniform = 2;
constexpr uint32_t StorageClassOutput = 3;
constexpr uint32_t StorageClassPushConstant = 9;

struct TypeInfo {
    uint16_t op = 0;
    uint32_t width = 0;        // OpTypeInt / OpTypeFloat
    uint32_t sign = 0;         // OpTypeInt
    uint32_t componentType = 0; // OpTypeVector / OpTypeMatrix / OpTypeArray
    uint32_t componentCount = 0;
    uint32_t pointee = 0;       // OpTypePointer
    uint32_t storageClass = 0;  // OpTypePointer
    std::vector<uint32_t> members;  // OpTypeStruct
};

// Read a literal string starting at `at`, which SPIR-V stores as NUL-terminated UTF-8 packed into
// words. Returns the word count consumed.
std::string ReadString(const std::vector<uint32_t>& w, size_t at, size_t limit, size_t* consumed) {
    std::string s;
    size_t i = at;
    bool done = false;
    while (i < limit && !done) {
        const uint32_t word = w[i++];
        for (int b = 0; b < 4; ++b) {
            const char c = char((word >> (8 * b)) & 0xFF);
            if (c == '\0') {
                done = true;
                break;
            }
            s.push_back(c);
        }
    }
    if (consumed) *consumed = i - at;
    return s;
}

// The type names ShaderGC's sizing understands. Anything else comes back spelled out, so the caller
// can say what it saw rather than silently sizing it wrong.
std::string TypeName(const std::map<uint32_t, TypeInfo>& types, uint32_t id) {
    auto it = types.find(id);
    if (it == types.end()) return "unknown";
    const TypeInfo& t = it->second;

    switch (t.op) {
        case OpTypeFloat:
            return t.width == 32 ? "float" : "float" + std::to_string(t.width);
        case OpTypeInt:
            if (t.width != 32) return (t.sign ? "int" : "uint") + std::to_string(t.width);
            return t.sign ? "int" : "uint";
        case OpTypeVector: {
            const std::string base = TypeName(types, t.componentType);
            const std::string prefix = base == "float" ? "vec"
                                       : base == "int" ? "ivec"
                                       : base == "uint" ? "uvec"
                                                        : base + "vec";
            return prefix + std::to_string(t.componentCount);
        }
        case OpTypeMatrix: {
            const std::string col = TypeName(types, t.componentType);
            // Square matrices are spelled matN, which is all libretro blocks contain.
            if (col == "vec" + std::to_string(t.componentCount))
                return "mat" + std::to_string(t.componentCount);
            return col + "x" + std::to_string(t.componentCount);
        }
        case OpTypeArray:
            return TypeName(types, t.componentType) + "[]";
        case OpTypeStruct:
            return "struct";
        default:
            return "unknown";
    }
}

}  // namespace

Reflection ReflectBuiltin(const std::vector<uint32_t>& spirv) {
    if (spirv.size() < 5 || spirv[0] != kMagic)
        throw std::runtime_error("not a SPIR-V module");

    std::map<uint32_t, std::string> names;
    std::map<uint32_t, std::map<uint32_t, std::string>> memberNames;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> memberOffsets;
    std::map<uint32_t, uint32_t> bindings;
    std::map<uint32_t, bool> isBlock;
    std::map<uint32_t, TypeInfo> types;

    struct VariableInfo {
        uint32_t typeId = 0;
        uint32_t storageClass = 0;
    };
    std::map<uint32_t, VariableInfo> variables;
    std::vector<uint32_t> variableOrder;

    size_t i = 5;
    while (i < spirv.size()) {
        const uint32_t first = spirv[i];
        const uint16_t op = uint16_t(first & 0xFFFF);
        const uint16_t count = uint16_t(first >> 16);
        if (count == 0 || i + count > spirv.size())
            throw std::runtime_error("malformed SPIR-V instruction stream");

        const size_t end = i + count;

        switch (op) {
            case OpName:
                if (count >= 3) names[spirv[i + 1]] = ReadString(spirv, i + 2, end, nullptr);
                break;

            case OpMemberName:
                if (count >= 4)
                    memberNames[spirv[i + 1]][spirv[i + 2]] =
                        ReadString(spirv, i + 3, end, nullptr);
                break;

            case OpDecorate:
                if (count >= 3) {
                    const uint32_t target = spirv[i + 1];
                    const uint32_t decoration = spirv[i + 2];
                    if (decoration == DecorationBinding && count >= 4)
                        bindings[target] = spirv[i + 3];
                    else if (decoration == DecorationBlock || decoration == DecorationBufferBlock)
                        isBlock[target] = true;
                    (void) DecorationDescriptorSet;  // read, not needed: set 0 throughout
                }
                break;

            case OpMemberDecorate:
                if (count >= 5 && spirv[i + 3] == DecorationOffset)
                    memberOffsets[spirv[i + 1]][spirv[i + 2]] = spirv[i + 4];
                break;

            case OpTypeInt:
                if (count >= 4) {
                    TypeInfo t;
                    t.op = op;
                    t.width = spirv[i + 2];
                    t.sign = spirv[i + 3];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeFloat:
                if (count >= 3) {
                    TypeInfo t;
                    t.op = op;
                    t.width = spirv[i + 2];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeVector:
            case OpTypeMatrix:
                if (count >= 4) {
                    TypeInfo t;
                    t.op = op;
                    t.componentType = spirv[i + 2];
                    t.componentCount = spirv[i + 3];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeArray:
            case OpTypeRuntimeArray:
                if (count >= 3) {
                    TypeInfo t;
                    t.op = OpTypeArray;
                    t.componentType = spirv[i + 2];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeStruct:
                if (count >= 2) {
                    TypeInfo t;
                    t.op = op;
                    for (size_t m = i + 2; m < end; ++m) t.members.push_back(spirv[m]);
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypePointer:
                if (count >= 4) {
                    TypeInfo t;
                    t.op = op;
                    t.storageClass = spirv[i + 2];
                    t.pointee = spirv[i + 3];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeImage:
            case OpTypeSampler:
                if (count >= 2) {
                    TypeInfo t;
                    t.op = op;
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpTypeSampledImage:
                if (count >= 3) {
                    TypeInfo t;
                    t.op = op;
                    t.componentType = spirv[i + 2];
                    types[spirv[i + 1]] = t;
                }
                break;

            case OpVariable:
                if (count >= 4) {
                    VariableInfo v;
                    v.typeId = spirv[i + 1];
                    v.storageClass = spirv[i + 3];
                    variables[spirv[i + 2]] = v;
                    variableOrder.push_back(spirv[i + 2]);
                }
                break;

            default:
                break;
        }

        i = end;
    }

    Reflection out;
    int pushIndex = -1;

    for (uint32_t id : variableOrder) {
        const VariableInfo& v = variables[id];

        auto pit = types.find(v.typeId);
        if (pit == types.end() || pit->second.op != OpTypePointer) continue;
        const uint32_t pointee = pit->second.pointee;

        // A sampler: a UniformConstant pointing at a sampled image (or an array of them).
        if (v.storageClass == StorageClassUniformConstant) {
            uint32_t target = pointee;
            auto tit = types.find(target);
            if (tit != types.end() && tit->second.op == OpTypeArray) {
                target = tit->second.componentType;
                tit = types.find(target);
            }
            if (tit != types.end() &&
                (tit->second.op == OpTypeSampledImage || tit->second.op == OpTypeImage)) {
                ReflectedTexture tex;
                tex.name = names.count(id) ? names[id] : std::string();
                tex.binding = bindings.count(id) ? int(bindings[id]) : 0;
                if (!tex.name.empty()) out.textures.push_back(tex);
            }
            continue;
        }

        const bool uniform = v.storageClass == StorageClassUniform;
        const bool push = v.storageClass == StorageClassPushConstant;
        if (!uniform && !push) continue;

        auto sit = types.find(pointee);
        if (sit == types.end() || sit->second.op != OpTypeStruct) continue;

        // A Uniform struct is only a uniform block if it is decorated Block; anything else in that
        // storage class is not what we are looking for.
        if (uniform && !isBlock.count(pointee)) continue;

        ReflectedBlock block;
        block.pushConstant = push;
        block.binding = push ? pushIndex-- : (bindings.count(id) ? int(bindings[id]) : 0);

        const auto& memberTypes = sit->second.members;
        for (uint32_t m = 0; m < memberTypes.size(); ++m) {
            ReflectedMember member;
            auto mn = memberNames.find(pointee);
            if (mn != memberNames.end() && mn->second.count(m)) member.name = mn->second.at(m);

            auto mo = memberOffsets.find(pointee);
            if (mo != memberOffsets.end() && mo->second.count(m)) member.offset = mo->second.at(m);

            member.type = TypeName(types, memberTypes[m]);
            block.members.push_back(member);
        }

        out.blocks.push_back(block);
    }

    return out;
}

bool ReflectionsAgree(const Reflection& a, const Reflection& b, std::string* difference) {
    std::ostringstream why;

    if (a.blocks.size() != b.blocks.size()) {
        why << "block count " << a.blocks.size() << " vs " << b.blocks.size();
        if (difference) *difference = why.str();
        return false;
    }
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        const auto& x = a.blocks[i];
        const auto& y = b.blocks[i];
        if (x.binding != y.binding || x.pushConstant != y.pushConstant) {
            why << "block " << i << " binding " << x.binding << " vs " << y.binding;
            if (difference) *difference = why.str();
            return false;
        }
        if (x.members.size() != y.members.size()) {
            why << "block " << i << " member count " << x.members.size() << " vs "
                << y.members.size();
            if (difference) *difference = why.str();
            return false;
        }
        for (size_t m = 0; m < x.members.size(); ++m) {
            if (x.members[m].name != y.members[m].name ||
                x.members[m].offset != y.members[m].offset ||
                x.members[m].type != y.members[m].type) {
                why << "block " << i << " member " << m << ": " << x.members[m].name << "@"
                    << x.members[m].offset << ":" << x.members[m].type << " vs "
                    << y.members[m].name << "@" << y.members[m].offset << ":" << y.members[m].type;
                if (difference) *difference = why.str();
                return false;
            }
        }
    }

    if (a.textures.size() != b.textures.size()) {
        why << "texture count " << a.textures.size() << " vs " << b.textures.size();
        if (difference) *difference = why.str();
        return false;
    }
    for (size_t i = 0; i < a.textures.size(); ++i) {
        if (a.textures[i].name != b.textures[i].name ||
            a.textures[i].binding != b.textures[i].binding) {
            why << "texture " << i << ": " << a.textures[i].name << "@" << a.textures[i].binding
                << " vs " << b.textures[i].name << "@" << b.textures[i].binding;
            if (difference) *difference = why.str();
            return false;
        }
    }

    return true;
}

Reflection Reflect(const std::vector<uint32_t>& spirv) {
#ifdef SG_HAVE_SPIRV_CROSS
    return ReflectSpirvCross(spirv);
#else
    return ReflectBuiltin(spirv);
#endif
}

const char* ReflectBackendName() {
#ifdef SG_HAVE_SPIRV_CROSS
    return "spirv-cross";
#else
    return "built-in";
#endif
}


namespace {

// The user-defined interface of one stage: every variable in `storage` that carries a Location.
// Built-ins (gl_Position, gl_FragCoord) carry BuiltIn instead of Location, so they fall out on
// their own without needing to be named.
std::vector<InterfaceSlot> StageInterface(const std::vector<uint32_t>& spirv, uint32_t storage) {
    if (spirv.size() < 5 || spirv[0] != kMagic) throw std::runtime_error("not a SPIR-V module");

    std::map<uint32_t, uint32_t> locations, components;
    std::map<uint32_t, std::string> names;
    std::map<uint32_t, uint32_t> storageOf;

    size_t at = 5;
    while (at < spirv.size()) {
        const uint32_t word = spirv[at];
        const uint16_t op = uint16_t(word & 0xFFFF);
        const uint16_t count = uint16_t(word >> 16);
        if (count == 0 || at + count > spirv.size()) break;

        if (op == OpDecorate && count >= 4) {
            if (spirv[at + 2] == DecorationLocation) locations[spirv[at + 1]] = spirv[at + 3];
            else if (spirv[at + 2] == DecorationComponent) components[spirv[at + 1]] = spirv[at + 3];
        } else if (op == OpVariable && count >= 4) {
            storageOf[spirv[at + 2]] = spirv[at + 3];
        } else if (op == OpName && count >= 3) {
            size_t consumed = 0;
            names[spirv[at + 1]] = ReadString(spirv, at + 2, at + count, &consumed);
        }
        at += count;
    }

    std::vector<InterfaceSlot> slots;
    for (const auto& kv : storageOf) {
        if (kv.second != storage) continue;
        auto loc = locations.find(kv.first);
        if (loc == locations.end()) continue;

        InterfaceSlot slot;
        slot.location = loc->second;
        auto comp = components.find(kv.first);
        slot.component = comp == components.end() ? 0u : comp->second;
        auto name = names.find(kv.first);
        slot.name = name == names.end() ? std::string() : name->second;
        slots.push_back(std::move(slot));
    }
    return slots;
}

}  // namespace

std::vector<InterfaceSlot> StageInputs(const std::vector<uint32_t>& spirv) {
    return StageInterface(spirv, StorageClassInput);
}

std::vector<InterfaceSlot> StageOutputs(const std::vector<uint32_t>& spirv) {
    return StageInterface(spirv, StorageClassOutput);
}

std::vector<InterfaceSlot> UnmatchedInputs(const std::vector<uint32_t>& vertex,
                                           const std::vector<uint32_t>& fragment) {
    const auto outputs = StageOutputs(vertex);
    std::vector<InterfaceSlot> missing;

    for (const auto& in : StageInputs(fragment)) {
        bool found = false;
        for (const auto& out : outputs) {
            if (out.location == in.location && out.component == in.component) {
                found = true;
                break;
            }
        }
        if (!found) missing.push_back(in);
    }
    return missing;
}

}  // namespace shaderglass
