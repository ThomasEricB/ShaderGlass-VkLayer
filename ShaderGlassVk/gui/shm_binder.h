/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Derived from DLSS5VKLayer (relicensed to GPL-3.0, see RELICENSE.md).

Binds widgets to fields of the shared header.

Table-driven for the same reason shaderglass-ctl is: there are around thirty settings, and
hand-writing a widget, a default, a read-back and a connect() for each is four places to forget
something in. Here a control is one line, and a header field with no line is visibly absent rather
than quietly unreachable.

Two things the binder knows that the widgets do not. Settings the layer latches when it builds the
chain bump tuningSeq as well as controlSeq, which is what tells the layer to rebuild rather than
carry on with a chain made from the old values. And reloading from the header blocks signals while
it writes, so refreshing the interface cannot be mistaken for the user changing something.
*/

#pragma once

#include "../common/shm_protocol.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

// Tooltips carry real prose, and a single unbroken sentence reads as one gigantic line that runs
// off the side of the screen. Callers write the natural break points as '\n'; this turns those into
// hard breaks the tooltip will honour, escaping the text first so a stray '&' or '<' in the prose
// cannot be mistaken for markup once the string becomes rich text. A tooltip with no '\n' is left
// as plain text, so short one-liners are untouched and single-line.
QString FormatTip(const QString& text);

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSpinBox;
class QWidget;

class ShmBinder : public QObject {
    Q_OBJECT
  public:
    using Field = std::atomic<uint32_t> shaderglass::ShmHeader::*;

    // Whether the layer reads this when it builds the chain. Changing one of these costs a rebuild,
    // so the layer has to be told; changing anything else takes effect on the next frame.
    enum Latch { Live, AtBuild };

    ShmBinder(shaderglass::ShmHeader* header, QWidget* parent);

    void SetHeader(shaderglass::ShmHeader* header) { _hdr = header; }

    // invert: the checkbox reads as the opposite of the stored bit -- a box labelled "Run the
    // shader chain" over a field that stores the negative.
    // `key` is the name this setting has in a profile, and it is deliberately the same name
    // shaderglass-ctl uses. Keying a profile off the visible label would tie saved files to the
    // interface's wording and to whatever language it was running in.
    QCheckBox* AddBool(QFormLayout* form, const char* key, const QString& label, Field field,
                       const QString& tip, Latch latch = Live, bool invert = false);
    QSpinBox* AddInt(QFormLayout* form, const char* key, const QString& label, Field field, int lo,
                     int hi, const QString& tip, Latch latch = Live);
    QDoubleSpinBox* AddFloat(QFormLayout* form, const char* key, const QString& label, Field field,
                             double lo, double hi, double step, const QString& tip,
                             Latch latch = Live);
    QComboBox* AddChoice(QFormLayout* form, const char* key, const QString& label, Field field,
                         const QStringList& options, const QString& tip, Latch latch = Live);

    // Pull every bound control's value out of the header. Safe to call at any time.
    void Reload();

    // Every bound field's current value, as "name=value" lines -- what a profile is made of.
    QString Blob() const;

    // The other direction: put a profile's values back into the header and the widgets.
    void ApplyBlob(const QString& blob);

  private:
    void Write(Field field, uint32_t raw, Latch latch);

    shaderglass::ShmHeader* _hdr = nullptr;
    QWidget* _parent = nullptr;
    std::vector<std::function<void()>> _reloaders;
    std::vector<std::pair<QString, Field>> _fields;
    bool _reloading = false;
};
