# ShaderGlassVk

ShaderGlass as a Vulkan layer: libretro shader presets applied to a game's own frames, on Linux,
with no window capture and no overlay.

The Windows app captures another window with Windows Graphics Capture, runs the shader chain in
D3D11, and presents the result in its own always-on-top window. None of that works well on Linux — 
WGC has no counterpart, D3D11 is absent, and a click-through overlay over a fullscreen game is
unreliable under Wayland. A Vulkan layer needs none of it: it is loaded into the game's own process
and intercepts `vkQueuePresentKHR`, where the finished frame is already in hand.

This tree is additive. Nothing outside `ShaderGlassVk/` is touched, and the Windows app builds
exactly as it did before.

> **Status: phase 3 of 7.** The layer runs a real shader pass over the game's frames, and
> `shaderglass-gen` compiles libretro `.slangp` presets into a SPIR-V catalogue that builds into a
> loadable library. Phase 4 connects the two: running catalogue presets instead of the built-in
> passthrough. See [DESIGN.md](DESIGN.md) for the full plan.

## Building

Needs a C++17 compiler, Vulkan headers, and Meson or CMake.

Two optional headers, each detected at configure time, each degrading to something that still works:

| | For | Without it |
|---|---|---|
| `stb_image.h` (Arch `stb`, Debian `libstb-dev`, Fedora `stb_devel`) | Decoding preset textures — LUTs, bezels, overlays | Presets run, minus their textures |
| SPIRV-Cross | The generator's primary reflection backend | The built-in SPIR-V reader is used instead |

stb_image is header-only, so it adds nothing to what the layer links against — worth caring about for
a library mapped into every game on the system. If you add it to an existing build tree, CMake caches
the earlier negative result: configure a fresh one, or `meson setup --clearcache --reconfigure`.

```bash
meson setup build/native --native-file meson/native-clang.ini
meson compile -C build/native
```

The 32-bit layer, for 32-bit games — which is a good share of the audience for CRT shaders:

```bash
meson setup build/linux32 --native-file meson/native-clang.ini \
                          --cross-file meson/cross-clang-linux32.ini
meson compile -C build/linux32
```

CMake produces the same binaries, for anyone who would rather review that:

```bash
cmake -B build/cmake -S . && cmake --build build/cmake -j
```

## Trying it

There is no installer yet (phase 7). Point the loader at the manifest by hand:

```bash
# the manifest's library_path must resolve; an absolute path is simplest while developing
sed "s#\"./libVkLayer_ShaderGlass.so\"#\"$PWD/build/native/layer/libVkLayer_ShaderGlass.so\"#" \
    layer/manifest/VK_LAYER_SHADERGLASS.json > build/manifest/VK_LAYER_SHADERGLASS.json

VK_ADD_IMPLICIT_LAYER_PATH="$PWD/build/manifest" SHADERGLASS=1 vkcube
```

`VK_ADD_IMPLICIT_LAYER_PATH`, not `VK_ADD_LAYER_PATH` — the latter only covers explicit layers and
will silently do nothing here.

In another terminal:

```bash
SHM=/tmp/shaderglass-$UID/shm.bin
./build/native/tools/shaderglass-ctl $SHM status
./build/native/tools/shaderglass-ctl $SHM settings

# Any preset id selects the built-in passthrough until the catalogue exists.
./build/native/tools/shaderglass-ctl $SHM preset __builtin

# Show the shader a quarter-size raster -- ShaderGlass's "pixel size", which at
# nearest sampling makes the result visibly chunky.
./build/native/tools/shaderglass-ctl $SHM set sourcemode 1
./build/native/tools/shaderglass-ctl $SHM set sourcedivisor 4
```

### Checking it is correct, not just running

With matching rasters the passthrough must reproduce the frame exactly, and the layer can prove it:

```bash
SHADERGLASS_SELFTEST=1 VK_ADD_IMPLICIT_LAYER_PATH="$PWD/build/manifest" SHADERGLASS=1 vkcube
# [selftest] PASS: the passthrough reproduced the frame exactly (500x500, 1000000 bytes)
```

A half-texel sampling offset — the classic error in a pass like this — fails that comparison, so it
is worth running after any change to the quad, the MVP or the sampler. Running under
`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` at the same time covers the API side.

(With no preset selected the frame is passed through untouched and the chain is not run at all, so
`SHADERGLASS_SELFTEST=1` is also what turns the passthrough on in the first place.)

### Every preset, with no game and no window

A game proves a handful of presets a few seconds at a time, which will not find the preset whose
eleventh pass wants a format the device will not render to. `chain_test` creates a device, hands the
chain an ordinary image where the swapchain's would go, and builds and records every preset in a
catalogue:

```bash
SHADERGLASS_PRESETS=build/catalog/cm/libShaderGlassPresets.so \
VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation \
  ./build/native/tools/chain_test

# chain_test [--stride N] [--limit N] [--width W] [--height H] [preset-id ...]
```

Each preset is recorded twice, so feedback passes have a previous frame to read and the history ring
turns over. `semantics_test` covers the libretro naming rules on their own, with no device at all:

```bash
meson test -C build/native
```

## Compiling presets

`shaderglass-gen` is ShaderGen's job done for Vulkan. It compiles libretro `.slangp` presets to
SPIR-V and emits generated C — one `.cpp` per preset plus an index — which builds into
`libShaderGlassPresets.so`.

Build the whole catalogue with one command. **This is the step a package's build runs**, and the one
to use rather than calling the generator by hand, because it also applies the shader patches — see
below:

```bash
tools/build-catalogue.sh                        # clones into build/slang-shaders
tools/build-catalogue.sh /path/to/slang-shaders # or use a tree you already have

./build/native/tools/catalog_test build/catalog/cm/libShaderGlassPresets.so crt/crt-geom
```

The generator can still be driven directly for one preset or a subtree, which is what you want while
working on a shader:

```bash
./build/native/gen/shaderglass-gen --out build/catalog --tree /path/to/slang-shaders
```

### Shader patches

A few shaders in the libretro tree are broken as written rather than merely different from what we
would do, and `patches/` carries a fix for each with its reasoning in the patch header. They are not
cosmetic: `crt/simple-crt` raises a negative number to a power, which is undefined in GLSL, and on a
driver whose `pow()` returns NaN for a negative base — NVIDIA's Vulkan compiler does — roughly 88 %
of every frame comes out black.

`tools/build-catalogue.sh` applies them, and stops rather than generating from an unpatched tree, so
a shipped catalogue cannot quietly be missing them. The tree is modified in place; the patches are
idempotent and `tools/patch-shaders.sh --revert <tree>` takes them back out.

A patch that no longer applies is reported as a failure rather than skipped — usually it means the
shader was fixed upstream, and the patch should be dropped from `patches/`.

The catalogue is its own shared object rather than part of the layer, because the layer is mapped
into every game that has ShaderGlass armed and a game with no preset selected should pay nothing for
shaders it is not using. The layer `dlopen`s it on the first frame a preset is chosen.

### Reflection backends

The generator has to know where each parameter sits in a shader's uniform block. It gets that from
**SPIRV-Cross** when the library is installed, and from a **built-in SPIR-V reader** when it is not —
so the generator builds with nothing but glslang. Both are compiled when SPIRV-Cross is present, and

```bash
./build/native/gen/shaderglass-gen --reflect-check --tree /path/to/slang-shaders
```

runs them over every preset and reports any disagreement. A fallback nobody exercises is a fallback
nobody can trust.

## How it is wired

```
        game process                                   your desktop
┌────────────────────────────────┐              ┌──────────────────────────┐
│  libVkLayer_ShaderGlass.so     │   settings   │  shaderglass-gui (Qt 6)  │
│   vkQueuePresentKHR hook       │◄────────────►│   preset + parameters    │
│   source raster → passes →     │     shm      │   profiles, status       │
│   aspect · flip · crop → swap  │  (no frames) │                          │
└────────────────────────────────┘              └──────────────────────────┘
```

The shared mapping carries **settings and status only** — 64 KiB, no pixel regions. The chain runs on
the game's own device, so no frame ever leaves the process. That is the main simplification over
DLSS5VKLayer, which had to ship every frame to a Windows helper and wait for it to come back.

## Environment

| Variable | Effect |
|---|---|
| `SHADERGLASS=1` | Arms the layer. Without it the layer is inert in every process |
| `SHADERGLASS_DISABLE=1` | Forces it off even when armed |
| `SHADERGLASS_SHM` | Path to the mapping. Defaults to `/tmp/shaderglass-$UID/shm.bin` |
| `SHADERGLASS_LOG` | Log file. Defaults to stderr |
| `SHADERGLASS_VERBOSE=1` | Log every present |
| `SHADERGLASS_TIME=1` | Periodic frame-count lines |
| `SHADERGLASS_SELFTEST=1` | Run the chain with no preset and compare its output against its input once, logging whether they match |
| `SHADERGLASS_PRESETS` | Path to `libShaderGlassPresets.so`. Without it the normal loader search runs, which is what a packaged install wants |

The mapping lives under `/tmp` rather than `$XDG_RUNTIME_DIR` on purpose: a Steam game runs inside
pressure-vessel, which gives the container a private tmpfs there, so a mapping put in it is simply
absent inside the game — the layer would create its own empty one at a path that reads identically in
the log while the interface talked to the other file. `/tmp` is bind-mounted from the host into the
container, so both sides land on one file.

## Licence

GPL-3.0, matching ShaderGlass. Portions derive from DLSS5VKLayer (AGPL-3.0) and are relicensed to
GPL-3.0 with the rights holders' permission — see [RELICENSE.md](RELICENSE.md).
