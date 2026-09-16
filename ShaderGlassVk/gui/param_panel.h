/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The selected preset's parameters.

A preset's parameters live in its shaders, as "#pragma parameter NAME "Description" def min max
step". The same name often appears in several passes and means one value, so the panel shows the
union rather than one entry per pass.

What it publishes is *overrides*, not values. The layer already resolves each parameter as shader
default, then preset override, then whatever the interface has set -- so the shared block only has
to carry the third, and a Mega Bezel preset with several hundred parameters still fits in the 128
slots the protocol reserves. A parameter the user has not touched is simply absent.
*/

#pragma once

#include "../../presets/preset_api.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QFormLayout;
class QLabel;

namespace shaderglass {
struct ShmHeader;
}

class ParamPanel : public QWidget {
    Q_OBJECT
  public:
    explicit ParamPanel(QWidget* parent = nullptr);

    void SetHeader(shaderglass::ShmHeader* header) { _hdr = header; }

    // Rebuild for a preset, or clear when given nothing.
    void Show(const SgPreset* preset);

    // Overrides the user has set, as "name=value" lines. Part of a profile.
    QString Blob() const;
    void ApplyBlob(const QString& blob);

    void ResetAll();

  private:
    struct Entry {
        float value = 0.0f;
        float fallback = 0.0f;  // what the preset resolves to with no override
    };

    void Publish();
    void Clear();

    shaderglass::ShmHeader* _hdr = nullptr;
    const SgPreset* _preset = nullptr;

    // Overrides waiting to be applied the next time the rows are built. A profile is loaded before
    // the widgets for its preset exist, and setting a value behind a control that is about to be
    // rebuilt leaves the control showing something else.
    QHash<QString, float> _pending;
    QWidget* _form = nullptr;
    QFormLayout* _layout = nullptr;
    QLabel* _empty = nullptr;
    QHash<QString, Entry> _entries;
};
