# ShaderGlass on a Vulkan layer — design and port plan

**Status:** phases 1-4 complete (tree, builds, protocol, layer, the full multi-pass executor, and the .slangp -> SPIR-V compiler with a generated catalogue). The full libretro `slang-shaders` tree compiles: 3328 of 3330 presets, the other 2 being parameter fragments rather than presets, and 49 files failing only on dangling references that are broken in upstream itself. Phase 5 next.
**Scope:** everything below is additive under `ShaderGlassVk/`. No existing upstream file is modified,
moved or deleted, so the Windows app builds exactly as it does today.

---

## 1. Why a layer

ShaderGlass today *captures* another window with Windows Graphics Capture, runs a libretro shader
chain over the captured texture in D3D11, and presents the result in its own always-on-top overlay
window. Three of those four things have no usable Linux counterpart: WGC does not exist, a
click-through overlay over a fullscreen game is unreliable under Wayland, and D3D11 is absent.

A Vulkan layer removes the need for all three. The layer is loaded into the game's own process,
intercepts `vkQueuePresentKHR`, and already holds the finished swapchain image — no capture, no
compositing, no second window, no permissions. The shader chain runs on the game's own device and
the result is written back into the swapchain image before it is presented.

That is DLSS5VKLayer's architecture, minus the one thing that forced its complexity: a proprietary
Windows DLL that had to run in a second process under Wine. ShaderGlass needs no such thing, so the
helper, the frame transport, the dma-buf exchange, the optical flow and the NGX plumbing all
disappear. What is left is the part worth having.

```
                 game process                                   user's desktop
     ┌──────────────────────────────────────┐           ┌────────────────────────────┐
     │      libVkLayer_ShaderGlass.so       │           │   shaderglass-gui (Qt 6)   │
     │                                      │  settings │                            │
     │  vkQueuePresentKHR ─┐                │◄─────────►│  preset tree + parameters  │
     │                     ▼                │    shm    │  profiles, status, crop    │
     │   swapchain → source raster          │  (no      │                            │
     │        → pass 0 → pass 1 → … pass N  │   frames) │  imports compile in-process│
     │        → aspect/flip/crop → swapchain│           └────────────────────────────┘
     │                                      │                        │
     │   dlopen libShaderGlassPresets.so ───┼────────────────────────┘
     └──────────────────────────────────────┘        catalogue of ~3300 presets
```

The shared mapping carries **settings and status only**. Nothing on the frame path leaves the
process, which is a strict simplification of DLSS5VKLayer's protocol.

---

## 2. Decisions

| # | Decision |
|---|---|
| 1 | **Shader content**: all ~3300 RetroArch presets pre-generated to SPIR-V at build time |
| 2 | **Preset packaging**: generated C headers, linked into a separate `libShaderGlassPresets.so` that the layer `dlopen`s only when a preset is selected |
| 3 | **Naming**: tree `ShaderGlassVk/`; layer `libVkLayer_ShaderGlass.so`, manifest `VK_LAYER_SHADERGLASS` / `VK_LAYER_SHADERGLASS_32`; binaries `shaderglass-gui`, `shaderglass-ctl`, `shaderglass-gen` |
| 4 | **Licence**: the reused DLSS5VKLayer code is relicensed AGPL-3.0 → GPL-3.0 so the tree is single-licence. **Settled** — Thomas Eric is a rights holder and holds bmitch87's permission; recorded in `RELICENSE.md` and stated in the PR |
| 5 | **Source resolution**: Native, divisors /2…/8, classic rasters (320×240, 256×224, 640×480, 640×400) and a custom W×H |
| 6 | **Hotkeys**: none. Wayland makes in-game key capture unreliable, so `hotkey.cpp`, evdev and XInput2 are not ported at all |
| 7 | **Import**: the GUI links glslang and compiles an imported `.slangp` in-process into the user data directory, which the layer also searches |
| 18 | **Reflection**: SPIRV-Cross when installed, with a built-in SPIR-V reader as the always-available fallback. `shaderglass-gen --reflect-check` diffs the two |
| 8 | **Preview**: no preview renderer. The GUI displays the matched before/after screenshots the layer already writes, which gives a still comparison for free |
| 9 | **Build**: Meson as the working build, plus a `CMakeLists.txt` for reviewers who expect one |
| 10 | **Architectures**: 64-bit and 32-bit layers, with per-architecture layer names |
| 11 | **Packaging**: tarball with `install.sh --user/--system`, RPM, DEB and PKGBUILD; presets ship in the main package |
| 12 | **Crop**: drag a rectangle on the layer's screenshot *and* numeric boxes, kept in sync |
| 13 | **Default state**: no preset means the frame passes through untouched |
| 14 | **Idle repaint**: kept — settings changes re-compose the last frame while the game is paused |
| 15 | **GUI**: DLSS5VKLayer's tabbed settings widget and binder, with tabs named Shader / Input / Output / Advanced |
| 16 | **Pixel size stays its own control**, independent of source resolution: ×1…×10.8 as in the Windows app, deciding how many screen pixels one source pixel occupies. Defaults to **Auto** |
| 17 | **Output policy** — what happens when source × pixel size ≠ swapchain: Auto (default), Stretch to fill, Fit, Fill, Integer + letterbox, Centre 1:1 |

### The pass model (phase 4)

A pass reads what the *names in its shader* ask for, not what the preset lists: a sampler called
`PassFeedback2` wants pass 2's output from the previous frame, `OriginalHistory3` wants the frame
from three frames ago, and a `vec4` called `SourceSize` wants `(w, h, 1/w, 1/h)` of whatever the
pass is reading. `layer/src/semantics.h` is that vocabulary, resolved once when the chain is built
so recording a frame never compares a string.

| Decision | Why |
|---|---|
| **The last pass renders at the swapchain's raster, in the chain's format**, whatever its declared scale says | What RetroArch does with a final pass, and it makes the copy into the swapchain a plain `vkCmdCopyImage` |
| **Feedback passes ping-pong between two images**, rather than copying output to a feedback image | A full-resolution copy per feedback pass per frame, avoided for the cost of one extra image on the few passes that need it |
| **History is a ring written at the end of the frame**, after the passes have read it | At depth 1 the entry being written is the one `OriginalHistory1` just pointed at; writing first would feed a pass its own frame |
| **Feedback and history images are cleared on the first frame after a build** | They are sampled before anything writes them, and undefined contents are undefined behaviour, not merely an ugly first frame |
| **A declared pass format that the device cannot render to falls back to the chain's format — unless it is an integer format**, which fails the chain instead | Found by `test/format`: `#pragma format R32_UINT` is read back through a `usampler2D`, so substituting a UNORM image is a type error the shader cannot survive, not a loss of precision |
| **A sampler that resolves to nothing binds the frame** | An unwritten descriptor is undefined behaviour; a wrong-looking pass is better than a crash inside someone's game |
| **`<Alias>Feedback` resolves to that pass's previous frame**, and `OriginalFeedback` to `OriginalHistory1` | Sixty-odd libretro shaders spell feedback by alias rather than by index — `AfterglowPassFeedback`, `AvgLumPassFeedback`. Reading them as ordinary texture names is not an error anyone sees: the pass silently samples the current frame, which is a feedback loop with no delay. Finding this took counting feedback passes in the build log — a Mega Bezel preset reported 0 and should have reported 6 |
| **Indexed texture sizes are spelled `OriginalHistorySize1`, `PassOutputSize0`, `PassFeedbackSize0`** — the index after "Size", not before | This is the only spelling the libretro tree uses: 43 occurrences, against none of the `<name><index>Size` form. The first implementation here had it backwards and the unit test asserted the wrong way round with it, so the test agreed with the assumption instead of catching it |
| **`OriginalFPS` reports the game's presentation rate, smoothed, and never zero** | It is the rate a libretro core runs its content at, and shaders derive line counts and colour-carrier timing by dividing by it. Left unimplemented it is a silent zero, and the whole `bezel/scanline-classic` family — every composite preset in it — turns to NaN at the demodulator and renders black. Smoothed because a rate that jitters frame to frame makes the picture jitter with it |
| **`FrameTimeDelta`, `OriginalAspect`, `OriginalAspectRotated`, `TotalSubFrames`, `CurrentSubFrame` and `Rotation` are filled in** | They are uniform-block members like any other, so an unhandled one is not a failure — it is a silent zero. Zero freezes anything integrating over frame time, and `TotalSubFrames` of zero is a divide waiting to happen. This layer composes once per present, so subframes are always 1 of 1 |
| **A `<Something>Size` that names nothing the preset declared keeps its parameter value** instead of reporting the frame's size | A shader parameter may end in "Size" too (`FrameHSize`), and there is no way to tell from the name. A sampler must be bound to something valid, so that path still substitutes the frame; a size can honestly stay what it was |
| **`mipmap_input` builds a real mip chain**, on pass outputs, on Original and on preset textures | 1645 presets set it and 2568 pass declarations turn it on — roughly half the catalogue. Without it a glow or bloom pass samples level 0 everywhere, which does not fail, it just quietly looks wrong. A mipmapped pass needs a second image view over level 0 alone, because a framebuffer attachment may not span a chain |
| **History frames are never mipmapped**, even when Original is | A history entry is a copy of Original, no preset asks for a chain on a past frame, and building one per entry per frame is not cheap |
| **`PassOutput#` resolves only for a pass earlier than the one asking** | Its own index is the image it is drawing into — a read-write hazard — and a later index has not rendered yet. libretro ships `test/feedback-noncausal` for precisely this case, and the name is the verdict |
| **Preset textures stay compressed in the catalogue and are decoded at load time** | Over the libretro tree they are 43 MiB as PNG and JPEG and 1.19 GiB as raw RGBA. Upstream decodes with WIC for the same reason; this uses stb_image, which is header-only and so adds nothing to what the layer links against — worth caring about for a library mapped into every game |
| **Parameter precedence: shader default, then preset override, then the interface** | Resolved when a value changes rather than per frame, since a preset like crt-royale has dozens of parameters across dozens of passes |
| **`Chain::FrameCompleted()` is the caller's one ordering obligation**: call it once the previous `Record`'s submit has retired | Texture staging buffers are tens of megabytes for a Mega Bezel preset and have to be released, but only the caller knows when the GPU is done with them. An earlier version inferred it inside `Record` and destroyed a buffer underneath a command buffer that still referenced it — `chain_test` caught it on the first 23-texture preset it ran |

### Patching upstream shaders

The catalogue is generated from libretro's `slang-shaders` unmodified wherever possible. `patches/`
is the exception, for shaders that are broken as written rather than merely different: each patch
carries its reasoning in its header and is reportable upstream as-is.

The mechanism matters as much as the patches. `tools/build-catalogue.sh` is the single command that
fetches, patches, generates and compiles, it **refuses to generate from a tree it could not patch**,
and it is what packaging calls — so a catalogue that reaches a user cannot be missing them. A patch
that no longer applies is a failure, not a skip, because the likeliest reason is that the shader was
fixed upstream and the patch should go.

Both so far are the same mistake: a shader raising a negative number to a power. `pow(x, y)` is
undefined in GLSL for `x < 0`, and NVIDIA's Vulkan compiler returns NaN, which then survives every
guard the shader puts around it — `NaN * 0.0` is still NaN.

`crt/simple-crt` raises a negative number to a power. `pow(x, y)` is undefined
in GLSL for `x < 0`, the shader's `* float(diff > 0.0)` guard cannot discard the result because
NaN × 0 is still NaN, and on NVIDIA's Vulkan compiler about 88 % of every frame comes out black —
every pixel whose luma falls below the shader's threshold.

`bezel/scanline-classic`'s `limiter.slang` does the same to a composite signal's sub-black
excursions: its gamma guard is `color.r < 1.0`, meant to leave extended values above 1.0 alone,
which also admits the values below zero the same signal carries deliberately.

### Auditing the semantics, rather than remembering them

An unimplemented semantic does not fail. It reads zero, and the shader carries on into a divide or a
black frame — which is how `OriginalFPS` stayed missing. Eyeballing a frequency-sorted list is what
let it hide the first time, so the check is now mechanical and repeatable:

- every uniform block member in the catalogue with no `#pragma parameter` declaration is classified
  through the layer's own `ClassifyUniform`, and anything still landing on "parameter" is reviewed;
- every sampler name is classified through `ClassifyTexture`, and any structural-looking name that
  lands on "LUT or alias" is checked against what the presets actually declare.

`shaderglass-gen` now prints the first of those as a summary at the end of a tree run — *N uniform
members are neither a semantic nor a declared parameter (each reads zero)* — and links the layer's
`semantics.cpp` to do it, so the generator's idea of a semantic cannot drift from the runtime's.

The audit as it stands: 664 distinct undeclared uniform names, of which 55 are texture sizes and 12
are named semantics; the remaining 597 are shader parameters, and the only ones spelled like a
semantic (`MaxNits`, `PaperWhiteNits`, `InputGamma` and seven more) are declared with
`#pragma parameter` in most shaders and merely omitted in a few, so they read zero in RetroArch too.
Of 317 sampler names, every structural-looking one (`Pass1`, `PassPrev2`, `SourceHDR`) is a preset
alias — `Pass1` in 891 presets — and resolves through the alias mechanism.

### Decisions taken without asking, and why

- **No vendor check.** DLSS5VKLayer goes inert on non-NVIDIA devices because NGX is NVIDIA-only.
  Shader chains are not, so the check is removed and the layer runs on AMD, Intel and NVIDIA alike.
- **Graphics pipelines, not compute.** Slang passes are vertex+fragment pairs drawing a fullscreen
  quad. DLSS5VKLayer's pass machinery is entirely compute and cannot be reused as-is.
- **Fail-open everywhere.** Inherited wholesale: an unbuildable chain, a missing preset library, an
  unsupported swapchain format or any failed Vulkan call ends with the game's own frame presented.
- **`SHADERGLASS=1`** in the manifest's `enable_environment`, mirroring `VKLayer_DLSS5=1`, so the
  layer is inert in every process that does not ask for it.

### Source resolution and pixel size are two controls, not one

In the Windows app these collapse into a single knob only because the output window is resizable.
Here the swapchain is a fixed size set by the game, so they are independent and both are needed:

```
  swapchain           3840 × 2160          what the game presented
    → source raster   R  = ÷8, or explicit 256×224    what the chain is shown
    → shader chain
    → output raster   V  = R × pixel size             how big one source pixel lands
    → aspect · flip · crop → swapchain
```

The case that forces this: a modern re-release of a 2D game reports 4K, but its art sits on a
256×224 grid. The player has to be able to say what the real pixel grid is (**source resolution**)
*and* how many screen pixels one of those should occupy (**pixel size**) — because the same CRT
preset given a ×3 target draws three-pixel scanlines and given a ×12 target draws twelve-pixel ones.
One knob cannot express both.

**Pixel size defaults to Auto**, meaning the chain targets the full swapchain and the setting is
invisible to anyone who never touches it. Setting it explicitly makes `V ≠ swapchain` possible, and
the **output policy** decides what happens to the difference:

| Policy | Behaviour |
|---|---|
| **Auto** (default) | `V` = swapchain; pixel size is implied by the source raster. Nothing to reconcile |
| Stretch to fill | Scale `V` to the swapchain, ignoring aspect |
| Fit | Preserve aspect, scale to the largest that fits, letterbox the remainder |
| Fill | Preserve aspect, cover the swapchain, crop the overflow |
| Integer + letterbox | Scale by the largest whole multiple that fits and centre; the shader's pixel grid survives exactly |
| Centre 1:1 | Place `V` at its own size in the middle, cropping if it is larger — a true magnifier |

---

## 3. What is reused

### From DLSS5VKLayer

| Component | Fate |
|---|---|
| `layer.cpp` — instance/device hooks, dispatch chains, primary-swapchain selection, `vkSetDeviceLoaderData`, duplicate-copy guard, per-arch naming, `DEVICE_LOST` handling, idle repaint | **Keep**, minus the shm round trip, helper liveness and the vendor check |
| `vk_table.h` | **Keep**, plus render-pass/framebuffer/draw entry points |
| `shader_vk.{h,cpp}` | **Keep**; add a graphics-pipeline creator beside the compute one |
| `log.h`, `capture.{h,cpp}` | **Keep verbatim** |
| `shm_protocol.h` mechanism — atomics, `controlSeq`/`tuningSeq`, sequence-guarded strings, `ShmResetSettings`, per-pass override blocks | **Keep**; every DLSS-specific field replaced |
| `gui/` — `mainwindow`, `shm_binder`, `passdialog`, profiles, status line, first-run prompt | **Keep**; re-pointed at the new settings, plus a preset tree |
| `tools/shmctl` → `shaderglass-ctl` | **Keep** |
| meson build, cross files, manifest, `install.sh`/`uninstall.sh`, `environment.d` snippet, specs, PKGBUILD | **Keep** |
| `composition.{h,cpp}` — encode/resolve, white-point meter, HDR proxy, supersampling | **Replaced** by the pass-chain executor |
| `hotkey.{h,cpp}` | **Dropped** (decision 6) |
| `helper/`, `core/` (NGX ABI, parameter block, SEH guard, snippet lifecycle), optical flow, dma-buf transport, runner discovery, `dlssnr-helper`, Wine/Proton prefix handling | **Dropped entirely** |

### From ShaderGlass

| Component | Fate |
|---|---|
| `ShaderGC/` — `.slangp` parsing, `.slang` include resolution, stage splitting, glslang GLSL→SPIR-V, parameter reflection, sha256 cache | **Keep**, minus `HLSL.cpp` and `SPIRV::GenerateHLSL`. It already targets `GLSLANG_CLIENT_VULKAN`/`GLSLANG_TARGET_SPV`; the D3D11 path only got HLSL because SPIRV-Cross ran afterwards. Portability cost is three `__declspec(noinline)` and one `WIN32_LEAN_AND_MEAN` |
| `ShaderPass.cpp` — scale types, filter/wrap, feedback and history, `SourceSize`/`OutputSize`/`FrameCount`, MVP | **Port**: the libretro semantics transfer, the D3D11 calls do not |
| `ShaderGlass.cpp` — chain rebuild, pass target allocation, frame counter, feedback bookkeeping | **Port** |
| `ShaderGen/` | **Port** to emit SPIR-V headers instead of DXBC |
| `Preset.h`, `PresetDef.h`, `ShaderDef.h`, `TextureDef.h`, `SourceDefs.h` | **Keep** as the data model, with the D3D11 members swapped for Vulkan handles |
| `ShaderWindow.cpp`, `CaptureManager/Session`, `DeviceCapture`, `CursorEmulator`, `BrowserWindow`, `WineCap/`, `lib/`, `External/`, `.sln`/`.vcxproj` | **Not ported** |

---

## 4. Feature inventory — every Windows feature and its verdict

**Direct** ports as-is · **Reinterpreted** keeps the intent, changes the mechanism · **Dropped**
is meaningless once the layer owns the frame.

### Input

| Feature | Verdict | Notes |
|---|---|---|
| Capture window (scan, pick by title) | Dropped | The layer is *in* the process; selection is which game you launch with `SHADERGLASS=1` |
| Capture display / all displays | Dropped | |
| Capture device (webcam, capture card, formats) | Dropped | No frame source but the swapchain |
| Capture from image file | Reinterpreted | Becomes the GUI's screenshot view |
| Glass vs Clone mode | Dropped | Both describe where an output window sits |
| Capture cursor, remove yellow border | Dropped | Artifacts of Windows Graphics Capture |
| Cursor emulation (393 lines) | Dropped | The game draws its own cursor |
| Pixel size x1…x10.8 | **Direct** | Kept as its own control, alongside the new source-resolution setting — decisions 16 and 17 |
| Lock input area | Reinterpreted | Folds into crop |
| Crop window | **Direct** | Effect confined to a sub-rectangle; the rest presents untouched |
| GPU selection | Dropped | The chain runs on the device the game created |

### Output

| Feature | Verdict | Notes |
|---|---|---|
| Aspect ratio correction (DOS/NTSC, PAL, NES, SNES, custom) | **Direct** | Letterboxed inside the game's window |
| Output scale 100–1000 % | **Deferred** | It scaled ShaderGlass's own window. Whether a layer can magnify the *game's* window is a research item — see §8 |
| Flip horizontal / vertical | **Direct** | Final blit |
| Orientation horizontal / vertical (TATE) | **Direct** | Final blit |
| Free scale / lock scale | Dropped | Output-window sizing |
| Transparent / solid / borderless / click-through | Dropped | No window |
| Fullscreen | Dropped | The game owns that |
| Flip presentation, allow tearing | Dropped | Swapchain created before the layer sees it |
| HDR output | Reinterpreted | Detection, not a toggle: `DetectHdrKind` decides whether the chain works in PQ, float or 8-bit |

### Shader

| Feature | Verdict | Notes |
|---|---|---|
| ~3300 presets in 37 categories | **Direct** | Generated to SPIR-V |
| Parameters with min/max/step/default | **Direct** | Already reflected by ShaderGC |
| Preset parameter overrides | **Direct** | Already parsed |
| Preset textures (LUTs, bezels, overlays) | **Direct** | Needs an image loader in the layer |
| Multi-pass chains, scale types (source/viewport/absolute) | **Direct** | Core of the port |
| Feedback and history frames | **Direct** | Ring of previous outputs |
| Filter linear/nearest, wrap clamp/mirror/repeat, `frame_count_mod`, float/sRGB pass formats | **Direct** | Per-pass sampler and image state |
| Next / previous / random preset | **Direct** | GUI actions (no hotkeys) |
| Shader active toggle, quick toggle | **Direct** | Bypass flag in the header |
| Import a `.slangp` | **Direct** | Compiled in-process by the GUI (decision 7) |
| Browser window with previews | Reinterpreted | Searchable category tree with favourites; no thumbnails |
| Compile progress window | **Direct** | Progress for in-GUI imports |

### Processing and global

| Feature | Verdict | Notes |
|---|---|---|
| Start / stop | Reinterpreted | Enable/bypass; the layer is armed per-launch by env var |
| Pause | **Direct** | Freeze the chain, keep presenting |
| Frame skip 1/2…1/20 | **Direct** | Run every Nth present, reuse the last result |
| Screenshot | **Direct** | `CaptureWriter` already writes matched before/after pairs |
| Save / load profile, recent profiles | **Direct** | The GUI already has profiles |
| Set as default / remove default | **Direct** | |
| Remember window position | Dropped | No window |
| Remember FPS | **Direct** | Status line |
| Global hotkeys | **Dropped** | Decision 6 |
| Toggle menu, tray behaviour | Dropped | No overlay |
| FPS counter | **Direct** | Status line |
| Renderer selection (D3D11) | Dropped | Vulkan only |
| Max capture frame rate | Dropped | No capture loop |
| Help: README / FAQ / version | **Direct** | |

**Tally:** 25 Direct · 7 Reinterpreted · 19 Dropped · 1 Deferred. Nothing in the shader pipeline is
lost; what goes is capture and window management, which is exactly what the layer replaces.

---

## 5. New code

1. **Pass executor** (`pipeline.{h,cpp}`) — render passes or dynamic rendering, framebuffers, a quad
   vertex buffer, per-pass colour targets, and the libretro uniform contract (`MVP`, `SourceSize`,
   `OutputSize`, `OriginalSize`, `FrameCount`, `FrameDirection`) written into both a UBO and a push
   constant block, matching what ShaderGC reflects.
2. **Source-resolution stage** — an area-average downsample of the swapchain image to the chosen
   raster before pass 0, the counterpart of ShaderGlass's preprocess pass.
3. **Preset catalogue** — an index over the generated library: name, category, pass count, parameters.
4. **Texture loader** for preset LUTs and overlays.
5. **Qt preset tree and parameter panel**, plus the crop-on-screenshot widget.
6. **`shaderglass-gen`** — ShaderGen rewritten to emit SPIR-V headers.

---

## 6. Phasing

| Phase | Deliverable |
|---|---|
| 1 | Tree, Meson + CMake, manifest, protocol header, layer that loads and passes frames through untouched; `shaderglass-ctl` |
| 2 | **Done.** Pass executor with a built-in passthrough: swapchain → source raster → one pass → swapchain, verified bit-exact and validation-clean |
| 3 | **Done.** ShaderGC ported to Linux emitting SPIR-V; `shaderglass-gen` producing the catalogue; the preset library builds and loads through `dlopen` |
| 4 | **Done.** Full pass model — multi-pass, scale types, feedback/history, textures, per-pass formats. `tests/chain_test.cpp` builds and records every preset in the catalogue with no game and no window |
| 5 | Qt GUI — preset tree, parameters, profiles, status, screenshot view |
| 6 | Source resolution, pixel size and output policy, aspect ratio, crop, flip/rotate, frame skip, pause, idle repaint, the gamescope launch helper |
| 7 | Packaging, docs, `RELICENSE.md`, PR preparation. Every package's build runs `tools/build-catalogue.sh`, which is what guarantees the shader patches reach an installed catalogue |

---

## 7. Risks and open items

- **Preset library size — measured, and settled by content sharing.** The first full-catalogue run
  emitted every pass's SPIR-V inline in the preset that used it, and produced 5.4 GB before it was
  killed. Measured over the whole tree, that model would reach **23.3 GiB**: 3328 presets make
  52,725 pass references, but those resolve to only **1166 unique shaders**, and 19,927 texture
  references to **244 unique textures** — the Mega Bezel family alone is hundreds of variants over
  one set of passes.

  The catalogue therefore shares by content, which is the model the Windows app already uses (one
  `<Shader>ShaderDef.h` per unique shader, referenced by every preset that uses it). Each unique
  shader and texture is emitted once under a content-addressed symbol and referenced by `extern`
  declaration; `SgPass` and `SgTexture` hold pointers rather than payloads, which is what ABI
  version 2 is. Result:

  | | inline per preset | shared by content |
  |---|---|---|
  | generated C | 23,281 MiB | **231 MiB** |

  A 99.0 % saving, and close to upstream ShaderGlass's own 229 MB DXBC catalogue. On disk the
  generated tree is 269 MB over 4741 files, compiling in 5.6 s wall (32-way) to a **134 MB**
  `libShaderGlassPresets.so`. It is `dlopen`ed rather than linked into the layer, so game processes
  pay for it only when a preset is selected. Splitting it into its own package stays available.
- **A driver's shader compiler can segfault, and it takes the game with it.** `crt/simple-crt` and
  `crt/simple-crt-fxaa` did: `vkCreateGraphicsPipelines` crashed inside `libnvidia-gpucomp` on
  driver 615.71.09, on SPIR-V that `spirv-val` accepts and whose stage interfaces match exactly.
  Reproduced in a 130-line program with none of this layer's code involved.

  Bisected to `precision mediump float;` — a GLSL ES qualifier in a `#version 450` shader, which
  glslang turns into `RelaxedPrecision` decorations. Removing them from the **vertex** stage alone
  avoids the crash; removing them from the fragment stage does not. So the generator now strips
  them from every vertex stage (`gen/spirv_edit.cpp`): `RelaxedPrecision` is a hint that full
  precision always satisfies, and a fullscreen-quad pass runs its vertex shader over four vertices
  a frame, where relaxing precision cannot buy anything measurable. The fragment stage, where it
  might, keeps its decorations. Across the whole tree exactly three shaders change — the
  `simple-crt` family, the only vertex stages carrying the decoration — and both presets now run.

  This is a workaround for one driver bug, not a guarantee against the next one. Nothing here can
  catch a SIGSEGV inside a driver; `chain_test --isolate` is what finds them.
- **Fifteen upstream shaders declare a fragment input their vertex stage never writes**, so its
  value is undefined and the Vulkan runtime reports `VUID-RuntimeSpirv-OpEntryPoint-08743` when the
  pipeline is built. `2xsal`, `crt-hyllian-pass0`, `crt-gdv-mini` and the rest are authored that way
  upstream. `shaderglass-gen` now warns for each rather than letting it first appear inside a game.
- **Shaders authored against a low-res source** will look wrong at Native until the source resolution
  is set. Documented, not solvable.
- **`VK_FORMAT` coverage.** Presets request float and sRGB pass formats; devices that cannot render
  to one must fall back per pass rather than fail the chain.

---

## 8. Magnifying the game's own window

ShaderGlass's output scale blew up its *own* window. The equivalent question here is whether a layer
can blow up the **game's** window: a 480p ScummVM window filling a 4K screen with chunky pixels. This
is deliberately scheduled after the port, but the groundwork is worth recording now.

**What the layer can see.** Hooking `vkCreateXlibSurfaceKHR` / `vkCreateXcbSurfaceKHR` /
`vkCreateWaylandSurfaceKHR` gives the layer the native window handle out of the create-info, so it
knows which window a swapchain belongs to. That part is free.

**Enlarging the window does not magnify anything.** On X11/XWayland the layer could call
`XResizeWindow`; the app's next present returns `VK_SUBOPTIMAL_KHR` and a well-behaved app recreates
its swapchain larger. But the app then *renders* at the larger size — exactly what happens when the
user drags the window edge. Nothing is magnified, and the window manager already offered that.

**The promising path is the inverse: let the app render small and have the presentation engine
scale.** `VK_EXT_swapchain_maintenance1` defines `VkSwapchainPresentScalingCreateInfoEXT`, which
declares what the presentation engine does when the swapchain extent differs from the surface
extent — one-to-one, aspect-preserving stretch, or stretch — plus gravity for the leftover. A layer
can inject that structure into the app's `vkCreateSwapchainKHR` `pNext` chain while reducing the
requested extent, which is a spec-sanctioned magnifier rather than a hack. The layer already enables
this extension for the idle repaint, so the plumbing exists.

**What has to be established before building it:**

- How many drivers and compositors actually implement the scaling modes, versus advertising the
  extension for its other features.
- App compatibility. Some applications clamp to `currentExtent`, some query surface capabilities once
  and cache, some derive their internal resolution from the swapchain extent and would render their
  UI at the reduced size too.
- Filtering. The presentation engine picks the filter; if it is bilinear, a nearest-neighbour
  magnifier is not achievable this way and the shader chain's own scaling is the better answer.
- The fallback: spoofing `currentExtent` in `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` achieves a
  similar effect without the extension, with more compatibility risk and no defined filter.

**Verdict on the in-layer path.** Research item, X11/XWayland first, opt-in per game, never default.

### The guaranteed fallback: gamescope

The in-layer path may not survive contact with real drivers. gamescope will, because it is a
compositor whose entire job is this: run a client at one resolution and scale it to another with a
chosen filter. Verified against gamescope 3.16.25:

```
gamescope -w 640 -h 480 -W 3840 -H 2160 -S integer -F nearest -f -- env SHADERGLASS=1 scummvm
          └─ what the game renders ─┘ └─ the display ─┘
```

Its `--scaler` options are **auto, integer, fit, fill, stretch** — the same set as the output
policies in §2, which is not a coincidence so much as both of us solving the same problem:

| Output policy | gamescope |
|---|---|
| Auto | `-S auto` |
| Stretch to fill | `-S stretch` |
| Fit | `-S fit` |
| Fill | `-S fill` |
| Integer + letterbox | `-S integer` |
| Centre 1:1 | `-S integer -m 1` (cap the scale factor at one) |

`-F nearest` or `-F pixel` gives the hard pixel edges that pixel art wants; `linear`, `fsr` and `nis`
are there for everything else.

**The bonus is bigger than the fallback.** `-w/-h` sets what the game *actually renders at*, not what
we downsample afterwards. For a game that honours its window size, `gamescope -w 320 -h 240` gives
the shader chain a real 320×240 source — sharper than downsampling a 4K frame, and far cheaper,
because every pass runs at a fraction of the pixels. For fullscreen games that pick their own
resolution regardless, the layer's own source-resolution setting remains the answer.

**Two configurations, two different pictures.** Worth documenting for users, because the difference
is not subtle:

- **gamescope magnifies.** Game renders at 640×480, the chain runs there too, gamescope integer-scales
  the shaded result to the display. Cheapest by far — but the shader's own output is magnified with
  everything else, so a one-pixel scanline becomes a four-pixel bar.
- **the layer magnifies.** gamescope runs at display resolution (or is not used), the layer's source
  resolution downsamples to 320×240, and the chain targets the full swapchain. Scanlines are drawn at
  display resolution and stay one pixel thin. More expensive, and usually what a CRT preset's author
  intended.

**Delivery.** The GUI cannot launch the game — Steam or the user does — so this is a launch-command
helper, not a runtime feature: detect gamescope with `command -v`, detect whether we are *already*
inside it (`GAMESCOPE_WAYLAND_DISPLAY`), and generate the exact command from the current settings for
the user to paste into Steam's launch options. A `shaderglass-run` wrapper script ships alongside it
for people launching from a terminal or a .desktop file.

**Layer ordering.** gamescope installs its own implicit layer, `VK_LAYER_FROG_gamescope_wsi_x86_64`,
and this machine also carries MangoHud, `obs_vkcapture` and the NVIDIA layers. Ours has to be pinned
relative to them through `VK_INSTANCE_LAYERS`, exactly as DLSS5VKLayer's `environment.d` snippet pins
itself against `VK_LAYER_NV_present`. Note also that the gamescope layer uses an `_x86_64` name
suffix — the same per-architecture trick as decision 3, independently arrived at.
