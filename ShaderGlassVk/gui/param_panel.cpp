/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "param_panel.h"

#include "../common/shm_protocol.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

using shaderglass::ShmHeader;

namespace {

// A "#pragma parameter" whose range is empty is not a control, it is a heading -- the libretro
// shaders use them to break long parameter lists into sections.
bool IsHeading(const SgParam& p) {
    return p.min_value == p.max_value && p.step_value == 0.0f;
}

// Copy a parameter name into the shared block without touching any sequence number.
//
// ShmStoreString would bump one, and the only sequence guarding these names is presetSeq -- which
// the layer also reads the preset id under. Bumping it while a slider is being dragged would make
// that guarded read retry and, often enough, give up and return nothing, which the layer would take
// as "no preset". The names are safe to write plainly: they only change when the preset does, and
// the layer reads them alongside paramCount, which is published under controlSeq.
void StoreName(char* dst, size_t cap, const QByteArray& src) {
    std::memset(dst, 0, cap);
    if (cap > 1) {
        const size_t n = size_t(src.size());
        std::memcpy(dst, src.constData(), n < cap - 1 ? n : cap - 1);
    }
}

}  // namespace

ParamPanel::ParamPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll, 1);

    _form = new QWidget(scroll);
    _layout = new QFormLayout(_form);
    _layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scroll->setWidget(_form);

    _empty = new QLabel(tr("No preset selected."), this);
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setEnabled(false);
    outer->addWidget(_empty);
}

void ParamPanel::Clear() {
    while (QLayoutItem* item = _layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void ParamPanel::Show(const SgPreset* preset) {
    Clear();
    _preset = preset;
    _entries.clear();

    if (!preset) {
        _empty->setText(tr("No preset selected."));
        _empty->show();
        return;
    }

    // The union across passes, in the order first seen. The same parameter in two passes is one
    // value -- that is what the name means -- so the second sighting is skipped.
    QStringList seen;
    for (size_t p = 0; p < preset->pass_count; ++p) {
        const SgShader* shader = preset->passes[p].shader;
        if (!shader) continue;

        for (size_t i = 0; i < shader->param_count; ++i) {
            const SgParam& param = shader->params[i];
            if (!param.name || !param.name[0]) continue;
            if (!param.description || !param.description[0]) continue;  // a semantic, not a control

            const QString name = QString::fromUtf8(param.name);
            if (seen.contains(name)) continue;
            seen.append(name);

            const QString label = QString::fromUtf8(param.description);

            if (IsHeading(param)) {
                auto* heading = new QLabel(label, _form);
                QFont bold = heading->font();
                bold.setBold(true);
                heading->setFont(bold);
                _layout->addRow(heading);
                continue;
            }

            // What this parameter resolves to with no override: the shader's default, unless the
            // preset overrode it.
            float fallback = param.default_value;
            for (size_t o = 0; o < preset->override_count; ++o) {
                if (preset->overrides[o].name && name == QLatin1String(preset->overrides[o].name)) {
                    fallback = preset->overrides[o].value;
                    break;
                }
            }
            // A profile loaded before this row existed is applied here, where the widgets can
            // still be given the value.
            float value = fallback;
            auto pending = _pending.constFind(name);
            if (pending != _pending.constEnd()) value = pending.value();
            _entries.insert(name, Entry {value, fallback});

            const double step = param.step_value > 0.0f ? double(param.step_value) : 0.01;
            const double lo = double(param.min_value);
            const double hi = double(param.max_value);

            auto* row = new QWidget(_form);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);

            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(0, int(std::lround((hi - lo) / step)));
            rowLayout->addWidget(slider, 1);

            auto* spin = new QDoubleSpinBox(row);
            spin->setRange(lo, hi);
            spin->setSingleStep(step);
            spin->setDecimals(step < 0.01 ? 4 : (step < 1.0 ? 2 : 0));
            spin->setValue(double(value));
            rowLayout->addWidget(spin);

            slider->setValue(int(std::lround((double(value) - lo) / step)));

            // The two controls are one value. Each writes the other with signals blocked, so
            // moving either does not bounce back through the one that moved.
            connect(slider, &QSlider::valueChanged, this, [this, spin, name, lo, step](int v) {
                const double value = lo + v * step;
                QSignalBlocker block(spin);
                spin->setValue(value);
                _entries[name].value = float(value);
                Publish();
            });
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                    [this, slider, name, lo, step](double v) {
                        QSignalBlocker block(slider);
                        slider->setValue(int(std::lround((v - lo) / step)));
                        _entries[name].value = float(v);
                        Publish();
                    });

            auto* labelWidget = new QLabel(label, _form);
            labelWidget->setToolTip(name);
            _layout->addRow(labelWidget, row);
        }
    }

    if (_layout->rowCount() == 0) {
        _empty->setText(tr("This preset has no adjustable parameters."));
        _empty->show();
    } else {
        _empty->hide();
    }

    // Consumed: anything still here named a parameter this preset does not have.
    _pending.clear();
    Publish();
}

// Only what differs from what the preset would do on its own. That keeps the published block small
// -- a Mega Bezel preset declares several hundred parameters and the protocol reserves 128 slots --
// and it keeps the meaning honest: this is a list of overrides, not of values.
void ParamPanel::Publish() {
    if (!_hdr) return;

    uint32_t n = 0;
    for (auto it = _entries.constBegin(); it != _entries.constEnd(); ++it) {
        if (it.value().value == it.value().fallback) continue;
        if (n >= shaderglass::kMaxParams) break;

        StoreName(_hdr->params[n].name, shaderglass::kParamNameBytes, it.key().toUtf8());
        shaderglass::ShmStoreParam(_hdr->params[n], it.value().value);
        ++n;
    }

    _hdr->paramCount.store(n, std::memory_order_relaxed);
    _hdr->controlSeq.fetch_add(1, std::memory_order_release);
}

QString ParamPanel::Blob() const {
    QString out;
    for (auto it = _entries.constBegin(); it != _entries.constEnd(); ++it) {
        if (it.value().value == it.value().fallback) continue;
        out += it.key();
        out += QLatin1Char('=');
        out += QString::number(double(it.value().value), 'g', 9);
        out += QLatin1Char('\n');
    }
    return out;
}

void ParamPanel::ApplyBlob(const QString& blob) {
    _pending.clear();
    for (const QString& line : blob.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        _pending.insert(line.left(eq), line.mid(eq + 1).toFloat());
    }
    // Rebuilding is what puts the values into the widgets; Show() consumes _pending and
    // republishes.
    Show(_preset);
}

void ParamPanel::ResetAll() {
    _pending.clear();
    Show(_preset);
}
