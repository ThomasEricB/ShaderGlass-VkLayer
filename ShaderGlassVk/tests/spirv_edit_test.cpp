/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

StripRelaxedPrecision, on modules built by hand.

Over the real catalogue this function is well covered -- 1166 shader pairs go through it and only
the three that should change do -- but real modules are all well-formed. What is not covered there
is what it does with a truncated instruction or a zero word, and those are the cases where an edit
that walks instruction lengths can run off the end.
*/

#include "../gen/spirv_edit.h"

#include <cstdio>
#include <vector>

using namespace shaderglass;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

constexpr uint32_t kMagic = 0x07230203;

uint32_t Instr(uint16_t op, uint16_t words) { return uint32_t(op) | (uint32_t(words) << 16); }

std::vector<uint32_t> Header() { return {kMagic, 0x00010000, 8, 100, 0}; }

}  // namespace

int main() {
    // A module with two RelaxedPrecision decorations among others, and one member decoration.
    {
        std::vector<uint32_t> m = Header();
        m.push_back(Instr(71, 3)); m.push_back(10); m.push_back(0);   // OpDecorate %10 RelaxedPrecision
        m.push_back(Instr(71, 4)); m.push_back(11); m.push_back(30); m.push_back(2);  // Location 2
        m.push_back(Instr(72, 5)); m.push_back(12); m.push_back(0); m.push_back(0); m.push_back(0);
        m.push_back(Instr(71, 3)); m.push_back(13); m.push_back(0);   // OpDecorate %13 RelaxedPrecision
        m.push_back(Instr(59, 4)); m.push_back(20); m.push_back(21); m.push_back(3);  // OpVariable

        const size_t before = m.size();
        const size_t removed = StripRelaxedPrecision(m);

        Check(removed == 3, "three RelaxedPrecision decorations removed");
        // 3 + 4 + 3 words of decoration gone... the member decoration is 5 words.
        Check(m.size() == before - (3 + 5 + 3), "exactly those words removed");

        // The header survives untouched, including the id-bound: removing decorations does not
        // renumber anything.
        Check(m.size() >= 5 && m[0] == kMagic && m[3] == 100, "header and id-bound untouched");

        // The Location decoration and the OpVariable are still there.
        bool sawLocation = false, sawVariable = false, sawRelaxed = false;
        size_t at = 5;
        while (at < m.size()) {
            const uint16_t op = uint16_t(m[at] & 0xFFFF);
            const uint16_t count = uint16_t(m[at] >> 16);
            if (!count) break;
            if (op == 71 && count >= 3 && m[at + 2] == 30) sawLocation = true;
            if (op == 71 && count >= 3 && m[at + 2] == 0) sawRelaxed = true;
            if (op == 72 && count >= 4 && m[at + 3] == 0) sawRelaxed = true;
            if (op == 59) sawVariable = true;
            at += count;
        }
        Check(sawLocation, "the Location decoration survives");
        Check(sawVariable, "the OpVariable survives");
        Check(!sawRelaxed, "no RelaxedPrecision decoration remains");
    }

    // Nothing to do: a module with no RelaxedPrecision is returned unchanged.
    {
        std::vector<uint32_t> m = Header();
        m.push_back(Instr(71, 4)); m.push_back(11); m.push_back(30); m.push_back(2);
        const std::vector<uint32_t> before = m;
        Check(StripRelaxedPrecision(m) == 0, "a clean module reports nothing removed");
        Check(m == before, "...and is byte-for-byte unchanged");
    }

    // Not SPIR-V, and too short to be: left alone rather than walked.
    {
        std::vector<uint32_t> m {1, 2, 3};
        Check(StripRelaxedPrecision(m) == 0, "a module too short to have a header is left alone");
        std::vector<uint32_t> wrong = Header();
        wrong[0] = 0xDEADBEEF;
        const std::vector<uint32_t> before = wrong;
        Check(StripRelaxedPrecision(wrong) == 0, "a wrong magic number is left alone");
        Check(wrong == before, "...and unchanged");
    }

    // A zero word would not advance the walk, and a length past the end would read off it. Both
    // stop the edit and keep the rest verbatim.
    {
        std::vector<uint32_t> m = Header();
        m.push_back(Instr(71, 3)); m.push_back(10); m.push_back(0);  // one real removal first
        m.push_back(0);                                              // then a zero word
        m.push_back(0xAAAA);
        const size_t removed = StripRelaxedPrecision(m);
        Check(removed == 1, "the decoration before a zero word is still removed");
        Check(m.back() == 0xAAAA, "everything after the zero word is kept verbatim");

        std::vector<uint32_t> t = Header();
        t.push_back(Instr(71, 9));  // claims nine words, supplies two
        t.push_back(10);
        const std::vector<uint32_t> before = t;
        Check(StripRelaxedPrecision(t) == 0, "a truncated instruction removes nothing");
        Check(t == before, "...and leaves the module as it was");
    }

    if (g_failures) {
        std::printf("\n%d check%s failed\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("spirv_edit: all checks passed\n");
    return 0;
}
