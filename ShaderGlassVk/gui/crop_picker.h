/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The crop rectangle, dragged on the layer's screenshot (decision 12).

It shows the newest "before" capture -- the game's frame as it presented it, in the game's own pixels
-- and the rectangle the four numeric boxes hold. Dragging on it changes those boxes, so there is only
ever one copy of the numbers: the boxes, which write the mapping. The picture is redrawn from the
boxes, so a change typed into them, loaded with a profile or made by shaderglass-ctl shows here too.

The boxes are written when the mouse is released, not on every move. A different crop is a different
chain size and so a rebuild, and a rebuild per mouse move would stutter the game for nothing.
*/

#pragma once

#include <QImage>
#include <QWidget>

#include "crop_geometry.h"

class QCheckBox;
class QSpinBox;

class CropPicker : public QWidget {
    Q_OBJECT
  public:
    CropPicker(QCheckBox* enabled, QSpinBox* x, QSpinBox* y, QSpinBox* w, QSpinBox* h,
               QWidget* parent = nullptr);

    // Look in `dir` for the newest before-capture and show it, if it is not the one shown already.
    void Refresh(const QString& dir);

    QSize sizeHint() const override { return QSize(480, 270); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    // Where the image sits in the widget, and the conversions between the two.
    QRectF ImageArea() const;
    QPoint ToImage(const QPointF& widgetPoint) const;
    QRectF ToWidget(const shaderglass::CropRect& r) const;
    shaderglass::CropRect Boxes() const;
    void UpdateCursor(const QPointF& at);

    QCheckBox* _enabled;
    QSpinBox* _x;
    QSpinBox* _y;
    QSpinBox* _w;
    QSpinBox* _h;

    QImage _image;
    QString _imagePath;
    qint64 _imageStamp = 0;

    bool _dragging = false;
    shaderglass::CropHandle _handle = shaderglass::CropHandle::kNone;
    shaderglass::CropRect _start;
    shaderglass::CropRect _live;
    QPoint _from;
};
