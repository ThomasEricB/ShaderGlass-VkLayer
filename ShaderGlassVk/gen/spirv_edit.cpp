/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "spirv_edit.h"

namespace shaderglass {

namespace {

constexpr uint32_t kMagic = 0x07230203;
constexpr uint16_t OpDecorate = 71;
constexpr uint16_t OpMemberDecorate = 72;
constexpr uint32_t DecorationRelaxedPrecision = 0;

}  // namespace

size_t StripRelaxedPrecision(std::vector<uint32_t>& spirv) {
    if (spirv.size() < 5 || spirv[0] != kMagic) return 0;

    std::vector<uint32_t> out;
    out.reserve(spirv.size());
    out.insert(out.end(), spirv.begin(), spirv.begin() + 5);  // the header, unchanged

    size_t removed = 0;
    size_t at = 5;
    while (at < spirv.size()) {
        const uint32_t word = spirv[at];
        const uint16_t op = uint16_t(word & 0xFFFF);
        const uint16_t count = uint16_t(word >> 16);

        // A zero-length instruction would not advance; anything past the end is not ours to edit.
        if (count == 0 || at + count > spirv.size()) {
            out.insert(out.end(), spirv.begin() + at, spirv.end());
            break;
        }

        // OpDecorate <target> <decoration>, OpMemberDecorate <struct> <member> <decoration>.
        const bool drop =
            (op == OpDecorate && count >= 3 && spirv[at + 2] == DecorationRelaxedPrecision) ||
            (op == OpMemberDecorate && count >= 4 && spirv[at + 3] == DecorationRelaxedPrecision);

        if (drop)
            ++removed;
        else
            out.insert(out.end(), spirv.begin() + at, spirv.begin() + at + count);

        at += count;
    }

    if (removed) spirv.swap(out);
    return removed;
}

}  // namespace shaderglass
