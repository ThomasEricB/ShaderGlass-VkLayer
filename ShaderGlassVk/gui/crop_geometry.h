/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

What a mouse drag does to the crop rectangle (decision 12: drag it on the layer's screenshot, or type
it into the numeric boxes, and the two stay in sync).

Everything is in the game's own pixels -- the capture's pixels, which are the swapchain's -- so what is
dragged here is exactly the four numbers the layer reads. The widget only converts the mouse to these
coordinates and back.

Plain C++, no Qt, so it can be tested without a display.
*/

#pragma once

#include <cstdint>

namespace shaderglass {

struct CropRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool Empty() const { return w <= 0 || h <= 0; }
    bool operator==(const CropRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
};

// Which part of the rectangle a press grabbed. Edges and corners resize, the inside moves, and
// anywhere else starts a new rectangle.
enum class CropHandle { kNone, kMove, kLeft, kRight, kTop, kBottom, kTopLeft, kTopRight,
                        kBottomLeft, kBottomRight };

// `slop` is how close, in image pixels, counts as on an edge. The widget works it out from a few
// screen pixels, so grabbing an edge is as easy at any zoom.
CropHandle HitCrop(const CropRect& r, int px, int py, int slop);

// The rectangle after dragging `handle` from (fromX, fromY) to (toX, toY), starting from `start`.
// A new rectangle (kNone) spans the two points. Always inside the image, never inside out: dragging
// an edge past its opposite one flips the rectangle rather than giving it a negative size.
CropRect DragCrop(CropHandle handle, const CropRect& start, int fromX, int fromY, int toX, int toY,
                  int imageW, int imageH);

}  // namespace shaderglass
