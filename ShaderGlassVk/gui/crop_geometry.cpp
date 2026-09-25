/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "crop_geometry.h"

#include <algorithm>
#include <cstdlib>

namespace shaderglass {

CropHandle HitCrop(const CropRect& r, int px, int py, int slop) {
    if (r.Empty()) return CropHandle::kNone;
    const int l = r.x, t = r.y, rr = r.x + r.w, b = r.y + r.h;
    const bool inX = px >= l - slop && px <= rr + slop;
    const bool inY = py >= t - slop && py <= b + slop;
    if (!inX || !inY) return CropHandle::kNone;

    const bool nearL = std::abs(px - l) <= slop, nearR = std::abs(px - rr) <= slop;
    const bool nearT = std::abs(py - t) <= slop, nearB = std::abs(py - b) <= slop;
    if (nearT && nearL) return CropHandle::kTopLeft;
    if (nearT && nearR) return CropHandle::kTopRight;
    if (nearB && nearL) return CropHandle::kBottomLeft;
    if (nearB && nearR) return CropHandle::kBottomRight;
    if (nearL) return CropHandle::kLeft;
    if (nearR) return CropHandle::kRight;
    if (nearT) return CropHandle::kTop;
    if (nearB) return CropHandle::kBottom;
    if (px > l && px < rr && py > t && py < b) return CropHandle::kMove;
    return CropHandle::kNone;
}

CropRect DragCrop(CropHandle handle, const CropRect& start, int fromX, int fromY, int toX, int toY,
                  int imageW, int imageH) {
    const auto clampX = [imageW](int v) { return std::clamp(v, 0, imageW); };
    const auto clampY = [imageH](int v) { return std::clamp(v, 0, imageH); };
    const int dx = toX - fromX, dy = toY - fromY;

    if (handle == CropHandle::kMove) {
        // Moved whole, stopped at the image's edges without changing size.
        CropRect r = start;
        r.x = std::clamp(start.x + dx, 0, std::max(0, imageW - start.w));
        r.y = std::clamp(start.y + dy, 0, std::max(0, imageH - start.h));
        return r;
    }

    // Everything else is two corners, one of which may move.
    int x0, y0, x1, y1;
    if (handle == CropHandle::kNone) {
        x0 = fromX;
        y0 = fromY;
        x1 = toX;
        y1 = toY;
    } else {
        x0 = start.x;
        y0 = start.y;
        x1 = start.x + start.w;
        y1 = start.y + start.h;
        switch (handle) {
            case CropHandle::kLeft: x0 += dx; break;
            case CropHandle::kRight: x1 += dx; break;
            case CropHandle::kTop: y0 += dy; break;
            case CropHandle::kBottom: y1 += dy; break;
            case CropHandle::kTopLeft: x0 += dx; y0 += dy; break;
            case CropHandle::kTopRight: x1 += dx; y0 += dy; break;
            case CropHandle::kBottomLeft: x0 += dx; y1 += dy; break;
            case CropHandle::kBottomRight: x1 += dx; y1 += dy; break;
            default: break;
        }
    }
    x0 = clampX(x0);
    x1 = clampX(x1);
    y0 = clampY(y0);
    y1 = clampY(y1);
    CropRect r;
    r.x = std::min(x0, x1);
    r.y = std::min(y0, y1);
    r.w = std::abs(x1 - x0);
    r.h = std::abs(y1 - y0);
    return r;
}

}  // namespace shaderglass
