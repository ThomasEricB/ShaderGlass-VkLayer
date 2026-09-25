/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Where the shaded picture goes: every output setting reduced to rectangles.

The chain used to have one geometry. It read the whole swapchain, and its last pass wrote the whole
swapchain back -- libretro's "viewport" and the swapchain were the same thing. Phase 6 pulls them
apart (DESIGN.md §2, decisions 16 and 17):

    swapchain -> target area T      the whole frame, or the crop rectangle
              -> source raster R    what the chain is shown
              -> output raster V    what its last pass writes: R x pixel size, aspect-corrected
              -> placed into T      by the output policy, turned, mirrored, letterboxed

Everything here is arithmetic on rectangles, with no Vulkan in it, so every policy can be tested
against numbers worked out by hand before a pixel is drawn. The chain executes the result with
blits, plus one built-in pass for a quarter turn, which a blit cannot do.

Decisions DESIGN.md leaves open, made here and not scattered:

  * Auto with no aspect correction fills T, which is what the layer always did. Auto with a
    correction letterboxes to the corrected shape: "letterboxed inside the game's window" is the
    whole point of the setting, and filling T would undo it.
  * Auto with an explicit pixel size honours the size -- centred at one to one, or fitted when too
    big. The design says both "Auto means V is the swapchain" and "an explicit pixel size makes V
    differ from it"; the second is the one somebody chose, so it wins.
  * Integer falls back to Fit when not even one whole multiple fits, rather than clipping the
    picture: a too-small window should shrink it, not cut it.
  * Half a turn is both mirrors, and three quarters is one quarter plus both mirrors. So the only
    rotation that ever needs a render pass is a single quarter turn.
*/

#pragma once

#include <cstdint>

namespace shaderglass {

struct IRect {
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;

    bool Empty() const { return w == 0 || h == 0; }
    bool operator==(const IRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct PlacementInput {
    uint32_t swapW = 0, swapH = 0;

    bool cropEnabled = false;
    IRect crop;  // in swapchain pixels; clamped to the swapchain, ignored when empty

    uint32_t sourceW = 0, sourceH = 0;  // R, already resolved against the target area

    float pixelSize = 0.0f;     // screen pixels per source pixel; 0 is Auto
    uint32_t policy = 0;        // OutputPolicy
    // Height of a source pixel relative to its width; 1 is none. The Windows app's convention, which
    // its presets are written in -- x1.2 for DOS and NTSC, x2.0 for double tall, x0.5 for double
    // wide -- and which its own dialog calls "Aspect Ratio Correction (Pixel Height)".
    float pixelHeight = 1.0f;

    bool flipH = false, flipV = false;
    uint32_t rotation = 0;  // quarter turns clockwise, 0..3
};

struct Placement {
    IRect target;           // the part of the swapchain the effect owns
    uint32_t viewW = 0;     // V, before any turn: what the chain's last pass targets
    uint32_t viewH = 0;
    bool quarterTurn = false;  // V is turned a quarter before placing; its shape is then viewH x viewW

    IRect src;  // the part of the (turned) V that is shown -- all of it unless Fill or Centre crop
    IRect dst;  // where that part lands in the swapchain, always inside target

    IRect bars[4];  // the rest of target, which is cleared to black
    uint32_t barCount = 0;

    bool flipH = false, flipV = false;  // after half turns have been folded in
    bool nearest = false;               // the scale is whole on both axes, so pixels stay hard

    bool Valid() const { return !target.Empty() && viewW && viewH && !src.Empty() && !dst.Empty(); }
};

Placement ComputePlacement(const PlacementInput& in);

// The target area alone, for the caller that has to resolve the source raster against it before the
// rest of the placement can be worked out.
IRect TargetArea(uint32_t swapW, uint32_t swapH, bool cropEnabled, const IRect& crop);

}  // namespace shaderglass
