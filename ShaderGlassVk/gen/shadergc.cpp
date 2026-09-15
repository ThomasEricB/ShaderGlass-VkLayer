/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Ported from ShaderGC (mausimus). See shadergc.h for what was kept and what was dropped.
*/

#include "shadergc.h"

#include "glsl.h"
#include "reflect.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace shaderglass {

namespace {

// The sizes ShaderGC understands, by the type names reflection reports. A type outside this set is
// refused rather than guessed at: a wrong size here silently corrupts every parameter after it in
// the block.
int SizeOfType(const std::string& type) {
    if (type == "float" || type == "uint" || type == "int") return 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2") return 8;
    if (type == "vec3" || type == "ivec3" || type == "uvec3") return 12;
    if (type == "vec4" || type == "ivec4" || type == "uvec4") return 16;
    if (type == "mat4") return 64;
    if (type == "mat3") return 48;
    if (type == "mat2") return 16;
    throw std::runtime_error("unsupported uniform member type: " + type);
}

// Match what reflection found against what the shader declared, and fill in the offsets. A member
// with no declaration is a built-in (MVP, SourceSize, FrameCount and friends) and is carried anyway,
// because the runtime has to know where to write it.
void AddParams(std::vector<GenParam>& actual, const std::vector<GenParam>& declared,
               const ReflectedBlock& block) {
    for (const auto& member : block.members) {
        bool found = false;
        int declIndex = 0;
        for (const auto& d : declared) {
            if (d.name == member.name) {
                GenParam p(d);
                p.declIndex = declIndex;
                p.buffer = block.binding;
                p.offset = int(member.offset);
                p.size = SizeOfType(member.type);
                actual.push_back(p);
                found = true;
            }
            ++declIndex;
        }
        if (!found) {
            GenParam p;
            p.name = member.name;
            p.buffer = block.binding;
            p.offset = int(member.offset);
            p.size = SizeOfType(member.type);
            p.declIndex = 0;
            actual.push_back(p);
        }
    }
}

std::vector<GenParam> LookupParams(const std::vector<GenParam>& declared,
                                   std::vector<GenSampler>& textures, const Reflection& r) {
    std::vector<GenParam> actual;
    for (const auto& block : r.blocks) AddParams(actual, declared, block);
    for (const auto& t : r.textures) textures.push_back(GenSampler {t.name, t.binding});

    std::stable_sort(actual.begin(), actual.end(),
                     [](const GenParam& a, const GenParam& b) { return a.declIndex < b.declIndex; });
    return actual;
}

std::pair<std::string, std::string> GetKeyValue(const std::string& input) {
    auto key = trim(input.substr(0, input.find('=')));
    auto value = trim(input.substr(input.find('=') + 1));
    // Strip a trailing comment, in either of the two forms presets use. trim() has already taken
    // the quotes off a quoted value, so a remaining quote is the one that closed it.
    if (value.find('\"') != std::string::npos) value = value.substr(0, value.find('\"'));

    // A "//" comment only counts when whitespace precedes it: presets write
    //   filter_linear0 = true // required: sample lut-output linear
    // but they also write shader0 = "../..//edge-smoothing/nnedi3/shaders/rgb-to-yuv.slang",
    // where the "//" is a path separator. Cutting at every "//" truncates that path to "../..",
    // which then resolves to a directory and reaches glslang as an empty shader.
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '/' && value[i + 1] == '/' &&
            std::isspace(static_cast<unsigned char>(value[i - 1]))) {
            value = value.substr(0, i);
            break;
        }
    }
    return {key, trim(value)};
}

std::string GetValue(const std::string& key, int shaderNo,
                     const std::map<std::string, std::string>& keyValues,
                     std::unordered_set<std::string>& seen) {
    std::string full = key;
    if (shaderNo >= 0) full += std::to_string(shaderNo);
    auto it = keyValues.find(full);
    if (it == keyValues.end()) return {};
    seen.insert(full);
    return it->second;
}

std::string GetValue(const std::string& key, const std::string& suffix,
                     const std::map<std::string, std::string>& keyValues,
                     std::unordered_set<std::string>& seen) {
    const std::string full = key + suffix;
    auto it = keyValues.find(full);
    if (it == keyValues.end()) return {};
    seen.insert(full);
    return it->second;
}

std::filesystem::path GetKeyPath(const std::string& key, int shaderNo,
                                 const std::map<std::string, std::filesystem::path>& keyPaths) {
    std::string full = key;
    if (shaderNo >= 0) full += std::to_string(shaderNo);
    auto it = keyPaths.find(full);
    return it == keyPaths.end() ? std::filesystem::path() : it->second;
}

std::filesystem::path GetKeyPath(const std::string& key, const std::string& suffix,
                                 const std::map<std::string, std::filesystem::path>& keyPaths) {
    auto it = keyPaths.find(key + suffix);
    return it == keyPaths.end() ? std::filesystem::path() : it->second;
}

// A good number of upstream libretro files spell their relative paths the Windows way -- shader0 =
// "..\..\shaders\menus\menu-hdr.slang", #include "include\hdr10.h". RetroArch accepts either
// separator, so the catalogue has to as well: on Linux a backslash is an ordinary filename
// character, so these have to be translated rather than handed to std::filesystem as they are.
std::filesystem::path RelativePath(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return std::filesystem::path(text);
}

// A preset's paths are relative to the file that declared them, which is not always the preset being
// compiled: #reference pulls keys in from another file, and those resolve against *its* directory.
std::filesystem::path GetPath(const std::string& key, int shaderNo,
                              const std::map<std::string, std::string>& keyValues,
                              const std::map<std::string, std::filesystem::path>& keyPaths,
                              std::unordered_set<std::string>& seen) {
    return GetKeyPath(key, shaderNo, keyPaths) /
           RelativePath(GetValue(key, shaderNo, keyValues, seen));
}

std::filesystem::path GetPath(const std::string& key, const std::string& suffix,
                              const std::map<std::string, std::string>& keyValues,
                              const std::map<std::string, std::filesystem::path>& keyPaths,
                              std::unordered_set<std::string>& seen) {
    return GetKeyPath(key, suffix, keyPaths) /
           RelativePath(GetValue(key, suffix, keyValues, seen));
}

void SetPassParam(const std::string& name, GenPass& pass, int i,
                  const std::map<std::string, std::string>& keyValues,
                  std::unordered_set<std::string>& seen) {
    const auto value = GetValue(name, i, keyValues, seen);
    if (!value.empty()) pass.presetParams.emplace(name, value);
}

// Every per-pass key a preset may set. Consumed by the runtime in phase 4.
void SetPassParams(GenPass& pass, int i, const std::map<std::string, std::string>& keyValues,
                   std::unordered_set<std::string>& seen) {
    for (const char* key : {"filter_linear", "float_framebuffer", "srgb_framebuffer", "scale_type",
                            "scale", "scale_type_x", "scale_x", "scale_type_y", "scale_y", "alias",
                            "mipmap_input", "frame_count_mod", "wrap_mode"})
        SetPassParam(key, pass, i, keyValues, seen);
}

void SetTextureParams(GenTexture& tex, const std::string& name,
                      const std::map<std::string, std::string>& keyValues,
                      std::unordered_set<std::string>& seen) {
    for (const char* suffix : {"linear", "wrap_mode", "mipmap"}) {
        const auto value = GetValue(name + "_", suffix, keyValues, seen);
        if (!value.empty()) tex.presetParams.emplace(suffix, value);
    }
}

// Split one .slang into its two stages, collecting the pragmas along the way.
struct SplitSource {
    std::string vertex;
    std::string fragment;
    std::string format;
    std::string alias;
    std::vector<GenParam> params;
};

SplitSource SplitShader(const std::filesystem::path& input) {
    SplitSource out;
    std::ostringstream vertex, fragment;
    bool isVertex = true, isFragment = true;

    for (const auto& line : LoadSource(input.lexically_normal(), true)) {
        const auto trimmed = trim(line);

        if (line.rfind("#pragma parameter", 0) == 0) {
            // De-duped: a header included twice would otherwise declare its parameters twice.
            GenParam param(line, 4, 0);
            bool dupe = false;
            for (const auto& p : out.params) {
                if (p.name == param.name) {
                    dupe = true;
                    break;
                }
            }
            if (!dupe) out.params.push_back(param);
            continue;
        }
        if (line.rfind("#pragma stage vertex", 0) == 0) {
            isFragment = false;
            isVertex = true;
            continue;
        }
        if (line.rfind("#pragma stage fragment", 0) == 0) {
            isFragment = true;
            isVertex = false;
            continue;
        }
        if (trimmed.rfind("#pragma format", 0) == 0) {
            out.format = trim(trimmed.substr(14));
            continue;
        }
        if (trimmed.rfind("#pragma name", 0) == 0) {
            out.alias = trim(trimmed.substr(12));
            continue;
        }

        if (isFragment) fragment << line << "\n";
        if (isVertex) vertex << line << "\n";
    }

    out.vertex = vertex.str();
    out.fragment = fragment.str();
    return out;
}

// Compile one .slang, or hand back the one already compiled from the same file.
//
// The cache is what makes a whole tree tractable. Mega Bezel alone has hundreds of preset variants
// over one set of passes; without this, each variant recompiles all 48 of them through glslang and
// then emits its own copy of the result.
std::shared_ptr<GenShaderData> CompileShaderCached(const std::filesystem::path& input,
                                                   Registry& registry, std::ostream& log,
                                                   bool& warn) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(input, ec);
    if (ec) canonical = input.lexically_normal();
    const std::string key = canonical.string();

    ++registry.shaderRequests;
    auto it = registry.shaders.find(key);
    if (it != registry.shaders.end()) return it->second;

    SplitSource split = SplitShader(input);

    auto shader = std::make_shared<GenShaderData>();
    shader->key = key;
    shader->name = input.filename().string();
    shader->format = split.format;
    shader->alias = split.alias;

    shader->vertex = GenerateSPIRV(split.vertex.c_str(), false, log, warn);
    shader->fragment = GenerateSPIRV(split.fragment.c_str(), true, log, warn);

    // The fragment stage carries the full uniform block and every texture, which is why ShaderGC
    // reflects that one and not the vertex stage.
    const Reflection r = Reflect(shader->fragment);

    std::vector<GenSampler> textures;
    shader->params = LookupParams(split.params, textures, r);
    shader->samplers = textures;

    registry.shaders.emplace(key, shader);
    return shader;
}

// 64-bit FNV-1a. Nothing here is adversarial -- this only has to tell two texture files apart -- and
// a collision would have to be between two files that are both in the same catalogue.
uint64_t HashBytes(const std::vector<uint8_t>& data) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

GenPass MakePass(const std::filesystem::path& input, Registry& registry, std::ostream& log,
                 bool& warn) {
    GenPass pass;
    pass.shader = CompileShaderCached(input, registry, log, warn);
    // A #pragma name in the shader is the pass's default alias; a preset may override it below.
    if (!pass.shader->alias.empty()) pass.presetParams.emplace("alias", pass.shader->alias);
    return pass;
}

}  // namespace

GenParam::GenParam(const std::string& pragma, int size, int buffer)
    : buffer(buffer), size(size) {
    std::istringstream iss(pragma);
    // "#pragma parameter NAME "Description" default min max [step]"
    iss >> name;  // #pragma
    iss >> name;  // parameter
    iss >> name;  // NAME
    iss >> std::quoted(desc);
    desc = trim(ascii(desc));
    iss >> def;
    iss >> min;
    iss >> max;
    if (!iss.eof()) iss >> step;
}

std::vector<std::string> LoadSource(const std::filesystem::path& input, bool followIncludes) {
    std::vector<std::string> lines;

    std::ifstream infile(input);
    if (!infile.good()) throw file_error("unable to find " + input.string());

    std::string line;
    while (std::getline(infile, line)) {
        if (followIncludes && line.rfind("#include", 0) == 0) {
            std::istringstream iss(line);
            std::string directive, file;
            iss >> directive;
            // '\0' as the escape character: a backslash here is a path separator, not an escape.
            iss >> std::quoted(file, '"', '\0');

            std::filesystem::path includePath(input);
            includePath.remove_filename();
            includePath /= RelativePath(file);

            const auto included = LoadSource(includePath.lexically_normal(), true);
            lines.insert(lines.end(), included.begin(), included.end());
        } else {
            lines.push_back(line);
        }
    }
    return lines;
}

void ParsePreset(const std::filesystem::path& input, std::map<std::string, std::string>& keyValues,
                 std::map<std::string, std::filesystem::path>& valuePaths) {
    std::ifstream infile(input.lexically_normal());
    if (!infile.good()) throw file_error("unable to find " + input.lexically_normal().string());

    std::string line;
    while (std::getline(infile, line)) {
        if (line.rfind("#reference", 0) == 0) {
            std::istringstream iss(line);
            std::string directive, file;
            iss >> directive;
            // '\0' as the escape character: a backslash here is a path separator, not an escape.
            iss >> std::quoted(file, '"', '\0');

            std::filesystem::path includePath(input);
            includePath.remove_filename();
            includePath /= RelativePath(file);
            ParsePreset(includePath, keyValues, valuePaths);
            continue;
        }
        if (line.rfind("#", 0) == 0) continue;
        // A commented-out setting is still a line with an "=" in it; without this, presets that
        // explain themselves ("// float_framebuffer1 = true has been commented ...") turn into
        // junk keys.
        if (trim(line).rfind("//", 0) == 0) continue;
        if (line.find('=') == std::string::npos) continue;

        const auto kv = GetKeyValue(line);
        keyValues[kv.first] = kv.second;
        // Paths resolve against the file that declared them, not the preset being compiled.
        valuePaths[kv.first] = input.parent_path();
    }
}

GenPreset CompilePreset(const std::filesystem::path& input, Registry& registry, std::ostream& log,
                        bool& warn) {
    GenPreset preset;
    preset.name = input.filename().string();
    preset.category = "Imported";

    // A bare .slang is a one-pass preset.
    std::string ext = input.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".slang") {
        preset.passes.push_back(MakePass(input, registry, log, warn));
        return preset;
    }

    std::map<std::string, std::string> keyValues;
    std::map<std::string, std::filesystem::path> keyPaths;
    std::unordered_set<std::string> seen;
    ParsePreset(input, keyValues, keyPaths);

    const int passCount = atoi(GetValue("shaders", -1, keyValues, seen).c_str());
    if (passCount <= 0) throw fragment_error("parameter fragment: no shaders declared");

    for (int i = 0; i < passCount; ++i) {
        auto path = GetPath("shader", i, keyValues, keyPaths, seen).lexically_normal();
        try {
            GenPass pass = MakePass(path, registry, log, warn);
            SetPassParams(pass, i, keyValues, seen);
            preset.passes.push_back(std::move(pass));
        } catch (const std::exception& e) {
            // A 29-pass preset that just says "compilation failed" is not a diagnosis. Say which
            // pass and which file, since that is what anyone chasing the failure needs.
            throw std::runtime_error("pass " + std::to_string(i) + " (" + path.string() +
                                     "): " + e.what());
        }
    }

    auto textureList = GetValue("textures", -1, keyValues, seen);
    if (!textureList.empty()) {
        textureList += ";";
        size_t pos = 0;
        while ((pos = textureList.find(';')) != std::string::npos) {
            const std::string name = textureList.substr(0, pos);
            textureList.erase(0, pos + 1);
            if (name.empty()) continue;

            const auto path = GetPath(name, "", keyValues, keyPaths, seen).lexically_normal();

            GenTexture tex;
            tex.name = name;
            tex.presetParams.emplace("name", name);
            SetTextureParams(tex, name, keyValues, seen);

            // Embedded exactly as it sits on disk. Nothing here decodes it, which is why the
            // generator needs no image library at all; the runtime decodes when it builds the chain.
            std::ifstream inf(path, std::ios::binary | std::ios::ate);
            if (!inf.good()) throw file_error("unable to load texture " + path.string());
            const auto size = inf.tellg();
            inf.seekg(0, std::ios::beg);
            const size_t byteCount = static_cast<size_t>(size);
            std::vector<uint8_t> bytes(byteCount);
            inf.read(reinterpret_cast<char*>(bytes.data()), size);

            // Shared by content: one set of bezel art is referenced by hundreds of Mega Bezel
            // presets, and emitting it per preset is what made the first attempt unbuildable.
            char hash[32];
            snprintf(hash, sizeof(hash), "%016llx_%zu",
                     (unsigned long long) HashBytes(bytes), bytes.size());

            ++registry.textureRequests;
            auto tit = registry.textures.find(hash);
            if (tit == registry.textures.end()) {
                auto data = std::make_shared<GenTextureData>();
                data->key = hash;
                data->data = std::move(bytes);
                tit = registry.textures.emplace(hash, data).first;
            }
            tex.data = tit->second;

            preset.textures.push_back(std::move(tex));
        }
    }

    // Anything left over is a parameter override. A key that is not a number is reported rather
    // than dropped silently -- it usually means a preset key this port does not know about yet.
    for (const auto& kv : keyValues) {
        if (seen.count(kv.first) || kv.first.empty() || kv.second.empty()) continue;
        try {
            preset.overrides.push_back(GenOverride {kv.first, std::stof(kv.second)});
        } catch (const std::exception&) {
            log << "  ignoring non-numeric key: " << kv.first << " = " << kv.second << "\n";
            warn = true;
        }
    }

    return preset;
}

}  // namespace shaderglass
