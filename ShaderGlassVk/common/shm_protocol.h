/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

The shared-memory contract between the two processes that make up ShaderGlassVk:

  the Vulkan layer   inside the game; captures the frame, runs the shader chain, presents the result
  the Qt interface   writes settings and reads status

Everything here is plain atomics in a file mapping, so neither side needs the other's toolchain and a
process dying leaves the other reading a consistent -- if stale -- picture. Derived from
DLSS5VKLayer's protocol (relicensed to GPL-3.0, see RELICENSE.md), with one large simplification: no
pixels ever cross. DLSS5VKLayer had to ship a frame to a Windows helper and wait for it to come back,
so its mapping carried two full-size pixel regions. The shader chain runs on the game's own device,
so this mapping is the header and nothing else.

Every field is 32 bits wide, and 64-bit counters are split into lo/hi pairs, so the layout is
identical in the 32- and 64-bit layer builds that both attach to the same file.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace shaderglass {

// 'SGL1'.
static constexpr uint32_t kShmMagic = 0x314C4753;

// Bumped whenever the layout below changes. A mapping of any other version is re-initialised rather
// than half-read: a field inserted anywhere but the end moves everything after it, and a build that
// has not caught up then reads its neighbour's value.
static constexpr uint32_t kShmVersion = 3;

// How often the source detector re-measures, when it is on. Long enough that the readback and the
// analysis are lost in the noise of a frame, short enough to catch a game changing its raster at a
// scene or resolution change.
static constexpr uint32_t kAutoSourceDefaultMs = 2000;

// The whole mapping. No pixel regions -- see the file comment.
static constexpr size_t kHeaderBytes = 65536;

// A preset can declare a lot of parameters; crt-royale alone runs to dozens. The block is fixed so
// the layout stays pinned, and the count says how many entries mean anything.
static constexpr uint32_t kMaxParams = 128;
static constexpr size_t kParamNameBytes = 64;

static constexpr size_t kPresetIdBytes = 256;
static constexpr size_t kReasonBytes = 192;
static constexpr size_t kNameBytes = 128;

// The floor below which a swapchain is not worth shading. A game that presents a 1x1 probe swapchain
// -- some do, behind their real window -- would otherwise have a chain built at that size.
static constexpr uint32_t kMinW = 64, kMinH = 64;
static constexpr uint32_t kMaxW = 15360, kMaxH = 8640;

// What raster the shader chain is shown, before it scales back up. This is the reinterpretation of
// ShaderGlass's pixel-size menu: the game already renders at output resolution, so a CRT preset needs
// to be told what the real pixel grid is. See DESIGN.md section 2.
enum SourceMode : uint32_t {
    kSourceNative = 0,   // 1:1 with the swapchain; modern games stay untouched
    kSourceDivisor = 1,  // swapchain / sourceDivisor, the x2..x8 case
    kSourceRaster = 2,   // one of the classic rasters named by sourceWidth/sourceHeight
    kSourceCustom = 3,   // an explicit sourceWidth x sourceHeight
};

// What happens when the chain's output raster does not match the swapchain, which it only can once
// pixel size is set to something other than Auto.
enum OutputPolicy : uint32_t {
    kOutputAuto = 0,     // the chain targets the swapchain; nothing to reconcile
    kOutputStretch = 1,  // scale to the swapchain, ignoring aspect
    kOutputFit = 2,      // preserve aspect, largest that fits, letterbox the rest
    kOutputFill = 3,     // preserve aspect, cover the swapchain, crop the overflow
    kOutputInteger = 4,  // largest whole multiple that fits, centred
    kOutputCentre = 5,   // placed at its own size in the middle -- a true magnifier
    kOutputPolicyCount = 6,
};

enum Rotation : uint32_t {
    kRotate0 = 0,
    kRotate90 = 1,
    kRotate180 = 2,
    kRotate270 = 3,
};

// What the layer has managed to do. A freshly initialised header must read as "no layer", so that is
// the zero value.
enum LayerState : uint32_t {
    kLayerDetached = 0,  // nothing is attached to this mapping
    kLayerIdle = 1,      // attached, but not composing right now (paused, occluded, no preset)
    kLayerActive = 2,    // composing frames
    kLayerFailed = 3,    // attached and unable to run the chain; see layerReason
};

// Where the mapping lives.
//
// Not $XDG_RUNTIME_DIR. A Steam game runs inside pressure-vessel, which gives the container a private
// tmpfs there, so a mapping put in it is simply absent inside the game: the layer creates its own
// empty one at a path that reads identically in the log and the interface talks to the other file.
// /tmp is bind-mounted from the host into the container, so both sides land on one file.
inline std::string ShmRuntimeDir() {
    const char* uid = std::getenv("SHADERGLASS_UID");
    if (uid && *uid) return std::string("/tmp/shaderglass-") + uid;
#ifdef _WIN32
    return "/tmp/shaderglass";
#else
    return "/tmp/shaderglass-" + std::to_string((unsigned) getuid());
#endif
}

inline std::string ShmDefaultPath() { return ShmRuntimeDir() + "/shm.bin"; }

inline size_t ShmTotalBytes() { return kHeaderBytes; }

// Where the layer writes captured frames and the interface looks for them. Beside the mapping, for
// the same reason the mapping is where it is: both sides have to agree on a path that exists on
// both sides of a container boundary.
inline std::string ShmCaptureDir() { return ShmRuntimeDir() + "/captures"; }

// One shader parameter, as the interface publishes it and the layer reads it.
//
// The name is written when the preset changes and is guarded by presetSeq rather than by a sequence
// of its own: a reader that has seen a stable presetSeq either side of its read is looking at one
// preset's parameter block.
struct ShmParam {
    char name[kParamNameBytes];
    std::atomic<uint32_t> valueBits;
};

// A float is carried as its bits because std::atomic<float> has no lock-free guarantee across the
// ABIs this mapping is shared over, while a 32-bit integer does. memcpy rather than a cast: type
// punning through a pointer is undefined, and every compiler folds this away.
inline uint32_t ShmFloatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float ShmBitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void ShmStoreParam(ShmParam& param, float value) {
    param.valueBits.store(ShmFloatBits(value), std::memory_order_relaxed);
}

inline float ShmLoadParam(const ShmParam& param) {
    return ShmBitsFloat(param.valueBits.load(std::memory_order_relaxed));
}

struct ShmHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;

    // Bumped by whoever writes a setting, so the layer watches one number instead of re-reading
    // thirty every frame.
    std::atomic<uint32_t> controlSeq;

    // Bumped only when something that forces the chain to be rebuilt changes -- the preset, the
    // source raster, the pixel size. Rebuilding is expensive and a parameter tweak must not trigger
    // one, which is the distinction this field exists to draw.
    std::atomic<uint32_t> tuningSeq;

    // --- what to run -------------------------------------------------------------------------
    // Run the chain at all. Off presents the game's own frame, at the cost of the present hook and
    // nothing else.
    std::atomic<uint32_t> enabled;

    // Freeze the chain on the frame it last composed and keep presenting that, so a parameter can be
    // tuned against a still picture.
    std::atomic<uint32_t> paused;

    // Which preset, by catalogue id (its path within the generated catalogue, or a user import's
    // name). Empty means no preset, which is the default and presents untouched frames.
    std::atomic<uint32_t> presetSeq;
    char presetId[kPresetIdBytes];

    // --- the source raster -------------------------------------------------------------------
    std::atomic<uint32_t> sourceMode;     // SourceMode
    std::atomic<uint32_t> sourceDivisor;  // for kSourceDivisor; 2..8
    std::atomic<uint32_t> sourceWidth;    // for kSourceRaster / kSourceCustom
    std::atomic<uint32_t> sourceHeight;

    // --- the output raster -------------------------------------------------------------------
    // How many swapchain pixels one source pixel occupies. 0 means Auto: the chain targets the whole
    // swapchain and the setting is invisible.
    std::atomic<uint32_t> pixelSizeBits;
    std::atomic<uint32_t> outputPolicy;  // OutputPolicy

    // Pixel-aspect correction, applied by letterboxing inside the game's own window. 1.0 is none.
    std::atomic<uint32_t> aspectRatioBits;

    std::atomic<uint32_t> flipHorizontal;
    std::atomic<uint32_t> flipVertical;
    std::atomic<uint32_t> rotation;  // Rotation

    // Confine the effect to a sub-rectangle; the rest of the frame presents untouched. In swapchain
    // pixels, so the interface's screenshot and these numbers agree.
    std::atomic<uint32_t> cropEnabled;
    std::atomic<uint32_t> cropX;
    std::atomic<uint32_t> cropY;
    std::atomic<uint32_t> cropWidth;
    std::atomic<uint32_t> cropHeight;

    // --- cost --------------------------------------------------------------------------------
    // Run the chain every Nth present and re-present the last result in between. 0 and 1 both mean
    // every frame.
    std::atomic<uint32_t> frameSkip;

    // --- requests ----------------------------------------------------------------------------
    // Write a frame count here and the layer writes that many matched before/after pairs, then
    // clears it. The interface reads those back as its still preview.
    std::atomic<uint32_t> captureRequest;

    // --- status, written by the layer --------------------------------------------------------
    // The pid of the process the layer is loaded into, or 0 when nothing is attached.
    //
    // A pid rather than a flag, because a flag cannot survive the process that set it: nothing clears
    // this when a game crashes, so a reader checks the pid is still alive rather than trusting the
    // value. It is also what tells "paused, nothing to draw" apart from "no game running", which a
    // frame counter alone cannot do.
    std::atomic<uint32_t> layerPid;
    std::atomic<uint32_t> layerState;  // LayerState
    std::atomic<uint32_t> layerFramesLo;
    std::atomic<uint32_t> layerFramesHi;
    std::atomic<uint32_t> layerHeartbeat;

    // The swapchain as the game created it, and the two rasters the chain resolved from it.
    std::atomic<uint32_t> swapWidth;
    std::atomic<uint32_t> swapHeight;
    std::atomic<uint32_t> swapFormat;
    std::atomic<uint32_t> sourceActualWidth;
    std::atomic<uint32_t> sourceActualHeight;
    std::atomic<uint32_t> outputActualWidth;
    std::atomic<uint32_t> outputActualHeight;

    // How many passes the built chain actually runs, and how long one frame's chain took.
    std::atomic<uint32_t> passCount;
    std::atomic<uint32_t> layerMsBits;
    std::atomic<uint32_t> fpsBits;

    // Free text, each guarded by its own sequence number: bumped after the bytes are written, so a
    // reader that sees an unchanged number is looking at a whole string.
    std::atomic<uint32_t> layerReasonSeq;
    char layerReason[kReasonBytes];
    std::atomic<uint32_t> gameNameSeq;
    char gameName[kNameBytes];

    // --- parameters --------------------------------------------------------------------------
    // Appended last on purpose: everything above has a pinned offset, and a field inserted higher up
    // would move all of them.
    std::atomic<uint32_t> paramCount;
    ShmParam params[kMaxParams];

    // --- source auto-detection ----------------------------------------------------------------
    // After params for the reason params came after everything else: every offset above is pinned by
    // a static_assert, and appending here moves none of them.
    //
    // A pixel-art game upscaled by a fraction cannot be expressed by the x2..x8 divisor list, and
    // guessing the nearest integer is worse than not guessing. When this is on, the layer measures
    // the game's own raster out of the frame and uses that instead. See layer/src/pixel_grid.h.
    std::atomic<uint32_t> autoSourceEnabled;     // 0/1
    std::atomic<uint32_t> autoSourceIntervalMs;  // between measurements; 0 means kAutoSourceDefaultMs

    // Written by the layer, read by the interface: what the last measurement found. Width and height
    // are zero until something has been measured, which is also how "on but nothing found yet" is
    // told apart from "found a raster".
    std::atomic<uint32_t> autoSourceWidth;
    std::atomic<uint32_t> autoSourceHeight;
    std::atomic<uint32_t> autoSourceScaleXBits;      // float: swapchain pixels per source pixel
    std::atomic<uint32_t> autoSourceScaleYBits;      // float
    std::atomic<uint32_t> autoSourceConfidenceBits;  // float 0..1
    std::atomic<uint32_t> autoSourceSeq;             // bumped on each new measurement

    // Bumped by the interface to demand a fresh measurement: on the toggle changing, and on the
    // button. A request rather than the layer inferring an edge, because the layer cannot see one it
    // was not running for -- a toggle flipped while no game is attached, or flipped twice between
    // two of its frames, leaves no edge behind, and the user who pressed it is still owed a new
    // measurement. The layer keeps the last value it acted on and compares.
    std::atomic<uint32_t> autoSourceRefresh;
};

static_assert(sizeof(ShmHeader) <= kHeaderBytes, "ShmHeader outgrew its region");

// The layout, pinned.
//
// Every process that maps this file agrees on where each field is only because they were compiled
// from the same header. If these fire, the layout changed: bump kShmVersion in the same commit, then
// update these numbers.
// Verified identical in the 64-bit and 32-bit builds, which both attach to the same file.
static_assert(sizeof(ShmHeader) == 9484, "the header layout changed -- bump kShmVersion");
static_assert(sizeof(ShmParam) == 68, "the parameter block changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, enabled) == 16, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, sourceMode) == 284, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, layerPid) == 352, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, paramCount) == 740, "layout changed -- bump kShmVersion");

inline uint32_t FloatToBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float BitsToFloat(uint32_t u) {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// The field is cleared first, so the copy below can never leave it unterminated: at most cap-1 bytes
// are written and the last one is already zero. Written as an explicit length rather than strncpy
// because strncpy's own contract does not promise that, and a reader should not have to reason about
// the memset to know this is safe.
inline void ShmStoreString(std::atomic<uint32_t>& seq, char* dst, size_t cap, const char* src) {
    std::memset(dst, 0, cap);
    if (src && cap > 1) {
        const size_t n = std::strlen(src);
        std::memcpy(dst, src, n < cap - 1 ? n : cap - 1);
    }
    seq.fetch_add(1);
}

// Read a string guarded by its sequence number, retrying while it moves under us.
inline std::string ShmLoadString(const std::atomic<uint32_t>& seq, const char* src, size_t cap) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = seq.load();
        char buf[kPresetIdBytes];
        const size_t n = cap < sizeof(buf) ? cap : sizeof(buf);
        std::memset(buf, 0, sizeof(buf));
        std::memcpy(buf, src, n);
        buf[n - 1] = '\0';
        if (seq.load() == before) return std::string(buf);
    }
    return std::string();
}

inline void ShmInitDefaults(ShmHeader* h) {
    std::memset(static_cast<void*>(h), 0, sizeof(ShmHeader));
    h->magic.store(kShmMagic);
    h->version.store(kShmVersion);

    // Enabled, but with no preset -- so a game launched with the layer armed presents untouched
    // frames until someone picks a shader. A layer that silently applied a CRT filter to every Vulkan
    // game on the system would be hostile.
    h->enabled.store(1);
    h->sourceMode.store(kSourceNative);
    h->sourceDivisor.store(2);
    h->pixelSizeBits.store(0);  // Auto
    h->outputPolicy.store(kOutputAuto);
    h->aspectRatioBits.store(FloatToBits(1.0f));
    h->rotation.store(kRotate0);
    h->frameSkip.store(0);
    // Off by default. A layer that silently changed a game's source raster because it thought it saw
    // a pixel grid would be the same kind of hostile as one that applied a filter unasked.
    h->autoSourceEnabled.store(0);
    h->autoSourceIntervalMs.store(0);  // 0 means kAutoSourceDefaultMs
    h->layerState.store(kLayerDetached);
}

// Put the settings back to their defaults and leave the status alone.
//
// Distinct from ShmInitDefaults, which clears the whole header: appropriate when a mapping is being
// created, and destructive when it is not. Resetting settings on a live session through that path
// takes the layer's published presence down with them, and the layer republishes only when it next
// composes a frame -- so a paused game would read as nothing attached until it drew again.
inline void ShmResetSettings(ShmHeader* h) {
    if (!h) return;
    struct Saved {
        uint32_t controlSeq, tuningSeq;
        uint32_t layerPid, layerState, layerFramesLo, layerFramesHi, layerHeartbeat;
        uint32_t swapWidth, swapHeight, swapFormat;
        uint32_t sourceActualWidth, sourceActualHeight, outputActualWidth, outputActualHeight;
        uint32_t passCount, layerMsBits, fpsBits;
        uint32_t layerReasonSeq, gameNameSeq;
    } v;
#define SG_SAVE(f) v.f = h->f.load()
    SG_SAVE(controlSeq); SG_SAVE(tuningSeq);
    SG_SAVE(layerPid); SG_SAVE(layerState); SG_SAVE(layerFramesLo); SG_SAVE(layerFramesHi);
    SG_SAVE(layerHeartbeat);
    SG_SAVE(swapWidth); SG_SAVE(swapHeight); SG_SAVE(swapFormat);
    SG_SAVE(sourceActualWidth); SG_SAVE(sourceActualHeight);
    SG_SAVE(outputActualWidth); SG_SAVE(outputActualHeight);
    SG_SAVE(passCount); SG_SAVE(layerMsBits); SG_SAVE(fpsBits);
    SG_SAVE(layerReasonSeq); SG_SAVE(gameNameSeq);
#undef SG_SAVE
    char layerReason[kReasonBytes], gameName[kNameBytes];
    std::memcpy(layerReason, h->layerReason, sizeof(layerReason));
    std::memcpy(gameName, h->gameName, sizeof(gameName));

    ShmInitDefaults(h);

#define SG_LOAD(f) h->f.store(v.f)
    SG_LOAD(layerPid); SG_LOAD(layerState); SG_LOAD(layerFramesLo); SG_LOAD(layerFramesHi);
    SG_LOAD(layerHeartbeat);
    SG_LOAD(swapWidth); SG_LOAD(swapHeight); SG_LOAD(swapFormat);
    SG_LOAD(sourceActualWidth); SG_LOAD(sourceActualHeight);
    SG_LOAD(outputActualWidth); SG_LOAD(outputActualHeight);
    SG_LOAD(passCount); SG_LOAD(layerMsBits); SG_LOAD(fpsBits);
    SG_LOAD(layerReasonSeq); SG_LOAD(gameNameSeq);
#undef SG_LOAD
    std::memcpy(h->layerReason, layerReason, sizeof(layerReason));
    std::memcpy(h->gameName, gameName, sizeof(gameName));

    // Announced last, so both readers see the settled values rather than a half-applied header.
    h->controlSeq.store(v.controlSeq + 1);
    h->tuningSeq.store(v.tuningSeq + 1);
}

inline bool ShmEnabled(const ShmHeader* h) { return h && h->enabled.load() != 0; }

inline std::string ShmPresetId(const ShmHeader* h) {
    return h ? ShmLoadString(h->presetSeq, h->presetId, kPresetIdBytes) : std::string();
}

// The raster the chain should be shown, resolved from the settings and the swapchain size. Kept here
// rather than in the layer so the interface can show the same number the layer will use.
inline void ShmSourceExtent(const ShmHeader* h, uint32_t swapW, uint32_t swapH,
                            uint32_t* outW, uint32_t* outH) {
    uint32_t w = swapW, hgt = swapH;
    // The detector wins over the mode rather than being a mode of its own, so turning it off puts
    // the user's own choice back rather than leaving them on a raster nobody picked.
    if (h && h->autoSourceEnabled.load()) {
        const uint32_t aw = h->autoSourceWidth.load();
        const uint32_t ah = h->autoSourceHeight.load();
        if (aw && ah) {
            *outW = aw < kMinW ? kMinW : (aw > kMaxW ? kMaxW : aw);
            *outH = ah < kMinH ? kMinH : (ah > kMaxH ? kMaxH : ah);
            return;
        }
        // On, but nothing measured yet: fall through to the mode until there is an answer.
    }
    if (h) {
        switch (h->sourceMode.load()) {
            case kSourceDivisor: {
                uint32_t d = h->sourceDivisor.load();
                if (d < 2) d = 2;
                if (d > 8) d = 8;
                w = swapW / d;
                hgt = swapH / d;
                break;
            }
            case kSourceRaster:
            case kSourceCustom: {
                const uint32_t cw = h->sourceWidth.load();
                const uint32_t ch = h->sourceHeight.load();
                if (cw && ch) { w = cw; hgt = ch; }
                break;
            }
            case kSourceNative:
            default:
                break;
        }
    }
    if (w < kMinW) w = kMinW;
    if (hgt < kMinH) hgt = kMinH;
    if (w > kMaxW) w = kMaxW;
    if (hgt > kMaxH) hgt = kMaxH;
    *outW = w;
    *outH = hgt;
}

inline uint64_t ShmLoad64(const std::atomic<uint32_t>& lo, const std::atomic<uint32_t>& hi) {
    return (uint64_t(hi.load()) << 32) | uint64_t(lo.load());
}

inline void ShmStore64(std::atomic<uint32_t>& lo, std::atomic<uint32_t>& hi, uint64_t v) {
    hi.store(uint32_t(v >> 32));
    lo.store(uint32_t(v & 0xFFFFFFFFu));
}

}  // namespace shaderglass
