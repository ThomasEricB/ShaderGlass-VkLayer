/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Ported from ShaderGC's framework.h/SourceDefs.h. The string helpers are unchanged; what is gone is
the Windows-only apparatus around them -- the precompiled header, WIN32_LEAN_AND_MEAN, and the
__declspec(noinline) markers that existed to stop MSVC folding identical generated functions.
*/

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace shaderglass {

inline void ltrim(std::string& s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch) && ch != '\"';
            }));
}

inline void rtrim(std::string& s) {
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char ch) { return !std::isspace(ch) && ch != '\"'; })
                .base(),
            s.end());
}

inline std::string trim(std::string s) {
    ltrim(s);
    rtrim(s);
    return s;
}

// Preset and shader comments carry all sorts of encodings; anything outside ASCII is dropped rather
// than passed through into generated C.
inline std::string ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 128) out.push_back(c);
    }
    return out;
}

class file_error : public std::runtime_error {
  public:
    explicit file_error(const std::string& what) : std::runtime_error(what) {}
};

// Not every .slangp in a shader tree is a preset. Some are parameter fragments -- a list of
// key/value overrides with no "shaders" line -- that exist only to be pulled in by #reference.
// Walking a tree should pass over those, not report them as broken presets.
class fragment_error : public std::runtime_error {
  public:
    explicit fragment_error(const std::string& what) : std::runtime_error(what) {}
};

}  // namespace shaderglass
