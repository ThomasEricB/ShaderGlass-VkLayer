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

It runs the whole libretro `slang-shaders` catalogue — about 3300 presets, CRT, scanline, handheld,
upscaling — on any Vulkan game, 64-bit or 32-bit, including Proton/DXVK games. OpenGL games reach it
through Zink or through gamescope. [DESIGN.md](DESIGN.md) has the design and every decision behind it.

## Installing

Every package holds the same files: both layers and both preset catalogues in `/usr/lib/shaderglass`,
the programs `shaderglass-gui`, `shaderglass-ctl`, `shaderglass-run` and `shaderglass-gen`, one Vulkan
manifest per architecture, and `zz-shaderglass.conf` in `environment.d` (see *Running a game*).

| | |
|---|---|
| Arch | `packaging/PKGBUILD` — builds from source, the catalogue included |
| Fedora and other RPM systems | `packaging/make-dist.sh rpm` |
| Debian, Ubuntu | `packaging/make-dist.sh deb` (needs `dpkg-deb`) |
| Anywhere | `packaging/make-dist.sh` makes a tarball; inside it, `./install.sh` installs for you under `~/.local`, `./install.sh --system` for everyone under `/usr/local` |

`make-dist.sh` builds whatever is not built yet, catalogue included, which takes a few minutes. A
tarball install records what it put where, and `~/.local/lib/shaderglass/uninstall.sh` removes
exactly that.

**Log out and back in once after installing.** The layer order is read when the session starts.

## Running a game

Add this to the game's launch options in Steam — or put it in front of the command anywhere else:

```
SHADERGLASS=1 %command%
```

Then open **ShaderGlass** (`shaderglass-gui`), pick a preset, and tune it while the game runs. Nothing
happens to a game started without `SHADERGLASS=1`.

- **OpenGL games** are invisible to a Vulkan layer. The interface says so when a game switches to
  OpenGL, and offers the launch options that route it through Vulkan with Zink. Running the game in
  gamescope works too; see the Advanced tab.
- **Presets made for a console** — `bezel/scanline-classic` above all, whose composite and S-video
  presets model the signal a SNES or a Mega Drive sent to a TV — expect that console's raster, like
  256×224, as their input. Fed a 2560×1080 frame they come out colourless and striped. Set the source
  raster on the Input tab to the console's, or let *Detect automatically* measure a pixel-art game.
- **HDR presets** (`fhd-hdr`, `uhd-4k-hdr`) produce an HDR10 signal and look washed out on an SDR
  screen; use the `-sdr` ones there.
- **Handheld overlays for PSP** come in two versions; the `Y_flip` ones are the right way up here.
- **DLSS5VKLayer** is supported: installed together, ShaderGlass runs after it and shades the finished
  picture. See [docs/DLSS5VKLayer.md](docs/DLSS5VKLayer.md).

`shaderglass-run [--gamescope[=output]] command...` does the same from a terminal or a `.desktop` file.

## Building

Needs a C++17 compiler, Vulkan headers, Meson or CMake, glslang (for the preset compiler) and, for
the interface, Qt 6.

Two optional headers, each detected at configure time, each degrading to something that still works:

| | For | Without it |
|---|---|---|
| `stb_image.h` (Arch `stb`, Debian `libstb-dev`, Fedora `stb_devel`) | Decoding preset textures — LUTs, bezels, overlays | Presets run, minus their textures |
| SPIRV-Cross | The generator's primary reflection backend | The built-in SPIR-V reader is used instead |

stb_image is header-only, so it adds nothing to what the layer links against — worth caring about for
a library mapped into every game on the system. If you add it to an existing build tree, CMake caches
the earlier negative result: configure a fresh one, or `meson setup --clearcache --reconfigure`.

Everything, both architectures:

```bash
tools/build.sh             # the layers, the tools and the interface
tools/build-catalogue.sh   # the preset catalogue, both architectures (see Compiling presets)
```

or by hand:

```bash
meson setup build/native --native-file meson/native-clang.ini
meson compile -C build/native

# The 32-bit layer, for 32-bit games -- a good share of the audience for CRT shaders.
meson setup build/linux32 --native-file meson/native-clang.ini \
                          --cross-file meson/cross-clang-linux32.ini
meson compile -C build/linux32
```

CMake produces the same binaries, for anyone who would rather review that:

```bash
cmake -B build/cmake -S . && cmake --build build/cmake -j
```

## Running from the build tree

```bash
tools/local-install.sh               # manifests pointing at build/, and the session layer order
tools/local-install.sh --uninstall
```

A rebuild then takes effect without reinstalling. The layer finds the catalogue in `build/catalog`
by itself.

`shaderglass-ctl` drives the same settings the interface does, from a shell:

```bash
SHM=/tmp/shaderglass-$UID/shm.bin
./build/native/tools/shaderglass-ctl $SHM status
./build/native/tools/shaderglass-ctl $SHM settings
./build/native/tools/shaderglass-ctl $SHM preset crt/crt-geom
./build/native/tools/shaderglass-ctl $SHM set sourcemode 1
./build/native/tools/shaderglass-ctl $SHM set sourcedivisor 4
```

### Checking it is correct, not just running

With matching rasters the passthrough must reproduce the frame exactly, and the layer can prove it:

```bash
SHADERGLASS_SELFTEST=1 SHADERGLASS=1 vkcube     # after tools/local-install.sh
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
#   --source WxH       a source raster smaller than the frame, as the output tab sets one
#   --frames N         run N frames instead of 2, for what feedback does over time
#   --stats            per-pass ranges and NaN counts
#   --dump DIR         write the result of each preset as a picture; with --dump-passes, every pass
```

Each preset is recorded twice, so feedback passes have a previous frame to read and the history ring
turns over. `semantics_test` covers the libretro naming rules on their own, with no device at all:

```bash
meson test -C build/native
```

## The interface

`shaderglass-gui` is a second process that shares the file mapping with the layer. It does not
launch the game, attach to it, or know anything about it beyond what the layer writes into that
mapping, so it can be started and stopped at any point — including while a game is running.

- **Shader** — the catalogue as a tree, with a filter box, and the selected preset's parameters as
  sliders. What it publishes is *overrides*: a parameter you have not touched is absent, so a preset
  with several hundred of them still fits the 128 slots the protocol reserves.
- **Input** — the source raster the shader is shown: native, a divisor, a classic console raster, a
  custom size, or *Detect automatically*, which measures a pixel-art game's real grid and keeps
  measuring as scenes change.
- **Output** — pixel size, output policy (Auto, Stretch, Fit, Fill, Integer, Centre 1:1), aspect
  correction, rotation and mirrors, and a crop that confines the effect to part of the window, typed
  or dragged on the newest capture.
- **Advanced** — frame skip, how often automatic detection re-measures, and the gamescope launch
  options, with a custom gamescope build if you use one.
- **Capture** — the matched pair the layer writes on request: the frame the game presented, and the
  frame it presented after the chain ran. There is no preview renderer (decision 8); a second render
  in another process would be a different frame at a different raster.

A setting changed while the game is paused still reaches the screen: the layer composes the frame it
last had again (`SHADERGLASS_REPAINT=0` turns that off).

Profiles save the preset, the settings and the parameter overrides together, under
`~/.local/share/ShaderGlass/ShaderGlassVk/profiles`. They are keyed by the same names
`shaderglass-ctl` uses, so a profile is readable and stays valid when the interface's wording does
not.

Qt 6 is optional: a build without it still produces the layer, the generator and the control tool,
which is everything a headless or packaging build needs.

## Compiling presets

`shaderglass-gen` is ShaderGen's job done for Vulkan. It compiles libretro `.slangp` presets to
SPIR-V and emits generated C — one `.cpp` per preset plus an index — which builds into
`libShaderGlassPresets.so`.

Build the whole catalogue with one command. **This is the step a package's build runs**, and the one
to use rather than calling the generator by hand, because it also applies the shader patches — see
below:

```bash
tools/build-catalogue.sh                        # ../Scripts/slang-shaders, or fetch it
tools/build-catalogue.sh /path/to/slang-shaders # or use a tree you already have

./build/native/tools/catalog_test build/catalog/cm/libShaderGlassPresets.so crt/crt-geom
```

The generator can still be driven directly for one preset or a subtree, which is what you want while
working on a shader:

```bash
./build/native/gen/shaderglass-gen --out build/catalog --tree /path/to/slang-shaders
```

With no tree named, the catalogue is built from the one the Windows app uses:
`Scripts/DownloadShaders.bat` clones mausimus's slang-shaders fork, branch `shaderglass`, into
`Scripts/slang-shaders`, and when that is there it is used as it is. Otherwise the fork is fetched at
a fixed commit — the one the patches below are made against — so that a package built next year
applies them the same way.

### Shader patches

A few shaders in the libretro tree are broken as written rather than merely different from what we
would do, and `patches/` carries a fix for each with its reasoning in the patch header. They are not
cosmetic: `crt/simple-crt` raises a negative number to a power, which is undefined in GLSL, and on a
driver whose `pow()` returns NaN for a negative base — NVIDIA's Vulkan compiler does — roughly 88 %
of every frame comes out black.

`bezel/scanline-classic`'s `limiter.slang` has the same fault applied to a composite signal's
sub-black excursions.

`downsample/mixed-res/*/mixed-res-nnedi3-luma` still names its nnedi3 passes `PassOutput0` and
`PassOutput3`, from before those shaders were changed to read `nnediPass0` and `nnediPass3`; the two
samplers find nothing, and the picture comes out magenta.

`bezel/koko-aio` ships with `_HAS_ROTATION_UNIFORM` commented out for old RetroArch versions, so it
guesses rotation from pass sizes instead of reading the `Rotation` uniform. The guess is wrong for its
fixed-size ambient-light pass, which leaves black rectangles in the glow either side of the bezel.

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
| `SHADERGLASS_REPAINT=0` | Turns off the idle repaint, which re-composes the last frame when a setting changes while the game has stopped presenting |
| `SHADERGLASS_GAMESCOPE` | `0` keeps the layer out of gamescope's composite. A path names a custom gamescope build whose file is not called `gamescope`, so the layer knows that process is gamescope |
| `SHADERGLASS_GAMESCOPE_BIN` | For `shaderglass-run`: a custom gamescope build to use instead of the one on `PATH` (also `--gamescope-bin=PATH`) |
| `SHADERGLASS_SHM` | Path to the mapping. Defaults to `/tmp/shaderglass-$UID/shm.bin` |
| `SHADERGLASS_LOG` | Log file. Defaults to stderr |
| `SHADERGLASS_VERBOSE=1` | Log every present |
| `SHADERGLASS_TIME=1` | Periodic frame-count lines |
| `SHADERGLASS_SELFTEST=1` | Run the chain with no preset and compare its output against its input once, logging whether they match |
| `SHADERGLASS_PRESETS` | Path to `libShaderGlassPresets.so`. Without it the normal loader search runs, which is what a packaged install wants |
| `SHADERGLASS_GUI_GRAB` | Render the interface's window to this path and exit — how the layout is checked without a person looking at it |
| `SHADERGLASS_GUI_TAB` | Which tab that grab should show |

The mapping lives under `/tmp` rather than `$XDG_RUNTIME_DIR` on purpose: a Steam game runs inside
pressure-vessel, which gives the container a private tmpfs there, so a mapping put in it is simply
absent inside the game — the layer would create its own empty one at a path that reads identically in
the log while the interface talked to the other file. `/tmp` is bind-mounted from the host into the
container, so both sides land on one file.

## Licence

GPL-3.0, matching ShaderGlass. Portions derive from DLSS5VKLayer (AGPL-3.0) and are relicensed to
GPL-3.0 with the rights holders' permission — see [RELICENSE.md](RELICENSE.md).
