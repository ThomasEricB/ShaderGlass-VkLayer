/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Loads a generated catalogue exactly as the layer will: dlopen, resolve the four ABI symbols, and
read a preset back. Run it against a directory the generator produced.

    catalog_test build/catalog/cm/libShaderGlassPresets.so crt/test-crt
*/

#include "../presets/preset_api.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: catalog_test <libShaderGlassPresets.so> [preset-id]\n");
        return 2;
    }

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto abi = (uint32_t (*)(void)) dlsym(lib, "SgPresetAbiVersion");
    auto count = (size_t (*)(void)) dlsym(lib, "SgPresetCount");
    auto at = (const SgPreset* (*) (size_t)) dlsym(lib, "SgPresetAt");
    auto find = (const SgPreset* (*) (const char*)) dlsym(lib, "SgFindPreset");

    if (!abi || !count || !at || !find) {
        fprintf(stderr, "catalogue is missing one of the four ABI symbols\n");
        return 1;
    }
    if (abi() != SG_PRESET_ABI_VERSION) {
        fprintf(stderr, "catalogue ABI %u, this build expects %u\n", abi(), SG_PRESET_ABI_VERSION);
        return 1;
    }

    printf("abi=%u presets=%zu\n", abi(), count());
    for (size_t i = 0; i < count(); ++i) {
        const SgPreset* p = at(i);
        printf("  %-24s category=%-10s passes=%zu textures=%zu overrides=%zu\n", p->id, p->category,
               p->pass_count, p->texture_count, p->override_count);
    }

    if (argc < 3) return 0;

    const SgPreset* p = find(argv[2]);
    if (!p) {
        fprintf(stderr, "preset not found: %s\n", argv[2]);
        return 1;
    }

    printf("\n%s (%s)\n", p->id, p->name);
    int failures = 0;
    for (size_t i = 0; i < p->pass_count; ++i) {
        const SgPass& pass = p->passes[i];
        const SgShader& sh = *pass.shader;
        printf("  pass %zu: %s  vert=%zu words  frag=%zu words  params=%zu  samplers=%zu\n", i,
               sh.name, sh.vertex_words, sh.fragment_words, sh.param_count, sh.sampler_count);

        // The SPIR-V has to still be SPIR-V after a round trip through generated C and a shared
        // object -- which is the whole point of this test.
        if (sh.vertex_words < 5 || sh.vertex_spirv[0] != 0x07230203u) {
            printf("    BAD vertex SPIR-V magic\n");
            ++failures;
        }
        if (sh.fragment_words < 5 || sh.fragment_spirv[0] != 0x07230203u) {
            printf("    BAD fragment SPIR-V magic\n");
            ++failures;
        }

        for (size_t k = 0; k < pass.preset_param_count; ++k)
            printf("    key %s = %s\n", pass.preset_params[k].key, pass.preset_params[k].value);
        for (size_t k = 0; k < sh.param_count; ++k) {
            const SgParam& pr = sh.params[k];
            printf("    param %-18s buffer=%d offset=%3d size=%2d  [%g..%g] def=%g step=%g  %s\n",
                   pr.name, pr.buffer, pr.offset, pr.size, pr.min_value, pr.max_value,
                   pr.default_value, pr.step_value, pr.description);
        }
        for (size_t k = 0; k < sh.sampler_count; ++k)
            printf("    sampler %-12s binding=%d\n", sh.samplers[k].name, sh.samplers[k].binding);
    }
    for (size_t i = 0; i < p->texture_count; ++i)
        printf("  texture %s (%zu bytes)\n", p->textures[i].name, p->textures[i].data->length);
    for (size_t i = 0; i < p->override_count; ++i)
        printf("  override %s = %g\n", p->overrides[i].name, p->overrides[i].value);

    dlclose(lib);
    printf("\n%s\n", failures ? "FAILURES" : "catalogue readable, SPIR-V intact");
    return failures ? 1 : 0;
}
