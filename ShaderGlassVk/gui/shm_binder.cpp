/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Derived from DLSS5VKLayer (relicensed to GPL-3.0, see RELICENSE.md).
*/

#include "shm_binder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWidget>

using shaderglass::ShmHeader;

QString FormatTip(const QString& text) {
    if (text.isEmpty() || !text.contains(QLatin1Char('\n'))) return text;
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"))
        .replace(QLatin1Char('<'), QLatin1String("&lt;"))
        .replace(QLatin1Char('>'), QLatin1String("&gt;"));
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"))
        .replace(QLatin1Char('\r'), QLatin1String("\n"))
        .replace(QLatin1Char('\n'), QLatin1String("<br>"));
    return out;
}

// Each Add* pulls its own value out of the header the moment the control exists, so a control is
// never showing something the header does not say. Relying on a single Reload() at the end of the
// panel worked only for as long as nobody added a control after it, which is the kind of ordering
// dependency that fails quietly and much later.
ShmBinder::ShmBinder(ShmHeader* header, QWidget* parent)
    : QObject(parent), _hdr(header), _parent(parent) {}

void ShmBinder::Write(Field field, uint32_t raw, Latch latch) {
    if (!_hdr || _reloading) return;
    (_hdr->*field).store(raw, std::memory_order_relaxed);
    _hdr->controlSeq.fetch_add(1, std::memory_order_release);
    if (latch == AtBuild) _hdr->tuningSeq.fetch_add(1, std::memory_order_release);
}

QCheckBox* ShmBinder::AddBool(QFormLayout* form, const char* key, const QString& label,
                              Field field, const QString& tip, Latch latch, bool invert) {
    auto* w = new QCheckBox(label, _parent);
    w->setToolTip(FormatTip(tip));
    form->addRow(w);
    connect(w, &QCheckBox::toggled, this, [this, field, latch, invert](bool on) {
        Write(field, (invert ? !on : on) ? 1u : 0u, latch);
    });
    _reloaders.push_back([this, w, field, invert] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const bool v = (_hdr->*field).load(std::memory_order_relaxed) != 0;
        w->setChecked(invert ? !v : v);
    });
    _reloaders.back()();
    _fields.push_back({QString::fromLatin1(key), field});
    return w;
}

QSpinBox* ShmBinder::AddInt(QFormLayout* form, const char* key, const QString& label, Field field,
                            int lo, int hi, const QString& tip, Latch latch) {
    auto* w = new QSpinBox(_parent);
    w->setRange(lo, hi);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this, field, latch](int v) { Write(field, uint32_t(v < 0 ? 0 : v), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        w->setValue(int((_hdr->*field).load(std::memory_order_relaxed)));
    });
    _reloaders.back()();
    _fields.push_back({QString::fromLatin1(key), field});
    return w;
}

QDoubleSpinBox* ShmBinder::AddFloat(QFormLayout* form, const char* key, const QString& label,
                                    Field field, double lo, double hi, double step,
                                    const QString& tip, Latch latch) {
    auto* w = new QDoubleSpinBox(_parent);
    w->setRange(lo, hi);
    w->setSingleStep(step);
    w->setDecimals(step < 0.01 ? 3 : 2);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this, field, latch](double v) {
                Write(field, shaderglass::ShmFloatBits(float(v)), latch);
            });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        w->setValue(double(
            shaderglass::ShmBitsFloat((_hdr->*field).load(std::memory_order_relaxed))));
    });
    _reloaders.back()();
    _fields.push_back({QString::fromLatin1(key), field});
    return w;
}

QComboBox* ShmBinder::AddChoice(QFormLayout* form, const char* key, const QString& label,
                                Field field, const QStringList& options, const QString& tip,
                                Latch latch) {
    auto* w = new QComboBox(_parent);
    w->addItems(options);
    w->setToolTip(FormatTip(tip));
    form->addRow(label, w);
    connect(w, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, field, latch](int i) { Write(field, uint32_t(i < 0 ? 0 : i), latch); });
    _reloaders.push_back([this, w, field] {
        if (!_hdr) return;
        QSignalBlocker block(w);
        const int v = int((_hdr->*field).load(std::memory_order_relaxed));
        w->setCurrentIndex(v < w->count() ? v : 0);
    });
    _reloaders.back()();
    _fields.push_back({QString::fromLatin1(key), field});
    return w;
}

void ShmBinder::Reload() {
    _reloading = true;
    for (auto& r : _reloaders) r();
    _reloading = false;
}

void ShmBinder::ApplyBlob(const QString& blob) {
    if (!_hdr) return;
    for (const QString& line : blob.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString key = line.left(eq);
        for (const auto& f : _fields) {
            if (f.first != key) continue;
            (_hdr->*f.second).store(line.mid(eq + 1).toUInt(), std::memory_order_relaxed);
            break;
        }
    }
    // Everything at once: one bump rather than one per setting, and a rebuild because a profile
    // can move the settings the chain is built from.
    _hdr->controlSeq.fetch_add(1, std::memory_order_release);
    _hdr->tuningSeq.fetch_add(1, std::memory_order_release);
    Reload();
}

QString ShmBinder::Blob() const {
    if (!_hdr) return {};
    QString out;
    for (const auto& f : _fields) {
        out += f.first;
        out += QLatin1Char('=');
        out += QString::number((_hdr->*f.second).load(std::memory_order_relaxed));
        out += QLatin1Char('\n');
    }
    return out;
}
