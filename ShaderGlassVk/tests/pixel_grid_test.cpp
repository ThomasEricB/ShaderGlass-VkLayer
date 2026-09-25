/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The source-raster detector, on frames whose answer is known.

Every case here is a small random raster blown up by a factor the test picks, so the number the
detector should report is not a matter of opinion. What is being proved is the awkward half of the
problem: fractional factors, which are the reason the x2..x8 divisor list cannot express what these
games actually do, and smooth upscales, where the step edges the method leans on have been filtered
away. The negative cases matter just as much -- a detector that finds a raster in a photograph would
be worse than no detector, because it would act on it.
*/

#include "../layer/src/pixel_grid.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace shaderglass;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

uint32_t g_seed = 12345;
int Rand(int n) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (int) ((g_seed >> 16) % (uint32_t) n);
}

std::vector<uint8_t> MakeSource(int w, int h) {
    std::vector<uint8_t> src((size_t) w * h);
    // Blocky, high-contrast, and not noise: neighbouring source pixels often share a value, which is
    // what real pixel art looks like and what makes a naive run-length method overcount.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            src[(size_t) y * w + x] = (uint8_t) (Rand(4) * 80);
    return src;
}

// Nearest-neighbour upscale by a possibly fractional factor, exactly as a game's own scaler would.
std::vector<uint8_t> UpscaleNearest(const std::vector<uint8_t>& src, int sw, int sh, int dw,
                                    int dh) {
    std::vector<uint8_t> dst((size_t) dw * dh);
    for (int y = 0; y < dh; ++y) {
        const int sy = (int) ((int64_t) y * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = (int) ((int64_t) x * sw / dw);
            dst[(size_t) y * dw + x] = src[(size_t) sy * sw + sx];
        }
    }
    return dst;
}

// A 1-2-1 blur, applied to soften the step edges the way a smooth upscale would.
std::vector<uint8_t> Blur(const std::vector<uint8_t>& in, int w, int h, int passes) {
    std::vector<uint8_t> a = in, b((size_t) w * h);
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int l = a[(size_t) y * w + (x > 0 ? x - 1 : 0)];
                const int c = a[(size_t) y * w + x];
                const int r = a[(size_t) y * w + (x + 1 < w ? x + 1 : w - 1)];
                b[(size_t) y * w + x] = (uint8_t) ((l + 2 * c + r) / 4);
            }
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h; ++y) {
                const int u = b[(size_t) (y > 0 ? y - 1 : 0) * w + x];
                const int c = b[(size_t) y * w + x];
                const int d = b[(size_t) (y + 1 < h ? y + 1 : h - 1) * w + x];
                a[(size_t) y * w + x] = (uint8_t) ((u + 2 * c + d) / 4);
            }
    }
    return a;
}

void CheckScale(const char* what, int sw, int sh, int dw, int dh, int blurPasses, float tolerance) {
    const std::vector<uint8_t> src = MakeSource(sw, sh);
    std::vector<uint8_t> big = UpscaleNearest(src, sw, sh, dw, dh);
    if (blurPasses) big = Blur(big, dw, dh, blurPasses);

    const GridEstimate g = DetectPixelGrid(big.data(), dw, dh, dw);
    const float wantX = float(dw) / float(sw);
    const float wantY = float(dh) / float(sh);

    if (!g.Valid()) {
        std::printf("  FAIL  %s: nothing detected (wanted %.3f x %.3f)\n", what, wantX, wantY);
        ++g_failures;
        return;
    }

    const float ex = std::fabs(g.scaleX - wantX);
    const float ey = std::fabs(g.scaleY - wantY);
    const bool ok = ex <= tolerance && ey <= tolerance;
    if (!ok)
        std::printf("  FAIL  %s: got %.3f x %.3f, wanted %.3f x %.3f (conf %.3f)\n", what, g.scaleX,
                    g.scaleY, wantX, wantY, g.confidence);
    else
        std::printf("  ok    %-34s %.3f x %.3f (wanted %.3f x %.3f, conf %.2f)\n", what, g.scaleX,
                    g.scaleY, wantX, wantY, g.confidence);
    if (!ok) ++g_failures;

    // The raster it implies has to come back to the source size, which is what the chain is built
    // at -- a scale that is right to a hundredth but rounds to the wrong raster helps nobody.
    uint32_t rw = 0, rh = 0;
    if (GridToSourceExtent(g, (uint32_t) dw, (uint32_t) dh, &rw, &rh)) {
        const int dwid = std::abs((int) rw - sw), dhei = std::abs((int) rh - sh);
        Check(dwid <= 2 && dhei <= 2, "GridToSourceExtent lands on the source raster");
        if (dwid > 2 || dhei > 2)
            std::printf("        raster %ux%u, wanted %dx%d\n", rw, rh, sw, sh);
    }
}

void CheckNoGrid(const char* what, const std::vector<uint8_t>& img, int w, int h) {
    const GridEstimate g = DetectPixelGrid(img.data(), w, h, w);
    if (g.Valid())
        std::printf("  FAIL  %s: found %.3f x %.3f (conf %.3f) where there is no grid\n", what,
                    g.scaleX, g.scaleY, g.confidence);
    else
        std::printf("  ok    %-34s no grid, as expected\n", what);
    if (g.Valid()) ++g_failures;
}

}  // namespace

int main() {
    std::printf("integer upscales\n");
    CheckScale("x2 nearest", 320, 240, 640, 480, 0, 0.05f);
    CheckScale("x3 nearest", 320, 240, 960, 720, 0, 0.05f);
    CheckScale("x4 nearest", 256, 224, 1024, 896, 0, 0.05f);

    std::printf("\nfractional upscales -- the point of the exercise\n");
    // 1082/224 = 4.830, 2562/320 = 8.006: a real ultrawide window over a real raster.
    CheckScale("320x224 -> 2562x1082", 320, 224, 2562, 1082, 0, 0.08f);
    CheckScale("256x224 -> 1920x1080", 256, 224, 1920, 1080, 0, 0.08f);
    CheckScale("384x216 -> 1600x900", 384, 216, 1600, 900, 0, 0.08f);

    std::printf("\nsmooth upscales -- step edges filtered away\n");
    CheckScale("x4 nearest + blur", 256, 224, 1024, 896, 1, 0.10f);
    CheckScale("320x224 -> 2562x1082 + blur", 320, 224, 2562, 1082, 1, 0.12f);

    std::printf("\nframes with no raster to find\n");
    {
        const int w = 1280, h = 720;
        std::vector<uint8_t> flat((size_t) w * h, 128);
        CheckNoGrid("flat frame", flat, w, h);

        std::vector<uint8_t> grad((size_t) w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) grad[(size_t) y * w + x] = (uint8_t) ((x * 255) / w);
        CheckNoGrid("smooth gradient", grad, w, h);

        std::vector<uint8_t> noise((size_t) w * h);
        for (auto& v : noise) v = (uint8_t) Rand(256);
        CheckNoGrid("per-pixel noise", noise, w, h);
    }

    // The case this actually failed on in a game: a publisher logo held on a black screen. Measured
    // over the whole frame it has a strong period -- the logo's own pixels -- and the detector once
    // reported a 211x69 raster and switched the chain to it, which is unusable and does not recover
    // on its own. The frame is mostly black, so the halves cannot agree, and that is the rejection.
    {
        const int w = 2562, h = 1082;
        std::vector<uint8_t> frame((size_t) w * h, 0);
        const int lw = 400, lh = 120;
        const std::vector<uint8_t> logo = MakeSource(lw / 4, lh / 4);
        const std::vector<uint8_t> big = UpscaleNearest(logo, lw / 4, lh / 4, lw, lh);
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x)
                frame[(size_t) (h / 2 - lh / 2 + y) * w + (w / 2 - lw / 2 + x)] =
                    big[(size_t) y * lw + x];
        CheckNoGrid("logo held on a black screen", frame, w, h);
    }

    // And a raster too small to be a game's, which the floor rejects even where a period exists.
    {
        const int w = 1280, h = 720;
        const std::vector<uint8_t> src = MakeSource(64, 48);
        const std::vector<uint8_t> big = UpscaleNearest(src, 64, 48, w, h);
        const GridEstimate g = DetectPixelGrid(big.data(), w, h, w);
        uint32_t rw = 0, rh = 0;
        const bool took = GridToSourceExtent(g, (uint32_t) w, (uint32_t) h, &rw, &rh);
        if (took) std::printf("  FAIL  64x48 raster accepted as %ux%u\n", rw, rh);
        else std::printf("  ok    %-34s rejected, below the floor\n", "64x48 raster");
        if (took) ++g_failures;
    }

    // The axes must stay independent. These are the numbers a real game produced -- x6.25 across
    // and x5.55 down on a 2562x1082 window, repeatably, at equal confidence. An earlier version
    // read that 12.6% disagreement as measurement error and averaged it into one x6.04, on the
    // theory that pixels are usually square. The game disagreed: at x6.04 the picture beat and
    // shimmered whenever the view scrolled, because the vertical grid no longer lined up with the
    // game's own. A repeatable disagreement is a fact about the game, and averaging it away
    // measures the assumption instead of the frame.
    // A frame that is not all one grid, which is what a game with a dialogue box or a menu over the
    // picture actually presents. Measured over the whole frame the loud region simply wins: a band
    // covering a third of the picture took the answer 42% wrong while still reporting confidently.
    //
    // The intruder's period is deliberately not a harmonic of the game's. When it is -- a x3 overlay
    // over a x6 game -- the two are genuinely indistinguishable, because a grid of period 3 is also
    // a grid of period 6 with every other boundary missing, and no amount of voting settles that.
    // That ambiguity is in the picture, not in the method.
    std::printf("\nframes carrying a second, competing grid\n");
    for (double frac : {0.20, 0.35}) {
        const int w = 2562, h = 1082, sw = 427, sh = 180;
        const std::vector<uint8_t> src = MakeSource(sw, sh);
        std::vector<uint8_t> img = UpscaleNearest(src, sw, sh, w, h);
        const int band = int(h * frac), y0 = (h - band) / 2;
        for (int y = y0; y < y0 + band; ++y)
            for (int x = 0; x < w; ++x)
                img[(size_t) y * w + x] = (uint8_t) (((x / 5) * 37 + (y / 5) * 91) % 256);

        const GridEstimate g = DetectPixelGrid(img.data(), w, h, w);
        const float wx = float(w) / sw, wy = float(h) / sh;
        const bool ok = g.Valid() && std::fabs(g.scaleX - wx) < 0.1f &&
                        std::fabs(g.scaleY - wy) < 0.1f;
        if (!ok) {
            std::printf("  FAIL  competing grid over %.0f%%: got %.3f x %.3f, wanted %.3f x %.3f\n",
                        frac * 100, g.scaleX, g.scaleY, wx, wy);
            ++g_failures;
        } else {
            std::printf("  ok    competing grid over %2.0f%% of the frame   %.3f x %.3f (conf %.2f)\n",
                        frac * 100, g.scaleX, g.scaleY, g.confidence);
        }
    }

    // A game drawing one small room in the middle of a wide window, which is what an unlit interior
    // in Chrono Trigger looks like: the drawn part covers about 40% of the width and 86% of the
    // height, and everything around it is the game's own near-black surround -- a very dark brown,
    // not zero, so nothing here may key off brightness.
    //
    // Measured across the whole frame this fails outright: most sampled scanlines lie in the empty
    // space and contribute nothing, and the real signal disappears into the average. The drawn area
    // has to be found first and the measurement confined to it.
    std::printf("\nan interior with a large empty surround\n");
    {
        const int w = 2572, h = 1092, sw = 410, sh = 196;
        const float wx = float(w) / sw, wy = float(h) / sh;
        const int cx0 = int(w * 0.305), cx1 = int(w * 0.70);
        const int cy0 = 0, cy1 = int(h * 0.86);

        const std::vector<uint8_t> src = MakeSource(sw, sh);
        const std::vector<uint8_t> big = UpscaleNearest(src, sw, sh, w, h);
        std::vector<uint8_t> frame((size_t) w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool drawn = x >= cx0 && x < cx1 && y >= cy0 && y < cy1;
                // The surround is dark but not flat, exactly as the game's is.
                frame[(size_t) y * w + x] = drawn ? big[(size_t) y * w + x] : (uint8_t) (16 + Rand(3));
            }

        const GridEstimate g = DetectPixelGrid(frame.data(), w, h, w);
        const bool ok = g.Valid() && std::fabs(g.scaleX - wx) < 0.2f &&
                        std::fabs(g.scaleY - wy) < 0.2f;
        if (!ok)
            std::printf("  FAIL  drawn area %d%% x %d%%: got %.3f x %.3f (conf %.2f), wanted "
                        "%.3f x %.3f\n",
                        int((cx1 - cx0) * 100.0 / w), int((cy1 - cy0) * 100.0 / h), g.scaleX,
                        g.scaleY, g.confidence, wx, wy);
        else
            std::printf("  ok    drawn area %d%% wide, %d%% tall      %.3f x %.3f (conf %.2f)\n",
                        int((cx1 - cx0) * 100.0 / w), int((cy1 - cy0) * 100.0 / h), g.scaleX,
                        g.scaleY, g.confidence);
        if (!ok) ++g_failures;
    }

    std::printf("\nthe two axes stay independent\n");
    {
        const GridEstimate g = CombineAxes(6.25f, 0.56f, 5.55f, 0.56f);
        Check(g.Valid(), "a disagreeing pair still produces an estimate");
        Check(std::fabs(g.scaleX - 6.25f) < 0.001f && std::fabs(g.scaleY - 5.55f) < 0.001f,
              "axes measured apart are reported apart, not averaged");
        uint32_t rw = 0, rh = 0;
        GridToSourceExtent(g, 2562, 1082, &rw, &rh);
        std::printf("  ok    %-34s x%.2f by x%.2f -> %ux%u\n", "x6.25 by x5.55", double(g.scaleX),
                    double(g.scaleY), rw, rh);
    }
    {
        const GridEstimate g = CombineAxes(8.006f, 0.53f, 4.830f, 0.53f);
        Check(std::fabs(g.scaleX - 8.006f) < 0.001f && std::fabs(g.scaleY - 4.830f) < 0.001f,
              "a stretched 4:3 raster is reported as measured");
        std::printf("  ok    %-34s x%.2f by x%.2f -> as measured\n", "x8.01 by x4.83",
                    double(g.scaleX), double(g.scaleY));
    }

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all pixel-grid checks passed");
    return g_failures ? 1 : 0;
}
