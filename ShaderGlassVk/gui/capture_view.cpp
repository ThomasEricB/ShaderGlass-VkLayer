/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "capture_view.h"

#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>

CaptureView::CaptureView(QWidget* parent) : QWidget(parent) {
    auto* grid = new QGridLayout(this);

    auto* beforeTitle = new QLabel(tr("Before"), this);
    auto* afterTitle = new QLabel(tr("After"), this);
    beforeTitle->setAlignment(Qt::AlignCenter);
    afterTitle->setAlignment(Qt::AlignCenter);
    grid->addWidget(beforeTitle, 0, 0);
    grid->addWidget(afterTitle, 0, 1);

    _beforeLabel = new QLabel(this);
    _afterLabel = new QLabel(this);
    for (QLabel* l : {_beforeLabel, _afterLabel}) {
        l->setAlignment(Qt::AlignCenter);
        l->setMinimumSize(160, 120);
        l->setFrameShape(QFrame::StyledPanel);
    }
    grid->addWidget(_beforeLabel, 1, 0);
    grid->addWidget(_afterLabel, 1, 1);
    grid->setRowStretch(1, 1);

    _note = new QLabel(tr("No captures yet. Use Capture while a game is running."), this);
    _note->setAlignment(Qt::AlignCenter);
    _note->setEnabled(false);
    grid->addWidget(_note, 2, 0, 1, 2);
}

void CaptureView::Refresh(const QString& dir) {
    QDir d(dir);
    // The layer names a pair "<counter>-before.ppm" and "<counter>-after.ppm"; the newest by
    // modification time is the one just taken.
    const QFileInfoList files =
        d.entryInfoList({QStringLiteral("*-after.ppm")}, QDir::Files, QDir::Time);
    if (files.isEmpty()) {
        _note->setText(tr("No captures in %1").arg(QDir::toNativeSeparators(dir)));
        _note->show();
        return;
    }

    const QString after = files.first().absoluteFilePath();
    QString before = after;
    before.replace(QStringLiteral("-after.ppm"), QStringLiteral("-before.ppm"));

    _after = QImage(after);
    _before = QImage(before);

    if (_after.isNull() && _before.isNull()) {
        _note->setText(tr("Could not read %1").arg(QDir::toNativeSeparators(after)));
        _note->show();
        return;
    }

    _note->setText(tr("%1  —  %2×%3")
                       .arg(QFileInfo(after).fileName())
                       .arg(_after.width())
                       .arg(_after.height()));
    _note->show();
    Rescale();
}

void CaptureView::Rescale() {
    const auto fit = [](QLabel* label, const QImage& image) {
        if (image.isNull()) {
            label->setText(tr("(none)"));
            return;
        }
        label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio,
                                                          Qt::SmoothTransformation));
    };
    fit(_beforeLabel, _before);
    fit(_afterLabel, _after);
}

void CaptureView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    Rescale();
}
