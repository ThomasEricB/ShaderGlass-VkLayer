/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "crop_picker.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

using shaderglass::CropHandle;
using shaderglass::CropRect;

namespace {

// How close to an edge, in screen pixels, still grabs it.
constexpr double kGrabPx = 6.0;

}  // namespace

CropPicker::CropPicker(QCheckBox* enabled, QSpinBox* x, QSpinBox* y, QSpinBox* w, QSpinBox* h,
                       QWidget* parent)
    : QWidget(parent), _enabled(enabled), _x(x), _y(y), _w(w), _h(h) {
    setMouseTracking(true);
    setMinimumSize(240, 180);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setToolTip(tr("Drag on the picture to draw the rectangle; drag inside it to move it, or its "
                  "edges and corners to resize it. The picture is the newest capture."));

    // Any change to the numbers, by whoever, is drawn.
    for (QSpinBox* box : {_x, _y, _w, _h})
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { update(); });
    connect(_enabled, &QCheckBox::toggled, this, [this](bool) { update(); });
}

int CropPicker::heightForWidth(int w) const {
    if (_image.isNull()) return w * 9 / 16;
    return int(std::lround(double(w) * _image.height() / _image.width()));
}

void CropPicker::Refresh(const QString& dir) {
    const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*-before.ppm")},
                                                        QDir::Files, QDir::Time);
    if (files.isEmpty()) return;
    const QFileInfo& newest = files.first();
    const qint64 stamp = newest.lastModified().toMSecsSinceEpoch();
    if (newest.absoluteFilePath() == _imagePath && stamp == _imageStamp) return;

    QImage image(newest.absoluteFilePath());
    if (image.isNull()) return;  // still being written; the next refresh gets it
    const bool resized = image.size() != _image.size();
    _image = std::move(image);
    _imagePath = newest.absoluteFilePath();
    _imageStamp = stamp;
    if (resized) updateGeometry();
    update();
}

QRectF CropPicker::ImageArea() const {
    if (_image.isNull()) return QRectF();
    const double sx = double(width()) / _image.width();
    const double sy = double(height()) / _image.height();
    const double s = std::min(sx, sy);
    const double w = _image.width() * s, h = _image.height() * s;
    return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

QPoint CropPicker::ToImage(const QPointF& p) const {
    const QRectF a = ImageArea();
    if (a.isEmpty()) return QPoint();
    const double s = a.width() / _image.width();
    return QPoint(int(std::lround((p.x() - a.x()) / s)), int(std::lround((p.y() - a.y()) / s)));
}

QRectF CropPicker::ToWidget(const CropRect& r) const {
    const QRectF a = ImageArea();
    if (a.isEmpty()) return QRectF();
    const double s = a.width() / _image.width();
    return QRectF(a.x() + r.x * s, a.y() + r.y * s, r.w * s, r.h * s);
}

CropRect CropPicker::Boxes() const {
    return {_x->value(), _y->value(), _w->value(), _h->value()};
}

void CropPicker::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (_image.isNull()) {
        p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("No capture yet.\nPress Capture while a game is running, then drag the "
                      "rectangle here."));
        return;
    }

    const QRectF area = ImageArea();
    p.drawImage(area, _image);

    const CropRect r = _dragging ? _live : Boxes();
    if (r.Empty()) return;
    const QRectF box = ToWidget(r).intersected(area);

    // Outside the rectangle is what the layer leaves alone: dimmed, the more so when the crop is off
    // and the rectangle is only a proposal.
    QPainterPath outside;
    outside.addRect(area);
    QPainterPath inside;
    inside.addRect(box);
    const bool on = _enabled->isChecked() || _dragging;
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, on ? 120 : 60));

    QPen pen(palette().color(QPalette::Highlight), 2.0);
    if (!on) pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(box);

    // Grab handles at the corners and edge midpoints.
    p.setBrush(palette().color(QPalette::Highlight));
    p.setPen(Qt::NoPen);
    const double hs = 3.5;
    for (QPointF c : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight(),
                      QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()),
                      QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y())})
        p.drawRect(QRectF(c.x() - hs, c.y() - hs, 2 * hs, 2 * hs));

    // The numbers, while dragging, so a precise rectangle can be drawn without looking away.
    if (_dragging) {
        const QString label =
            QStringLiteral("%1, %2   %3 × %4").arg(r.x).arg(r.y).arg(r.w).arg(r.h);
        const QRectF text = p.fontMetrics().boundingRect(label).adjusted(-6, -3, 6, 3);
        QRectF at(box.left(), box.top() - text.height() - 4, text.width(), text.height());
        if (at.top() < area.top()) at.moveTop(box.top() + 4);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(at, 3, 3);
        p.setPen(Qt::white);
        p.drawText(at, Qt::AlignCenter, label);
    }
}

void CropPicker::UpdateCursor(const QPointF& at) {
    if (_image.isNull()) {
        unsetCursor();
        return;
    }
    const double s = ImageArea().width() / _image.width();
    const int slop = std::max(1, int(std::lround(kGrabPx / s)));
    const QPoint ip = ToImage(at);
    switch (HitCrop(Boxes(), ip.x(), ip.y(), slop)) {
        case CropHandle::kMove: setCursor(Qt::SizeAllCursor); break;
        case CropHandle::kLeft:
        case CropHandle::kRight: setCursor(Qt::SizeHorCursor); break;
        case CropHandle::kTop:
        case CropHandle::kBottom: setCursor(Qt::SizeVerCursor); break;
        case CropHandle::kTopLeft:
        case CropHandle::kBottomRight: setCursor(Qt::SizeFDiagCursor); break;
        case CropHandle::kTopRight:
        case CropHandle::kBottomLeft: setCursor(Qt::SizeBDiagCursor); break;
        default: setCursor(Qt::CrossCursor); break;
    }
}

void CropPicker::mousePressEvent(QMouseEvent* event) {
    if (_image.isNull() || event->button() != Qt::LeftButton) return;
    const double s = ImageArea().width() / _image.width();
    const int slop = std::max(1, int(std::lround(kGrabPx / s)));
    _from = ToImage(event->position());
    _start = Boxes();
    _handle = HitCrop(_start, _from.x(), _from.y(), slop);
    _live = _handle == CropHandle::kNone ? CropRect {} : _start;
    _dragging = true;
    update();
}

void CropPicker::mouseMoveEvent(QMouseEvent* event) {
    if (!_dragging) {
        UpdateCursor(event->position());
        return;
    }
    const QPoint to = ToImage(event->position());
    _live = DragCrop(_handle, _start, _from.x(), _from.y(), to.x(), to.y(), _image.width(),
                     _image.height());
    update();
}

void CropPicker::mouseReleaseEvent(QMouseEvent* event) {
    if (!_dragging || event->button() != Qt::LeftButton) return;
    _dragging = false;
    // A click without a drag draws nothing, and must not wipe the rectangle there is.
    if (_live.w >= 2 && _live.h >= 2 && !(_live == _start)) {
        _x->setValue(_live.x);
        _y->setValue(_live.y);
        _w->setValue(_live.w);
        _h->setValue(_live.h);
        // Drawing a rectangle is asking for it.
        _enabled->setChecked(true);
    }
    UpdateCursor(event->position());
    update();
}
