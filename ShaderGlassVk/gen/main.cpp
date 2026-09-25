/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

shaderglass-gen -- compiles libretro .slangp presets into the generated C catalogue.

This is ShaderGen's job, done for Vulkan: walk a slang-shaders tree, compile every preset, and emit
one .cpp per preset plus an index, all of which build into libShaderGlassPresets.so. The Windows app
emits DXBC byte arrays; this emits SPIR-V word arrays and the reflection that goes with them.

  shaderglass-gen --out DIR PRESET...        compile the named presets
  shaderglass-gen --out DIR --tree DIR       compile every .slangp under a tree
  shaderglass-gen --reflect-check PRESET...  compile, and diff the two reflection backends
*/

#include "glsl.h"
#include "reflect.h"
#include "shadergc.h"

#include <cstring>
#include <iostream>

using namespace shaderglass;
namespace fs = std::filesystem;

namespace {

// A catalogue id is the preset's path within the tree, without its extension, with '/' separators:
// "crt/crt-geom". It is what the interface stores and what the layer asks for.
std::string MakeId(const fs::path& preset, const fs::path& root) {
    fs::path rel = root.empty() ? preset.filename() : fs::relative(preset, root);
    rel.replace_extension();
    std::string id = rel.generic_string();
    if (id.rfind("./", 0) == 0) id.erase(0, 2);
    return id;
}

std::string MakeCategory(const std::string& id) {
    const auto slash = id.find('/');
    return slash == std::string::npos ? std::string("misc") : id.substr(0, slash);
}

// A C identifier derived from the id, for the generated symbol names.
std::string MakeSymbol(const std::string& id) {
    std::string s = "sg_preset_";
    for (char c : id) s.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return s;
}

// "0f" is not a C++ float literal. Print enough digits to round-trip and make sure what comes out
// always carries a decimal point or an exponent before the suffix is appended.
std::string FloatLiteral(float v) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.9g", double(v));
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s + "f";
}

std::string CEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// How many bytes a word array costs once written as C. Counted rather than estimated, because the
// answer decides whether the catalogue is buildable at all.
size_t WordArrayBytes(const std::vector<uint32_t>& words) {
    size_t n = 0;
    char buf[16];
    for (uint32_t w : words) n += size_t(snprintf(buf, sizeof(buf), "%uu,", w));
    return n + words.size() / 10 * 5;  // the newline and indent every tenth word
}

size_t ByteArrayBytes(const std::vector<uint8_t>& bytes) {
    size_t n = 0;
    char buf[8];
    for (uint8_t b : bytes) n += size_t(snprintf(buf, sizeof(buf), "%u,", b));
    return n + bytes.size() / 16 * 5;
}

void EmitWords(std::ostream& o, const std::string& name, const std::vector<uint32_t>& words) {
    o << "static const uint32_t " << name << "[] = {";
    for (size_t i = 0; i < words.size(); ++i) {
        if (i % 10 == 0) o << "\n    ";
        o << words[i] << "u,";
    }
    o << "\n};\n\n";
}

void EmitBytes(std::ostream& o, const std::string& name, const std::vector<uint8_t>& bytes) {
    o << "static const uint8_t " << name << "[] = {";
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i % 16 == 0) o << "\n    ";
        o << unsigned(bytes[i]) << ",";
    }
    if (bytes.empty()) o << " 0";
    o << "\n};\n\n";
}

void EmitKeyValues(std::ostream& o, const std::string& name,
                   const std::map<std::string, std::string>& kv) {
    if (kv.empty()) return;
    o << "static const SgKeyValue " << name << "[] = {\n";
    for (const auto& p : kv)
        o << "    {\"" << CEscape(p.first) << "\", \"" << CEscape(p.second) << "\"},\n";
    o << "};\n\n";
}

// Each unique shader in its own translation unit: the SPIR-V, what reflection found, and one
// SgShader that every preset using it points at.
void EmitSharedShaders(const fs::path& out, Registry& registry, std::vector<std::string>& files) {
    for (const auto& kv : registry.shaders) {
        const GenShaderData& sh = *kv.second;
        const std::string file = sh.symbol + ".cpp";

        std::ofstream o(out / file);
        o << "// Generated by shaderglass-gen. Do not edit.\n";
        o << "// shader: " << sh.name << "\n\n";
        o << "#include \"preset_api.h\"\n\n";

        EmitWords(o, sh.symbol + "_vert", sh.vertex);
        EmitWords(o, sh.symbol + "_frag", sh.fragment);

        if (!sh.params.empty()) {
            o << "static const SgParam " << sh.symbol << "_params[] = {\n";
            for (const auto& p : sh.params)
                o << "    {\"" << CEscape(p.name) << "\", \"" << CEscape(p.desc) << "\", "
                  << p.buffer << ", " << p.offset << ", " << p.size << ", " << FloatLiteral(p.min)
                  << ", " << FloatLiteral(p.max) << ", " << FloatLiteral(p.def) << ", "
                  << FloatLiteral(p.step) << "},\n";
            o << "};\n\n";
        }

        if (!sh.samplers.empty()) {
            o << "static const SgSampler " << sh.symbol << "_samplers[] = {\n";
            for (const auto& sm : sh.samplers)
                o << "    {\"" << CEscape(sm.name) << "\", " << sm.binding << "},\n";
            o << "};\n\n";
        }

        o << "extern \"C\" const SgShader " << sh.symbol << " = {\n";
        o << "    \"" << CEscape(sh.name) << "\",\n";
        o << "    " << sh.symbol << "_vert, " << sh.vertex.size() << ",\n";
        o << "    " << sh.symbol << "_frag, " << sh.fragment.size() << ",\n";
        o << "    " << (sh.params.empty() ? "0" : (sh.symbol + "_params")) << ", "
          << sh.params.size() << ",\n";
        o << "    " << (sh.samplers.empty() ? "0" : (sh.symbol + "_samplers")) << ", "
          << sh.samplers.size() << ",\n";
        o << "    \"" << CEscape(sh.format) << "\",\n";
        o << "};\n";

        files.push_back(file);
    }
}

// Each unique texture likewise, keyed by content -- so one set of bezel art is emitted once however
// many presets reference it.
void EmitSharedTextures(const fs::path& out, Registry& registry, std::vector<std::string>& files) {
    for (const auto& kv : registry.textures) {
        const GenTextureData& tex = *kv.second;
        const std::string file = tex.symbol + ".cpp";

        std::ofstream o(out / file);
        o << "// Generated by shaderglass-gen. Do not edit.\n\n";
        o << "#include \"preset_api.h\"\n\n";
        EmitBytes(o, tex.symbol + "_bytes", tex.data);
        o << "extern \"C\" const SgTextureData " << tex.symbol << " = {" << tex.symbol << "_bytes, "
          << tex.data.size() << "};\n";

        files.push_back(file);
    }
}

void EmitPreset(std::ostream& o, const GenPreset& preset, const std::string& symbol) {
    o << "// Generated by shaderglass-gen. Do not edit.\n";
    o << "// preset: " << preset.id << "\n\n";
    o << "#include \"preset_api.h\"\n\n";

    // The payloads live in their own translation units; this one only refers to them.
    std::vector<std::string> declared;
    for (const auto& pass : preset.passes) {
        const std::string& sym = pass.shader->symbol;
        if (std::find(declared.begin(), declared.end(), sym) != declared.end()) continue;
        declared.push_back(sym);
        o << "extern \"C\" const SgShader " << sym << ";\n";
    }
    for (const auto& tex : preset.textures) {
        const std::string& sym = tex.data->symbol;
        if (std::find(declared.begin(), declared.end(), sym) != declared.end()) continue;
        declared.push_back(sym);
        o << "extern \"C\" const SgTextureData " << sym << ";\n";
    }
    o << "\n";

    for (size_t p = 0; p < preset.passes.size(); ++p)
        EmitKeyValues(o, symbol + "_p" + std::to_string(p) + "_keys", preset.passes[p].presetParams);
    for (size_t t = 0; t < preset.textures.size(); ++t)
        EmitKeyValues(o, symbol + "_t" + std::to_string(t) + "_keys",
                      preset.textures[t].presetParams);

    o << "static const SgPass " << symbol << "_passes[] = {\n";
    for (size_t p = 0; p < preset.passes.size(); ++p) {
        const auto& pass = preset.passes[p];
        const std::string tag = symbol + "_p" + std::to_string(p);
        o << "    {&" << pass.shader->symbol << ", "
          << (pass.presetParams.empty() ? "0" : (tag + "_keys")) << ", "
          << pass.presetParams.size() << "},\n";
    }
    o << "};\n\n";

    if (!preset.textures.empty()) {
        o << "static const SgTexture " << symbol << "_textures[] = {\n";
        for (size_t t = 0; t < preset.textures.size(); ++t) {
            const auto& tex = preset.textures[t];
            const std::string tag = symbol + "_t" + std::to_string(t);
            o << "    {\"" << CEscape(tex.name) << "\", &" << tex.data->symbol << ", "
              << (tex.presetParams.empty() ? "0" : (tag + "_keys")) << ", "
              << tex.presetParams.size() << "},\n";
        }
        o << "};\n\n";
    }

    if (!preset.overrides.empty()) {
        o << "static const SgOverride " << symbol << "_overrides[] = {\n";
        for (const auto& ov : preset.overrides)
            o << "    {\"" << CEscape(ov.name) << "\", " << FloatLiteral(ov.value) << "},\n";
        o << "};\n\n";
    }

    o << "extern \"C\" const SgPreset " << symbol << " = {\n";
    o << "    \"" << CEscape(preset.id) << "\",\n";
    o << "    \"" << CEscape(preset.name) << "\",\n";
    o << "    \"" << CEscape(preset.category) << "\",\n";
    o << "    " << symbol << "_passes, " << preset.passes.size() << ",\n";
    o << "    " << (preset.textures.empty() ? "0" : (symbol + "_textures")) << ", "
      << preset.textures.size() << ",\n";
    o << "    " << (preset.overrides.empty() ? "0" : (symbol + "_overrides")) << ", "
      << preset.overrides.size() << ",\n";
    o << "};\n";
}

void EmitIndex(std::ostream& o, const std::vector<std::string>& symbols) {
    o << "// Generated by shaderglass-gen. Do not edit.\n\n";
    o << "#include \"preset_api.h\"\n\n";
    o << "#include <string.h>\n\n";

    for (const auto& s : symbols) o << "extern \"C\" const SgPreset " << s << ";\n";

    o << "\nstatic const SgPreset* const kPresets[] = {\n";
    for (const auto& s : symbols) o << "    &" << s << ",\n";
    if (symbols.empty()) o << "    0,\n";
    o << "};\n\n";

    o << "static const size_t kCount = " << symbols.size() << ";\n\n";
    o << "extern \"C\" uint32_t SgPresetAbiVersion(void) { return SG_PRESET_ABI_VERSION; }\n\n";
    o << "extern \"C\" size_t SgPresetCount(void) { return kCount; }\n\n";
    o << "extern \"C\" const SgPreset* SgPresetAt(size_t index) {\n";
    o << "    return index < kCount ? kPresets[index] : 0;\n";
    o << "}\n\n";
    o << "extern \"C\" const SgPreset* SgFindPreset(const char* id) {\n";
    o << "    if (!id) return 0;\n";
    o << "    for (size_t i = 0; i < kCount; ++i) {\n";
    o << "        if (strcmp(kPresets[i]->id, id) == 0) return kPresets[i];\n";
    o << "    }\n";
    o << "    return 0;\n";
    o << "}\n";
}

// The generated tree builds on its own, so the catalogue can be regenerated and rebuilt without
// touching the rest of the project.
void EmitBuildFiles(const fs::path& out, const std::vector<std::string>& files) {
    {
        std::ofstream m(out / "meson.build");
        m << "# Generated by shaderglass-gen. Do not edit.\n";
        m << "#\n# A fragment, included with subdir(). Meson allows project() only in the top\n";
        m << "# level, so unlike the CMake file beside it this one is not standalone.\n\n";
        m << "preset_sources = files(\n";
        for (const auto& f : files) m << "  '" << f << "',\n";
        m << "  'sg_index.cpp',\n";
        m << ")\n\n";
        m << "shared_library('ShaderGlassPresets', preset_sources,\n";
        m << "  name_prefix: 'lib',\n";
        m << "  include_directories: include_directories('.'),\n";
        m << "  cpp_args: ['-fPIC'],\n";
        m << "  install: false,\n";
        m << ")\n";
    }
    {
        std::ofstream c(out / "CMakeLists.txt");
        c << "# Generated by shaderglass-gen. Do not edit.\n";
        c << "#\n# Builds either standalone or via add_subdirectory().\n\n";
        c << "cmake_minimum_required(VERSION 3.16)\n";
        c << "if(NOT DEFINED PROJECT_NAME)\n";
        c << "  project(ShaderGlassPresets CXX)\n";
        c << "endif()\n\n";
        c << "add_library(ShaderGlassPresets SHARED\n";
        for (const auto& f : files) c << "  " << f << "\n";
        c << "  sg_index.cpp\n";
        c << ")\n";
        c << "target_include_directories(ShaderGlassPresets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})\n";
        c << "set_target_properties(ShaderGlassPresets PROPERTIES PREFIX \"lib\")\n";
        // Loaded into games, like the layer, and linked like it: a game's own libstdc++ may be
        // older than the one this was built against -- Steam's runtime ships its own -- and a
        // 32-bit install should not need a 32-bit libstdc++ just to read a table of shaders.
        c << "target_link_options(ShaderGlassPresets PRIVATE -static-libstdc++ -static-libgcc)\n\n";

        // A shared object has an architecture, and a 32-bit game cannot load a 64-bit catalogue.
        // The two are named apart for the same reason the layers are, so both can sit in one
        // directory and each layer finds its own.
        c << "# -DSG_PRESETS_32=ON builds the catalogue a 32-bit game needs, under the name its\n";
        c << "# layer looks for. The generated sources are the same either way.\n";
        c << "option(SG_PRESETS_32 \"Build the 32-bit catalogue\" OFF)\n";
        c << "if(SG_PRESETS_32)\n";
        c << "  set_target_properties(ShaderGlassPresets PROPERTIES OUTPUT_NAME "
             "\"ShaderGlassPresets_32\")\n";
        c << "  target_compile_options(ShaderGlassPresets PRIVATE -m32)\n";
        c << "  target_link_options(ShaderGlassPresets PRIVATE -m32)\n";
        c << "endif()\n";
    }
}

int Usage() {
    std::cerr << "usage: shaderglass-gen --out DIR [--tree DIR] [PRESET...]\n"
                 "       shaderglass-gen --reflect-check PRESET...\n"
                 "\n"
                 "  --out DIR          where the generated catalogue is written\n"
                 "  --tree DIR         compile every .slangp under DIR\n"
                 "  --reflect-check    compile, and diff the two reflection backends\n";
    return 2;
}

}  // namespace

// Every uniform member that is neither a semantic the runtime fills in nor a parameter the shader
// declared. Each reads zero. Most are shaders that simply forgot a #pragma parameter, but a
// semantic this layer has not implemented looks exactly the same from here -- and that is how
// OriginalFPS stayed missing long enough to turn a whole preset family black. Worth a glance
// whenever a preset misbehaves for no visible reason.
void ReportUndeclaredUniforms(const Registry& registry) {
    if (registry.undeclaredUniforms.empty()) return;

    std::cout << "\n" << registry.undeclaredUniforms.size()
              << " uniform members are neither a semantic nor a declared parameter"
                 " (each reads zero):\n ";
    size_t on = 0;
    for (const auto& name : registry.undeclaredUniforms) {
        if (on && on % 6 == 0) std::cout << "\n ";
        std::cout << " " << name;
        ++on;
    }
    std::cout << "\n";
}

int main(int argc, char** argv) {
    fs::path out;
    fs::path tree;
    bool reflectCheck = false;
    bool measure = false;
    std::vector<fs::path> presets;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--tree" && i + 1 < argc) tree = argv[++i];
        else if (a == "--reflect-check") reflectCheck = true;
        else if (a == "--measure") measure = true;
        else if (a == "--help" || a == "-h") return Usage();
        else if (a.rfind("-", 0) == 0) return Usage();
        else presets.push_back(a);
    }

    if (!tree.empty()) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 tree, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() == ".slangp") presets.push_back(it->path());
        }
        std::sort(presets.begin(), presets.end());
    }

    if (presets.empty()) return Usage();
    if (out.empty() && !reflectCheck && !measure) return Usage();

    GlslangInit();

    // One registry across the whole run: a shader or texture shared between presets is compiled,
    // hashed and emitted once.
    Registry registry;

    std::vector<std::pair<std::string, GenPreset>> compiledPresets;
    size_t compiled = 0, failed = 0, mismatched = 0, fragments = 0;

    // What the first attempt cost -- every pass and every texture written out per preset -- against
    // what sharing costs. Both counted exactly.
    size_t inlineBytes = 0, sharedShaderBytes = 0, sharedTextureBytes = 0, presetBytes = 0;
    size_t totalPasses = 0, totalTextureRefs = 0;

    if (!out.empty()) fs::create_directories(out);

    for (const auto& path : presets) {
        const std::string id = MakeId(path, tree);
        bool warn = false;
        try {
            GenPreset preset = CompilePreset(path, registry, std::cerr, warn);
            preset.id = id;
            preset.category = MakeCategory(id);

            if (reflectCheck) {
                for (const auto& pass : preset.passes) {
#ifdef SG_HAVE_SPIRV_CROSS
                    std::string why;
                    const Reflection a = ReflectSpirvCross(pass.shader->fragment);
                    const Reflection b = ReflectBuiltin(pass.shader->fragment);
                    if (!ReflectionsAgree(a, b, &why)) {
                        std::cerr << "MISMATCH " << id << " (" << pass.shader->name << "): " << why
                                  << "\n";
                        ++mismatched;
                    }
#else
                    (void) ReflectBuiltin(pass.shader->fragment);
#endif
                }
            }

            for (const auto& pass : preset.passes) {
                ++totalPasses;
                inlineBytes += WordArrayBytes(pass.shader->vertex) +
                               WordArrayBytes(pass.shader->fragment);
            }
            for (const auto& tex : preset.textures) {
                ++totalTextureRefs;
                inlineBytes += ByteArrayBytes(tex.data->data);
            }
            // Roughly what a preset's own tables cost once the payloads are shared out.
            presetBytes += 512 + preset.passes.size() * 220 + preset.textures.size() * 120 +
                           preset.overrides.size() * 60;

            ++compiled;
            if (!measure) {
                std::cout << (warn ? "warn  " : "ok    ") << id << "  (" << preset.passes.size()
                          << " pass" << (preset.passes.size() == 1 ? "" : "es");
                if (!preset.textures.empty())
                    std::cout << ", " << preset.textures.size() << " texture"
                              << (preset.textures.size() == 1 ? "" : "s");
                std::cout << ")\n";
            } else if (compiled % 250 == 0) {
                std::cout << "  " << compiled << " / " << presets.size() << " ...\n" << std::flush;
            }

            if (!out.empty()) compiledPresets.emplace_back(id, std::move(preset));
        } catch (const fragment_error&) {
            // A parameter fragment, not a preset. Skipped, not failed.
            ++fragments;
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL  " << id << ": " << e.what() << "\n";
        }
    }

    for (const auto& kv : registry.shaders)
        sharedShaderBytes += WordArrayBytes(kv.second->vertex) + WordArrayBytes(kv.second->fragment);
    for (const auto& kv : registry.textures)
        sharedTextureBytes += ByteArrayBytes(kv.second->data);

    GlslangShutdown();

    const auto mib = [](size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };

    if (measure) {
        std::cout << "\n";
        std::cout << "presets compiled      " << compiled << "  (" << failed << " failed, "
                  << fragments << " parameter fragments skipped)\n";
        std::cout << "pass references       " << totalPasses << "\n";
        std::cout << "unique shaders        " << registry.shaders.size() << "  (of "
                  << registry.shaderRequests << " requested)\n";
        std::cout << "texture references    " << totalTextureRefs << "\n";
        std::cout << "unique textures       " << registry.textures.size() << "  (of "
                  << registry.textureRequests << " requested)\n";
        std::cout << "\n";
        std::cout << "generated C, everything inline per preset\n";
        std::cout << "  payloads            " << mib(inlineBytes) << " MiB\n";
        std::cout << "\n";
        std::cout << "generated C, shared by content\n";
        std::cout << "  shader SPIR-V       " << mib(sharedShaderBytes) << " MiB\n";
        std::cout << "  texture bytes       " << mib(sharedTextureBytes) << " MiB\n";
        std::cout << "  preset tables       " << mib(presetBytes) << " MiB\n";
        std::cout << "  total               "
                  << mib(sharedShaderBytes + sharedTextureBytes + presetBytes) << " MiB\n";
        if (inlineBytes)
            std::cout << "\n  sharing saves       "
                      << (100.0 - 100.0 * double(sharedShaderBytes + sharedTextureBytes +
                                                 presetBytes) /
                                      double(inlineBytes))
                      << " %\n";
        ReportUndeclaredUniforms(registry);
        return failed ? 1 : 0;
    }

    if (!out.empty()) {
        std::vector<std::string> files;
        std::vector<std::string> symbols;

        // Each unique shader and texture is emitted once, in its own translation unit.
        for (auto& kv : registry.shaders) {
            kv.second->symbol = MakeSymbol("shader_" + std::to_string(std::hash<std::string>{}(
                                                           kv.first)));
        }
        for (auto& kv : registry.textures) kv.second->symbol = MakeSymbol("texture_" + kv.first);

        EmitSharedShaders(out, registry, files);
        EmitSharedTextures(out, registry, files);

        for (const auto& entry : compiledPresets) {
            const std::string symbol = MakeSymbol(entry.first);
            const std::string file = symbol + ".cpp";
            std::ofstream o(out / file);
            if (!o.good()) {
                std::cerr << "cannot write " << (out / file).string() << "\n";
                ++failed;
                continue;
            }
            EmitPreset(o, entry.second, symbol);
            symbols.push_back(symbol);
            files.push_back(file);
        }

        {
            std::ofstream index(out / "sg_index.cpp");
            EmitIndex(index, symbols);
        }
#ifdef SG_PRESET_API_H_PATH
        std::error_code ec;
        fs::copy_file(SG_PRESET_API_H_PATH, out / "preset_api.h",
                      fs::copy_options::overwrite_existing, ec);
        if (ec)
            std::cerr << "note: could not copy preset_api.h into " << out << " (" << ec.message()
                      << ")\n";
#endif
        EmitBuildFiles(out, files);

        std::cout << "\nemitted " << registry.shaders.size() << " shaders, "
                  << registry.textures.size() << " textures, " << symbols.size() << " presets\n";
    }

    std::cout << "\n" << compiled << " compiled, " << failed << " failed";
    if (reflectCheck) std::cout << ", " << mismatched << " reflection mismatches";
    std::cout << " (reflection backend: " << ReflectBackendName() << ")\n";

    ReportUndeclaredUniforms(registry);

    return failed || mismatched ? 1 : 0;
}
