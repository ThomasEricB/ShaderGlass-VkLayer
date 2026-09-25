/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Output placement, checked against numbers worked out by hand.

The screen is a 2560x1080 ultrawide and the raster a SNES's 320x224, because that is where these
policies actually differ: the two aspects are far apart, so fitting, filling and stretching give three
visibly different pictures, and a mistake in any of them shows as a wrong number rather than hiding.

Every case also has to satisfy the invariants no hand calculation should need restating: the picture
lands inside the target, and the picture plus the bars cover the target exactly, with no overlap and
no gap -- a gap would present stale pixels, and an overlap would clear part of the picture.
*/

#include "../layer/src/placement.h"
#include "../common/shm_protocol.h"

#include <cstdio>

using namespace shaderglass;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

bool Inside(const IRect& in, const IRect& r) {
    return r.x >= in.x && r.y >= in.y && r.x + int64_t(r.w) <= in.x + int64_t(in.w) &&
           r.y + int64_t(r.h) <= in.y + int64_t(in.h);
}

bool Overlap(const IRect& a, const IRect& b) {
    return a.x < b.x + int64_t(b.w) && b.x < a.x + int64_t(a.w) && a.y < b.y + int64_t(b.h) &&
           b.y < a.y + int64_t(a.h);
}

void Invariants(const char* name, const Placement& p) {
    char what[160];
    const uint32_t vw = p.quarterTurn ? p.viewH : p.viewW;
    const uint32_t vh = p.quarterTurn ? p.viewW : p.viewH;

    std::snprintf(what, sizeof what, "%s: valid", name);
    Check(p.Valid(), what);
    std::snprintf(what, sizeof what, "%s: picture inside the target", name);
    Check(Inside(p.target, p.dst), what);
    std::snprintf(what, sizeof what, "%s: shown part inside V", name);
    Check(Inside({0, 0, vw, vh}, p.src), what);

    uint64_t area = uint64_t(p.dst.w) * p.dst.h;
    bool clean = true;
    for (uint32_t i = 0; i < p.barCount; ++i) {
        area += uint64_t(p.bars[i].w) * p.bars[i].h;
        if (!Inside(p.target, p.bars[i]) || Overlap(p.bars[i], p.dst)) clean = false;
        for (uint32_t j = i + 1; j < p.barCount; ++j)
            if (Overlap(p.bars[i], p.bars[j])) clean = false;
    }
    std::snprintf(what, sizeof what, "%s: bars neither overlap nor escape", name);
    Check(clean, what);
    std::snprintf(what, sizeof what, "%s: picture and bars tile the target exactly", name);
    Check(area == uint64_t(p.target.w) * p.target.h, what);
}

PlacementInput Base() {
    PlacementInput in;
    in.swapW = 2560;
    in.swapH = 1080;
    in.sourceW = 320;
    in.sourceH = 224;
    return in;
}

void Expect(const char* name, const Placement& p, IRect dst, uint32_t viewW, uint32_t viewH) {
    Invariants(name, p);
    const bool ok = p.dst == dst && p.viewW == viewW && p.viewH == viewH;
    if (!ok)
        std::printf("  FAIL  %s: V %ux%u dst %d,%d %ux%u -- wanted V %ux%u dst %d,%d %ux%u\n", name,
                    p.viewW, p.viewH, p.dst.x, p.dst.y, p.dst.w, p.dst.h, viewW, viewH, dst.x,
                    dst.y, dst.w, dst.h);
    else
        std::printf("  ok    %-38s V %4ux%-4u -> %4ux%-4u at %d,%d  %u bar%s  %s\n", name, viewW,
                    viewH, dst.w, dst.h, dst.x, dst.y, p.barCount, p.barCount == 1 ? "" : "s",
                    p.nearest ? "nearest" : "linear");
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    std::printf("policies, 320x224 on 2560x1080\n");
    {
        // What the layer always did, and must still do when nothing is set.
        const Placement p = ComputePlacement(Base());
        Expect("auto, nothing set: fills the screen", p, {0, 0, 2560, 1080}, 2560, 1080);
        Check(p.barCount == 0, "auto: no bars");
    }
    {
        // DOS and NTSC pixels are 1.2 times as tall as they are wide -- the Windows app's x1.2 -- so
        // 320x224 stands as 320x268.8. Fitted to 1080 tall that is 320 * 1080 / 268.8 = 1285.7 wide,
        // rounded to 1286 and centred at (2560 - 1286) / 2 = 637.
        PlacementInput in = Base();
        in.pixelHeight = 1.2f;
        Expect("auto + DOS/NTSC x1.2: letterboxed", ComputePlacement(in), {637, 0, 1286, 1080},
               1286, 1080);
    }
    {
        // The direction is the thing to get right, so pin it: taller pixels make the picture
        // narrower for its height, never wider.
        PlacementInput in = Base();
        in.pixelHeight = 2.0f;  // double tall: 320x448, fitted to 1080 tall is 771 wide
        const Placement p = ComputePlacement(in);
        Invariants("double tall", p);
        Check(p.dst.w < 1080u * 320u / 224u, "taller pixels narrow the picture");
    }
    {
        // x3 is 960x672, which fits, so it stands at its own size in the middle.
        PlacementInput in = Base();
        in.pixelSize = 3.0f;
        const Placement p = ComputePlacement(in);
        Expect("pixel size x3: centred one to one", p, {800, 204, 960, 672}, 960, 672);
        Check(p.nearest, "one to one is nearest");
    }
    {
        // x5 is 1600x1120, taller than the screen: fitted, 1080 tall, 1600 * 1080 / 1120 = 1543,
        // centred at (2560 - 1543) / 2 = 508.5, which centring floors to 508.
        PlacementInput in = Base();
        in.pixelSize = 5.0f;
        Expect("pixel size x5: too tall, fitted", ComputePlacement(in), {508, 0, 1543, 1080}, 1600,
               1120);
    }
    {
        // The largest whole multiple of 320x224 inside 2560x1080 is x4 (x5 would be 1120 tall).
        PlacementInput in = Base();
        in.pixelSize = 1.0f;
        in.policy = kOutputInteger;
        const Placement p = ComputePlacement(in);
        Expect("integer: x4, centred", p, {640, 92, 1280, 896}, 320, 224);
        Check(p.nearest, "integer is nearest");
    }
    {
        // Integer when even x1 does not fit: shrunk, not cut.
        PlacementInput in = Base();
        in.pixelSize = 6.0f;  // 1920x1344
        in.policy = kOutputInteger;
        Expect("integer, too big even once: fitted", ComputePlacement(in), {508, 0, 1543, 1080},
               1920, 1344);
    }
    {
        PlacementInput in = Base();
        in.pixelSize = 1.0f;
        in.policy = kOutputStretch;
        const Placement p = ComputePlacement(in);
        Expect("stretch: the whole screen", p, {0, 0, 2560, 1080}, 320, 224);
        // 1080 / 224 is not whole, so a nearest filter would draw uneven rows.
        Check(!p.nearest, "stretch at a fractional scale is linear");
    }
    {
        // Fill scales by the larger ratio, 2560 / 320 = 8, and shows the middle 1080 / 8 = 135 rows
        // of the 224, starting (224 - 135) / 2 = 44 down.
        PlacementInput in = Base();
        in.pixelSize = 1.0f;
        in.policy = kOutputFill;
        const Placement p = ComputePlacement(in);
        Expect("fill: covers, crops top and bottom", p, {0, 0, 2560, 1080}, 320, 224);
        Check(p.src == IRect {0, 44, 320, 135}, "fill shows the middle 135 rows");
        Check(p.barCount == 0, "fill leaves no bars");
    }
    {
        // A magnifier larger than the window shows its middle at full size.
        PlacementInput in = Base();
        in.pixelSize = 10.0f;  // 3200x2240
        in.policy = kOutputCentre;
        const Placement p = ComputePlacement(in);
        Expect("centre, larger than the screen", p, {0, 0, 2560, 1080}, 3200, 2240);
        Check(p.src == IRect {320, 580, 2560, 1080}, "centre shows the middle of V");
    }

    std::printf("\nturning and mirroring\n");
    {
        // A quarter turn renders V on its side and turns it upright: 1080 wide by 2560 tall.
        PlacementInput in = Base();
        in.rotation = kRotate90;
        const Placement p = ComputePlacement(in);
        Expect("90: V rendered on its side", p, {0, 0, 2560, 1080}, 1080, 2560);
        Check(p.quarterTurn && !p.flipH && !p.flipV, "90 is one quarter turn, no mirrors");
    }
    {
        PlacementInput in = Base();
        in.rotation = kRotate180;
        const Placement p = ComputePlacement(in);
        Invariants("180", p);
        Check(!p.quarterTurn && p.flipH && p.flipV, "180 is both mirrors and no render pass");
    }
    {
        PlacementInput in = Base();
        in.rotation = kRotate270;
        const Placement p = ComputePlacement(in);
        Invariants("270", p);
        Check(p.quarterTurn && p.flipH && p.flipV, "270 is a quarter turn plus both mirrors");
    }
    {
        // A mirror the user asked for, combined with a half turn, cancels on that axis.
        PlacementInput in = Base();
        in.flipH = true;
        in.rotation = kRotate180;
        const Placement p = ComputePlacement(in);
        Check(!p.flipH && p.flipV, "mirror + half turn cancel horizontally");
    }
    {
        // Integer on a turned raster uses the turned shape: 224x320 fits x3 in 1080 tall.
        PlacementInput in = Base();
        in.pixelSize = 1.0f;
        in.policy = kOutputInteger;
        in.rotation = kRotate90;
        Expect("90 + integer: x3 of the turned shape", ComputePlacement(in),
               {944, 60, 672, 960}, 320, 224);
    }

    std::printf("\ncrop\n");
    {
        PlacementInput in = Base();
        in.cropEnabled = true;
        in.crop = {100, 50, 800, 600};
        const Placement p = ComputePlacement(in);
        Expect("crop: the effect stays in its rectangle", p, {100, 50, 800, 600}, 800, 600);
        Check(p.barCount == 0, "outside the crop is not cleared");
    }
    {
        // Dragged past the corner: clamped to the edge, not refused.
        const IRect t = TargetArea(2560, 1080, true, {2400, 1000, 400, 400});
        Check(t == IRect {2400, 1000, 160, 80}, "crop past the edge is clamped");
    }
    {
        const IRect t = TargetArea(2560, 1080, true, {3000, 2000, 100, 100});
        Check(t == IRect {0, 0, 2560, 1080}, "crop entirely off screen is ignored");
    }
    {
        // Letterboxing inside a crop clears only the crop's own bars.
        PlacementInput in = Base();
        in.cropEnabled = true;
        in.crop = {100, 100, 1000, 400};
        in.pixelHeight = 1.2f;
        Invariants("crop + aspect", ComputePlacement(in));
    }

    std::printf("\nnonsense in, nothing out\n");
    {
        PlacementInput in = Base();
        in.sourceW = 0;
        Check(!ComputePlacement(in).Valid(), "no source raster is not a placement");
        in = Base();
        in.policy = 99;
        Invariants("an unknown policy behaves as auto", ComputePlacement(in));
        in = Base();
        in.pixelHeight = -1.0f;
        Invariants("a negative aspect is ignored", ComputePlacement(in));
    }

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all placement checks passed");
    return g_failures ? 1 : 0;
}
