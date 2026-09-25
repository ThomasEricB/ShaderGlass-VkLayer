/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "placement.h"

#include "../../common/shm_protocol.h"

#include <algorithm>
#include <cmath>

namespace shaderglass {
namespace {

uint32_t RoundPositive(double v) {
    const double r = std::floor(v + 0.5);
    return r < 1.0 ? 1u : uint32_t(r);
}

// A w x h rectangle centred in `in`, which it must not exceed.
IRect CentreIn(const IRect& in, uint32_t w, uint32_t h) {
    IRect r;
    r.w = std::min(w, in.w);
    r.h = std::min(h, in.h);
    r.x = in.x + int32_t((in.w - r.w) / 2);
    r.y = in.y + int32_t((in.h - r.h) / 2);
    return r;
}

// Whatever of `outer` the rectangle `inner` leaves uncovered, as at most four strips: full-width bars
// above and below, then the sides between them. Nothing overlaps, so each strip is cleared once.
uint32_t Uncovered(const IRect& outer, const IRect& inner, IRect* out) {
    uint32_t n = 0;
    const int32_t ox1 = outer.x + int32_t(outer.w), oy1 = outer.y + int32_t(outer.h);
    const int32_t ix1 = inner.x + int32_t(inner.w), iy1 = inner.y + int32_t(inner.h);

    if (inner.y > outer.y) out[n++] = {outer.x, outer.y, outer.w, uint32_t(inner.y - outer.y)};
    if (iy1 < oy1) out[n++] = {outer.x, iy1, outer.w, uint32_t(oy1 - iy1)};
    if (inner.x > outer.x) out[n++] = {outer.x, inner.y, uint32_t(inner.x - outer.x), inner.h};
    if (ix1 < ox1) out[n++] = {ix1, inner.y, uint32_t(ox1 - ix1), inner.h};
    return n;
}

// The largest `aspect`-shaped rectangle inside `t`, centred.
IRect FitShape(const IRect& t, double shapeW, double shapeH) {
    const double s = std::min(double(t.w) / shapeW, double(t.h) / shapeH);
    return CentreIn(t, RoundPositive(shapeW * s), RoundPositive(shapeH * s));
}

}  // namespace

IRect TargetArea(uint32_t swapW, uint32_t swapH, bool cropEnabled, const IRect& crop) {
    IRect whole {0, 0, swapW, swapH};
    if (!cropEnabled || crop.Empty()) return whole;

    // Clamped rather than rejected. A crop dragged a little past the edge of a screenshot means the
    // edge, and refusing it would leave the user wondering why their rectangle did nothing.
    const int64_t x0 = std::clamp<int64_t>(crop.x, 0, swapW);
    const int64_t y0 = std::clamp<int64_t>(crop.y, 0, swapH);
    const int64_t x1 = std::clamp<int64_t>(int64_t(crop.x) + crop.w, 0, swapW);
    const int64_t y1 = std::clamp<int64_t>(int64_t(crop.y) + crop.h, 0, swapH);
    if (x1 <= x0 || y1 <= y0) return whole;  // entirely off-screen: as good as no crop
    return {int32_t(x0), int32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
}

Placement ComputePlacement(const PlacementInput& in) {
    Placement p;
    if (!in.swapW || !in.swapH || !in.sourceW || !in.sourceH) return p;

    p.target = TargetArea(in.swapW, in.swapH, in.cropEnabled, in.crop);
    const IRect& t = p.target;

    // Half a turn is both mirrors, and three quarters is a quarter plus both, which leaves at most
    // one quarter turn for the render pass to do.
    const uint32_t turns = in.rotation & 3u;
    p.quarterTurn = (turns & 1u) != 0;
    const bool half = turns >= 2;
    p.flipH = in.flipH != half;
    p.flipV = in.flipV != half;

    // How tall a source pixel is for its width. The Windows app's convention, not the more usual
    // width-over-height pixel aspect: its presets are written that way, and following them is what
    // makes x1.2 mean what a DOS game's author meant rather than its reciprocal.
    const double tall = (in.pixelHeight > 0.0f && std::isfinite(in.pixelHeight)) ? in.pixelHeight
                                                                              : 1.0;
    const bool corrected = std::fabs(tall - 1.0) > 1e-4;
    const bool explicitSize = in.pixelSize > 0.0f && std::isfinite(in.pixelSize);
    uint32_t policy = in.policy < kOutputPolicyCount ? in.policy : uint32_t(kOutputAuto);

    // The shape the picture should have on screen, before any scaling decision: the source raster
    // with its pixels made the right height. Turned a quarter, width and height trade places.
    double shapeW = double(in.sourceW), shapeH = double(in.sourceH) * tall;
    if (p.quarterTurn) std::swap(shapeW, shapeH);

    // ---- V, what the chain renders -------------------------------------------------------------
    if (!explicitSize) {
        // Auto: the chain targets the whole target area -- or, with a correction, the largest
        // correctly shaped rectangle inside it.
        IRect shown = corrected ? FitShape(t, shapeW, shapeH) : t;
        p.viewW = p.quarterTurn ? shown.h : shown.w;
        p.viewH = p.quarterTurn ? shown.w : shown.h;
        if (policy == kOutputAuto) policy = corrected ? kOutputFit : kOutputStretch;
    } else {
        p.viewW = RoundPositive(double(in.sourceW) * in.pixelSize);
        p.viewH = RoundPositive(double(in.sourceH) * in.pixelSize * tall);
        if (policy == kOutputAuto) {
            // Somebody chose a pixel size: honour it at its own size, or fit it when it is too big.
            const uint32_t vw = p.quarterTurn ? p.viewH : p.viewW;
            const uint32_t vh = p.quarterTurn ? p.viewW : p.viewH;
            policy = (vw <= t.w && vh <= t.h) ? kOutputCentre : kOutputFit;
        }
    }

    // V as it will stand on screen, turned.
    const uint32_t vw = p.quarterTurn ? p.viewH : p.viewW;
    const uint32_t vh = p.quarterTurn ? p.viewW : p.viewH;
    const IRect all {0, 0, vw, vh};

    // ---- placing it ----------------------------------------------------------------------------
    if (policy == kOutputInteger) {
        const uint32_t k = std::min(t.w / vw, t.h / vh);
        if (k >= 1) {
            p.src = all;
            p.dst = CentreIn(t, vw * k, vh * k);
        } else {
            policy = kOutputFit;  // not even once: shrink it rather than cut it
        }
    }

    switch (policy) {
        case kOutputStretch:
            p.src = all;
            p.dst = t;
            break;
        case kOutputFit:
            p.src = all;
            p.dst = FitShape(t, double(vw), double(vh));
            break;
        case kOutputFill: {
            // Cover the target, and show only the middle of V that lands inside it.
            const double s = std::max(double(t.w) / vw, double(t.h) / vh);
            p.dst = t;
            p.src = CentreIn(all, RoundPositive(t.w / s), RoundPositive(t.h / s));
            break;
        }
        case kOutputCentre:
            if (vw <= t.w && vh <= t.h) {
                p.src = all;
                p.dst = CentreIn(t, vw, vh);
            } else {
                // Larger than the window: a magnifier shows the middle of it at full size.
                p.dst = CentreIn(t, std::min(vw, t.w), std::min(vh, t.h));
                p.src = CentreIn(all, p.dst.w, p.dst.h);
            }
            break;
        case kOutputInteger:
            break;  // placed above
        default:
            p.src = all;
            p.dst = t;
            break;
    }

    p.barCount = Uncovered(t, p.dst, p.bars);

    // Hard pixels only when every source pixel lands on a whole number of screen pixels. Anything
    // else is resampled, and nearest at a fractional scale draws some pixels one wide and some two,
    // which on a CRT mask is visible as beating.
    p.nearest = p.dst.w % p.src.w == 0 && p.dst.h % p.src.h == 0;
    return p;
}

}  // namespace shaderglass
