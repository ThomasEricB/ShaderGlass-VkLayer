/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The before/after view.

There is no preview renderer here (decision 8). Rendering the chain a second time in this process
would mean a second Vulkan device, a second copy of the catalogue, and a picture that still would
not be the one the game is showing -- different resolution, different source raster, different
frame. So the layer writes the two images it already has, straight out of the chain: the frame the
game presented and the frame it presented after. This shows that pair.

The layer writes them as PPM, which Qt reads natively, so nothing here needs an image decoder and
nothing in the layer needs an encoder.
*/

#pragma once

#include <QString>
#include <QWidget>

class QLabel;

class CaptureView : public QWidget {
    Q_OBJECT
  public:
    explicit CaptureView(QWidget* parent = nullptr);

    // Look in `dir` for the newest before/after pair and show it.
    void Refresh(const QString& dir);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void Rescale();

    QLabel* _beforeLabel = nullptr;
    QLabel* _afterLabel = nullptr;
    QLabel* _note = nullptr;
    QImage _before;
    QImage _after;
};
