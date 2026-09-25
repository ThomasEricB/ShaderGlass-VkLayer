/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2021-2025 mausimus (mausimus.net)
Copyright (C) 2026 Thomas Eric, bmitch87
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0

Finding a pixel-art game's real raster inside the frame it presents.

A game that draws 320x224 and hands the compositor 2562x1082 has upscaled by 8.006 across and 4.83
down, and neither number is on the x2..x8 divisor list. Picking the nearest integer is not a rounding
error, it is a different picture: the chain resamples on a grid that does not line up with the
game's, so every source pixel lands on a fractional boundary and the shader's scanlines and masks
beat against the art. That is why 3 looks worse than 2 and 4 worse than 3 -- none of them is right,
and being further wrong is not better.

The frame itself knows the answer. Whatever filter did the upscale, the edges between the game's own
pixels survive as a periodic ripple: with nearest-neighbour they are step changes every s columns,
and with a smooth filter they are softened but still periodic. So take the column-difference energy

    D[x] = sum over sampled rows of |luma(x, y) - luma(x - 1, y)|

which is large at a source-pixel boundary and small inside one, and ask which period best explains
it. The answer is the magnitude of D's Fourier component at that period, normalised by D's own
energy so a busy frame and a sparse one score alike:

    score(p) = |sum_x (D[x] - mean(D)) * exp(-2*pi*i*x/p)| / sum_x |D[x] - mean(D)|

Evaluated directly over candidate periods rather than through an FFT, because p is wanted to a
hundredth and an FFT's bins are not spaced that way -- fractional periods are the whole point.

One trap is worth naming. An impulse train of period p has Fourier peaks at p, p/2, p/3 and so on,
so the strongest score may sit on a harmonic and report a raster twice as fine as the real one. The
fundamental is the *largest* period among the strong peaks, so that is what is chosen, rather than
the highest-scoring one.

Confidence is reported rather than assumed. A frame with no pixel grid -- a photograph, a 3D scene,
a blank menu -- produces no peak worth the name, and the caller is expected to keep the previous
answer instead of acting on noise.
*/

#pragma once

#include <cstdint>

namespace shaderglass {

struct GridEstimate {
    // Swapchain pixels per source pixel, per axis. Fractional on purpose.
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    // 0..1. How much of the difference energy the winning period explains; see kMinConfidence.
    float confidence = 0.0f;

    bool Valid() const { return scaleX >= 1.0f && scaleY >= 1.0f && confidence > 0.0f; }
};

// Below this the frame is treated as having no pixel grid at all. Chosen from the synthetic cases in
// tests/pixel_grid_test.cpp: clean upscales land far above it, smooth content far below.
inline constexpr float kMinConfidence = 0.12f;

// The range worth searching. Below 1.5 there is no grid to find -- a game rendering at its own
// resolution needs no correction -- and above 16 a single source pixel would fill a sixteenth of the
// screen, which is not a game.
inline constexpr float kMinScale = 1.5f;
inline constexpr float kMaxScale = 16.0f;

// The smallest raster worth believing, comfortably under a Game Boy's 160x144 and comfortably over
// the nonsense a frame with no grid produces.
inline constexpr int kMinRasterW = 128;
inline constexpr int kMinRasterH = 112;


// luma is one byte per pixel, w by h, rows `stride` bytes apart. Returns an estimate whose Valid()
// is false when the frame does not look like upscaled pixel art.
GridEstimate DetectPixelGrid(const uint8_t* luma, int w, int h, int stride);

// The same measurement on one axis of a packed strip: `lines` scanlines of `count` samples, laid end
// to end. The caller packs rows for the horizontal period and columns for the vertical one -- the
// mathematics does not care which it was given, and packing is what lets the layer read a few
// hundred kilobytes of a frame instead of converting all of it.
//
// Returns 0 when there is no period to find; *confidence is set either way.
float DetectStripPeriod(const uint8_t* luma, int count, int lines, float* confidence);

// Fold two axis measurements into an estimate, applying the agreement and confidence rules so a
// caller that measured the axes itself reaches the same verdict DetectPixelGrid would. The two
// scales are kept independent; see the note in CombineAxes for why they must not be averaged.
GridEstimate CombineAxes(float scaleX, float confX, float scaleY, float confY);

// How many scanlines per axis the detector will actually look at. The layer sizes its readback from
// this, so the two cannot drift apart.
int MaxSampledLines();

// The raster the estimate implies, clamped to something a chain can be built at. Returns false when
// the estimate is not worth acting on.
bool GridToSourceExtent(const GridEstimate& g, uint32_t swapW, uint32_t swapH, uint32_t* outW,
                        uint32_t* outH);

}  // namespace shaderglass
