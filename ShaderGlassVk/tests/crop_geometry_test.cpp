/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Dragging the crop rectangle on a capture: what each grab does, and that no drag can leave the numbers
the layer reads outside the image or inside out.
*/

#include "../gui/crop_geometry.h"

#include <cstdio>
#include <string>

using namespace shaderglass;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::string Str(const CropRect& r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.w) + "x" +
           std::to_string(r.h);
}

void Expect(const char* what, const CropRect& got, const CropRect& want) {
    Check(got == want, std::string(what) + ": " + Str(got) +
                           (got == want ? "" : " (wanted " + Str(want) + ")"));
}

}  // namespace

int main() {
    const int W = 640, H = 480;
    const CropRect r {100, 100, 200, 150};  // right edge 300, bottom 250

    std::printf("what a press grabs\n");
    Check(HitCrop(r, 100, 100, 4) == CropHandle::kTopLeft, "the top-left corner");
    Check(HitCrop(r, 302, 248, 4) == CropHandle::kBottomRight, "the bottom-right corner, near it");
    Check(HitCrop(r, 98, 175, 4) == CropHandle::kLeft, "the left edge, just outside");
    Check(HitCrop(r, 200, 250, 4) == CropHandle::kBottom, "the bottom edge");
    Check(HitCrop(r, 200, 175, 4) == CropHandle::kMove, "the inside moves it");
    Check(HitCrop(r, 400, 175, 4) == CropHandle::kNone, "outside starts a new one");
    Check(HitCrop(CropRect {}, 0, 0, 4) == CropHandle::kNone, "no rectangle: always a new one");

    std::printf("\ndragging\n");
    Expect("a new rectangle spans the two points",
           DragCrop(CropHandle::kNone, r, 50, 60, 250, 160, W, H), {50, 60, 200, 100});
    Expect("drawn right to left and upwards, the same",
           DragCrop(CropHandle::kNone, r, 250, 160, 50, 60, W, H), {50, 60, 200, 100});
    Expect("moved", DragCrop(CropHandle::kMove, r, 150, 150, 170, 140, W, H), {120, 90, 200, 150});
    Expect("moved past the edge stops there, the same size",
           DragCrop(CropHandle::kMove, r, 150, 150, 900, 900, W, H), {440, 330, 200, 150});
    Expect("the right edge", DragCrop(CropHandle::kRight, r, 300, 150, 340, 150, W, H),
           {100, 100, 240, 150});
    Expect("the top-left corner", DragCrop(CropHandle::kTopLeft, r, 100, 100, 80, 90, W, H),
           {80, 90, 220, 160});
    Expect("the left edge dragged past the right one flips it",
           DragCrop(CropHandle::kLeft, r, 100, 150, 350, 150, W, H), {300, 100, 50, 150});
    Expect("a corner dragged off the image stops at its edge",
           DragCrop(CropHandle::kBottomRight, r, 300, 250, 1000, -50, W, H), {100, 0, 540, 100});
    Expect("a new rectangle started outside the image is clipped to it",
           DragCrop(CropHandle::kNone, r, -30, -30, 100, 100, W, H), {0, 0, 100, 100});

    std::printf("\n%s\n", g_failures ? "FAILURES" : "all crop-geometry checks passed");
    return g_failures ? 1 : 0;
}
