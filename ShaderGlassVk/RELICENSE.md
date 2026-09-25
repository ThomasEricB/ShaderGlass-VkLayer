# Relicensing record

`ShaderGlassVk` reuses code from [DLSS5VKLayer](https://github.com/bmitch87/DLSS5VKLayer), which is
published under **AGPL-3.0**. ShaderGlass is **GPL-3.0**.

GPLv3 §13 permits combining the two into one work, but it attaches the AGPL's network-use obligation
to the combination — which would mean a GPL-3.0 project carrying AGPL obligations in part of its
tree. Rather than do that, the reused code is **relicensed to GPL-3.0** for this use.

## What is reused

| File | Origin |
|---|---|
| `common/shm_protocol.h` | DLSS5VKLayer `common/shm_protocol.h` — the atomics-in-a-mapping mechanism, the sequence-guarded strings, the settings/status split, `ShmResetSettings`. Every DLSS-specific field is replaced |
| `layer/src/layer.cpp` | DLSS5VKLayer `layer_linux/src/layer.cpp` — dispatch chains, the hook set, primary-swapchain selection, loader-data handling, the duplicate-copy guard, per-architecture naming, fail-open discipline |
| `layer/src/vk_table.h` | DLSS5VKLayer `layer_linux/src/vk_table.h` |
| `layer/src/log.h` | DLSS5VKLayer `layer_linux/src/log.h` |
| `layer/shaderglass.map` | DLSS5VKLayer `layer_linux/dlssnr.map` |
| `tools/shmctl.cpp` | DLSS5VKLayer `tools/shmctl.cpp` |
| `gui/shm_binder.{h,cpp}` | DLSS5VKLayer `gui/` — the widget-to-mapping binder, its latching of settings that force a rebuild, and profile blobs |
| `gui/mainwindow.{h,cpp}` | DLSS5VKLayer's settings window, in shape: the tabbed layout, the status line and the profile row. The tabs and what is on them are new |
| `meson/*.ini`, `tools/build.sh`, `packaging/` | DLSS5VKLayer — the cross files, and the release layout: a staged tree shared by tarball, RPM and DEB, `install.sh --user/--system`, one manifest per architecture |

Planned but not reused in the end: `shader_vk.{h,cpp}` and `capture.{h,cpp}`. The chain builds its
own graphics pipelines, and captures are written by the chain itself (`layer/src/chain.cpp`).

Not reused, and deliberately absent: everything NVIDIA-specific. The NGX ABI, parameter block, SEH
guard and snippet lifecycle (`core/`), the Windows helper process (`helper/`), optical flow, the
dma-buf frame transport, Wine/Proton runner discovery and prefix management. None of it has a
counterpart here — the shader chain runs on the game's own device, so there is no second process and
no frame ever leaves it.

## Permission

DLSS5VKLayer's `ATTRIBUTION.md` identifies its rights holders as **bmitch87** (the base project) and
**Thomas Eric** (the layer, shared-memory protocol, GUI, capture and frame-hold path, scaling,
packaging and subsequent work).

- Thomas Eric is a rights holder in the reused material and contributes it here under GPL-3.0.
- bmitch87's permission to relicense has been obtained.

Anyone preparing a distribution or an upstream pull request should attach or link the written
permission alongside this file, so the provenance stands on record rather than on assertion.

## Attribution

Relicensing does not remove attribution. Every file carrying reused code says so in its header, and
the table above is the index. Upstream authorship of the original DLSS5VKLayer work remains with its
authors; the GPL-3.0 grant covers its use in this tree.
