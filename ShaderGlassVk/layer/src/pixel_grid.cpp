/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "pixel_grid.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace shaderglass {
namespace {

// Candidate periods are walked at this spacing. A hundredth of a pixel is finer than the raster it
// resolves to: at a 2562-wide swapchain, 0.01 of period separates rasters one pixel apart.
constexpr float kScaleStep = 0.01f;

// How close to the best score a longer period must come before it is preferred as the fundamental.
// Harmonics of a clean impulse train score within a few percent of it, so this has to be generous;
// what it must not do is promote noise, which is why it is a fraction of the best score rather than
// an absolute floor.
constexpr double kHarmonicTolerance = 0.80;

// At most this many rows (or columns) are summed. The difference energy converges long before a
// whole 1080-line frame is read, and the cost of this runs on a game's own thread.
constexpr int kMaxLines = 96;

// How much of an axis the edge energy must reach across before the period it implies is believed.
// Half, because a 4:3 game pillarboxed into a 21:9 window covers only 56% of the width and is
// perfectly legitimate, while a centred logo covers a small fraction of it and is not.
// A scanline counts as drawn-on when it varies at least this much relative to the busiest line in
// the frame, and the drawn part of an axis must reach at least this fraction of it to be believed.
constexpr double kLineActiveFrac = 0.06;
constexpr double kMinContentExtent = 0.22;

// How many scanlines are inspected when looking for the drawn band. Enough to place its edges within
// a few pixels at any resolution, cheap because each one is a single pass over the line.
constexpr int kActivityScanLines = 192;

// How many bands the measurement axis is split into, how many must find a period before their vote
// is used, and how far either side of their median the full-frame refinement is allowed to look.
// Five bands with three agreeing means a competing grid has to cover more than half the frame to
// carry the vote.
constexpr int kBands = 7;
constexpr int kMinBandsAgreeing = 4;
constexpr int kMinBandSamples = 96;
constexpr float kRefineWindow = 0.08f;

// Accumulates |luma[i] - luma[i-1]| along one axis.
//
// `count` is the length of the axis being measured and `lines` the one being summed over; `step` and
// `lineStep` are the byte strides along each. Writing it once for both axes keeps the vertical case
// from quietly disagreeing with the horizontal one.
std::vector<double> DifferenceEnergy(const uint8_t* base, int count, int lineFrom, int lineTo,
                                     int step, int lineStep) {
    std::vector<double> d((size_t) count, 0.0);
    const int span = lineTo - lineFrom + 1;
    if (count < 2 || span < 1) return d;

    // Spread over the given range rather than the whole frame. The range is the part the game
    // actually drew; sampling outside it averages in scanlines that vary not at all and dilutes
    // the very thing being measured.
    const int used = span < kMaxLines ? span : kMaxLines;
    for (int n = 0; n < used; ++n) {
        const int line =
            used == 1 ? lineFrom + span / 2 : lineFrom + (int) ((int64_t) n * (span - 1) / (used - 1));
        const uint8_t* p = base + (size_t) line * lineStep;
        int prev = p[0];
        for (int i = 1; i < count; ++i) {
            const int cur = p[(size_t) i * step];
            d[(size_t) i] += std::abs(cur - prev);
            prev = cur;
        }
    }
    return d;
}

// How much one scanline varies from sample to sample. Near zero for a line lying entirely in a
// game's black surround, whatever shade of black that surround happens to be -- which is why this
// measures variation and not brightness. The empty space around a Chrono Trigger room is a very dark
// brown, and a brightness threshold would have to be tuned per game to find it.
double LineActivity(const uint8_t* p, int count, int step) {
    double sum = 0.0;
    int prev = p[0];
    for (int i = 1; i < count; ++i) {
        const int cur = p[(size_t) i * step];
        sum += std::abs(cur - prev);
        prev = cur;
    }
    return sum / double(count - 1);
}

// The band of scanlines the game drew into, found by looking at every line and keeping the range
// from the first that carries detail to the last.
//
// This is the letterbox problem, and it is not only the black bars of a widescreen frame: a game
// that draws one small room in the middle of a large window is letterboxed on all four sides by its
// own empty space. Measuring across the whole frame then averages a little real signal into a great
// deal of nothing, and the period disappears into the noise -- which is exactly what happened in an
// unlit room where the drawn part covered barely a third of the width.
void ActiveLineRange(const uint8_t* base, int count, int lines, int step, int lineStep, int* from,
                     int* to) {
    *from = 0;
    *to = lines - 1;
    if (lines < 4) return;

    const int probes = lines < kActivityScanLines ? lines : kActivityScanLines;
    std::vector<double> act((size_t) probes, 0.0);
    double peak = 0.0;
    for (int n = 0; n < probes; ++n) {
        const int line = (int) ((int64_t) n * (lines - 1) / (probes - 1));
        act[(size_t) n] = LineActivity(base + (size_t) line * lineStep, count, step);
        peak = act[(size_t) n] > peak ? act[(size_t) n] : peak;
    }
    if (peak <= 0.0) return;

    const double level = peak * kLineActiveFrac;
    int first = -1, last = -1;
    for (int n = 0; n < probes; ++n) {
        if (act[(size_t) n] < level) continue;
        if (first < 0) first = n;
        last = n;
    }
    if (first < 0 || last <= first) return;

    // Back to line indices, widened by one probe each way so the edge of the drawn area is inside.
    const int f = (int) ((int64_t) (first > 0 ? first - 1 : 0) * (lines - 1) / (probes - 1));
    const int t = (int) ((int64_t) (last + 1 < probes ? last + 1 : probes - 1) * (lines - 1) /
                         (probes - 1));
    *from = f;
    *to = t > f ? t : f;
}

// The normalised magnitude of d's Fourier component at each candidate period, and the period that
// best explains the signal once harmonics are accounted for.
//
// Returns 0 for the period when there is nothing periodic to find.
float EstimatePeriod(const std::vector<double>& d, float* confidence, float lo = kMinScale,
                     float hi = kMaxScale) {
    *confidence = 0.0f;
    const int n = (int) d.size();
    if (n < 8 || hi <= lo) return 0.0f;

    double sum = 0.0;
    for (double v : d) sum += v;
    const double mean = sum / n;

    // Removing the mean matters: d is non-negative, so its DC term dwarfs everything and leaks into
    // every candidate. What is left is the ripple, which is the thing being measured.
    std::vector<double> centred((size_t) n);
    double energy = 0.0;
    for (int i = 0; i < n; ++i) {
        centred[(size_t) i] = d[(size_t) i] - mean;
        energy += std::abs(centred[(size_t) i]);
    }
    if (energy <= 1e-9) return 0.0f;  // a flat frame: no edges at all

    const int steps = (int) ((hi - lo) / kScaleStep) + 1;
    std::vector<double> score((size_t) steps, 0.0);

    for (int k = 0; k < steps; ++k) {
        const double p = lo + kScaleStep * k;
        const double w = 2.0 * M_PI / p;
        // Rotating a unit vector beats calling sin/cos per sample, and over a few thousand steps in
        // double the drift is far below the precision this needs.
        const double cw = std::cos(w), sw = std::sin(w);
        double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
        for (int i = 0; i < n; ++i) {
            const double v = centred[(size_t) i];
            re += v * cr;
            im -= v * ci;
            const double nr = cr * cw - ci * sw;
            ci = cr * sw + ci * cw;
            cr = nr;
        }
        score[(size_t) k] = std::sqrt(re * re + im * im) / energy;
    }

    double best = 0.0;
    for (double s : score) best = s > best ? s : best;
    if (best <= 0.0) return 0.0f;

    // The fundamental is the longest period that still scores near the best, because the peaks of an
    // impulse train of period p sit at p, p/2, p/3 ... -- all at shorter periods, never longer.
    const double floorScore = best * kHarmonicTolerance;
    int chosen = -1;
    for (int k = steps - 1; k >= 0; --k) {
        if (score[(size_t) k] < floorScore) continue;
        const bool peak = (k == 0 || score[(size_t) k] >= score[(size_t) k - 1]) &&
                          (k == steps - 1 || score[(size_t) k] >= score[(size_t) k + 1]);
        if (peak) {
            chosen = k;
            break;
        }
    }
    if (chosen < 0) return 0.0f;

    *confidence = (float) score[(size_t) chosen];
    return lo + kScaleStep * chosen;
}

// The stretch of the axis the game drew across, as [first, last] indices into d. Everything outside
// it is the empty surround, and measuring it measures nothing.
bool ActiveAxisRange(const std::vector<double>& d, int* from, int* to) {
    double peak = 0.0;
    for (double v : d) peak = v > peak ? v : peak;
    if (peak <= 0.0) return false;

    // Well below a real step edge, well above the rounding of a filtered flat area.
    const double level = peak * 0.02;
    int first = -1, last = -1;
    for (int i = 0; i < (int) d.size(); ++i) {
        if (d[(size_t) i] < level) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0 || last <= first) return false;
    *from = first;
    *to = last;
    return true;
}

// The period, but only if what produced it covers the picture.
//
// A Square Enix logo on a black screen was once measured as a 211x69 raster and acted on, and the
// chain switched to a raster nothing could recover from. Measured over the whole frame a logo looks
// exactly like a grid -- a strong peak at some period -- because it is one, just not the game's.
// What tells them apart is reach, so an answer that comes from a narrow band of the frame is thrown
// away no matter how confident it looks.
float GuardedPeriod(const uint8_t* base, int count, int lines, int step, int lineStep,
                    float* confidence) {
    *confidence = 0.0f;
    if (!base || count < 8 || lines < 1) return 0.0f;

    // What the game drew, before anything is measured. Both axes are trimmed to it: the scanlines
    // sampled are the ones with something on them, and the stretch of the axis measured is the
    // stretch with something across it.
    int lineFrom = 0, lineTo = lines - 1;
    ActiveLineRange(base, count, lines, step, lineStep, &lineFrom, &lineTo);

    const std::vector<double> full = DifferenceEnergy(base, count, lineFrom, lineTo, step, lineStep);
    int from = 0, to = count - 1;
    if (!ActiveAxisRange(full, &from, &to)) return 0.0f;

    // A drawn area this small is not a game's picture -- it is a logo, a title card or a lone
    // dialogue box on an otherwise empty screen, and the grid inside it is that object's, not the
    // game's. The guard has to be on the drawn extent rather than on the frame, because a game
    // legitimately draws one small room in the middle of a wide window.
    if (to - from < int(double(count) * kMinContentExtent)) return 0.0f;

    const std::vector<double> d(full.begin() + from, full.begin() + to + 1);

    // Measured in bands and settled by vote, because a frame is not all one grid. A dialogue box, a
    // menu, an HD overlay drawn at its own resolution -- each is a competing grid, and measured over
    // the whole frame the loud one simply wins: a band covering half the picture took the answer
    // 40% to 60% wrong while still reporting a confident result, which is the worst way to be wrong.
    //
    // No band can outvote the rest, so a region on its own grid has to cover most of the frame
    // before it changes the answer.
    const int n = (int) d.size();
    const int seg = n / kBands;
    std::vector<float> found;
    if (seg >= kMinBandSamples) {
        found.reserve(kBands);
        for (int k = 0; k < kBands; ++k) {
            const std::vector<double> part(d.begin() + (size_t) k * seg,
                                           d.begin() + (size_t) (k + 1) * seg);
            float c = 0.0f;
            const float p = EstimatePeriod(part, &c);
            if (p > 0.0f) found.push_back(p);
        }
    }

    if ((int) found.size() >= kMinBandsAgreeing) {
        std::sort(found.begin(), found.end());
        const float median = found[found.size() / 2];

        // The vote says which period; the whole axis says exactly which. Re-measured over every
        // sample but with the candidates narrowed to a window around the median, so the full
        // frame's resolution is kept while the competing grid's peak -- far outside that window --
        // can no longer be chosen.
        // How much of the frame stands behind the answer. A picture that is half one grid and half
        // another is a genuine tie, and the failure that matters there is not picking the wrong one
        // -- it is saying so confidently, which is what makes a caller act on it. Scaling the
        // confidence by the share of bands that agree lets such a frame fall below kMinConfidence
        // and be reported as no answer at all, which is the truth.
        int agreeing = 0;
        for (float p : found)
            if (std::fabs(p - median) <= median * kRefineWindow) ++agreeing;
        const float share = float(agreeing) / float(kBands);

        float c = 0.0f;
        const float refined =
            EstimatePeriod(d, &c, median * (1.0f - kRefineWindow), median * (1.0f + kRefineWindow));
        if (refined > 0.0f) {
            *confidence = c * share;
            return refined;
        }
        *confidence = 1.0f / float(found.size());  // the vote alone, weakly held
        return median;
    }

    // Too short an axis to band, or too few bands found anything: fall back to the whole frame.
    return EstimatePeriod(d, confidence);
}

}  // namespace

int MaxSampledLines() { return kMaxLines; }

float DetectStripPeriod(const uint8_t* luma, int count, int lines, float* confidence) {
    // A packed strip is exactly the degenerate case of the general walk: stride equals the row
    // length, so there is no gap between one scanline and the next.
    return GuardedPeriod(luma, count, lines, 1, count, confidence);
}

GridEstimate CombineAxes(float scaleX, float confX, float scaleY, float confY) {
    GridEstimate g {};
    if (scaleX <= 0.0f || scaleY <= 0.0f) return g;

    // Both axes have to agree that there is a grid. One axis finding a period on its own is what a
    // frame of horizontal scanlines or a vertical gradient looks like, and neither is a raster.
    g.confidence = confX < confY ? confX : confY;
    if (g.confidence < kMinConfidence) {
        g.confidence = 0.0f;
        return g;
    }

    // The axes are reported exactly as measured, and deliberately not reconciled against each other.
    //
    // There was a version of this that assumed pixels are usually square, and folded two axes that
    // landed within 25% of each other into one scale. It was wrong. A real game measured x6.25
    // across and x5.55 down -- the same numbers to the hundredth, twice running, at equal
    // confidence -- and forcing those to a single x6.04 made the picture beat and shimmer whenever
    // the view scrolled, because the vertical resampling grid no longer lined up with the game's.
    //
    // The lesson is that a repeatable disagreement between the axes is a fact about the game, not
    // error to be averaged away. A game whose raster does not match its window's aspect is an
    // ordinary thing -- the window stretches it -- and a detector that tidies that away is
    // measuring its own assumption instead of the frame.
    g.scaleX = scaleX;
    g.scaleY = scaleY;
    return g;
}

GridEstimate DetectPixelGrid(const uint8_t* luma, int w, int h, int stride) {
    GridEstimate g {};
    if (!luma || w < 16 || h < 16 || stride < w) return g;

    float cx = 0.0f, cy = 0.0f;
    const float sx = GuardedPeriod(luma, w, h, 1, stride, &cx);
    const float sy = GuardedPeriod(luma, h, w, stride, 1, &cy);
    return CombineAxes(sx, cx, sy, cy);
}

bool GridToSourceExtent(const GridEstimate& g, uint32_t swapW, uint32_t swapH, uint32_t* outW,
                        uint32_t* outH) {
    if (!g.Valid() || g.confidence < kMinConfidence || !swapW || !swapH) return false;

    const long w = std::lround(double(swapW) / double(g.scaleX));
    const long h = std::lround(double(swapH) / double(g.scaleY));
    // A floor below any real game's raster. The narrowest thing anyone actually shipped is a Game
    // Boy's 160x144, so anything under this is not a game's resolution -- it is a measurement of
    // something that was never a grid, and acting on it produces the unusable picture that made
    // this floor necessary.
    if (w < kMinRasterW || h < kMinRasterH) return false;
    if (w > (long) swapW || h > (long) swapH) return false;

    *outW = (uint32_t) w;
    *outH = (uint32_t) h;
    return true;
}

}  // namespace shaderglass
