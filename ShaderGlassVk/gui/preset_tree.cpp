/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "preset_tree.h"

#include "../layer/src/catalogue.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

// The id is the path; the last component is what the user reads.
constexpr int kIdRole = Qt::UserRole + 1;

}  // namespace

PresetTree::PresetTree(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    _filter = new QLineEdit(this);
    _filter->setPlaceholderText(tr("Filter presets"));
    _filter->setClearButtonEnabled(true);
    layout->addWidget(_filter);

    _tree = new QTreeWidget(this);
    _tree->setHeaderHidden(true);
    _tree->setUniformRowHeights(true);  // 3328 rows: let Qt skip measuring each one
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(_tree, 1);

    _status = new QLabel(this);
    _status->setWordWrap(true);
    _status->hide();
    layout->addWidget(_status);

    Populate();

    connect(_filter, &QLineEdit::textChanged, this, &PresetTree::ApplyFilter);
    connect(_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (!item) return;
                const QString id = item->data(0, kIdRole).toString();
                if (!id.isEmpty()) emit presetChosen(id);
            });
}

void PresetTree::Populate() {
    using shaderglass::Catalogue;
    Catalogue& catalogue = Catalogue::Instance();

    const size_t count = catalogue.Count();
    if (!count) {
        _tree->hide();
        _filter->hide();
        _status->setText(tr("No preset catalogue.\n\n%1\n\nBuild one with "
                            "tools/build-catalogue.sh, or point SHADERGLASS_PRESETS at an existing "
                            "libShaderGlassPresets.so.")
                             .arg(QString::fromUtf8(catalogue.Reason())));
        _status->show();
        return;
    }

    // One branch per path component, reused across presets that share a prefix.
    QHash<QString, QTreeWidgetItem*> branches;
    std::function<QTreeWidgetItem*(const QString&)> branchFor;
    branchFor = [&](const QString& path) -> QTreeWidgetItem* {
        if (path.isEmpty()) return nullptr;
        auto it = branches.find(path);
        if (it != branches.end()) return it.value();

        const int slash = path.lastIndexOf(QLatin1Char('/'));
        QTreeWidgetItem* parent = slash > 0 ? branchFor(path.left(slash)) : nullptr;
        const QString name = slash > 0 ? path.mid(slash + 1) : path;

        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_tree);
        item->setText(0, name);
        branches.insert(path, item);
        return item;
    };

    _tree->setUpdatesEnabled(false);
    for (size_t i = 0; i < count; ++i) {
        const SgPreset* preset = catalogue.At(i);
        if (!preset || !preset->id) continue;

        const QString id = QString::fromUtf8(preset->id);
        const int slash = id.lastIndexOf(QLatin1Char('/'));
        QTreeWidgetItem* parent = slash > 0 ? branchFor(id.left(slash)) : nullptr;

        auto* leaf = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(_tree);
        leaf->setText(0, slash > 0 ? id.mid(slash + 1) : id);
        leaf->setData(0, kIdRole, id);
        leaf->setToolTip(0, tr("%1\n%2 pass%3")
                                .arg(id)
                                .arg(preset->pass_count)
                                .arg(preset->pass_count == 1 ? QString() : QStringLiteral("es")));
        ++_count;
    }
    _tree->sortItems(0, Qt::AscendingOrder);
    _tree->setUpdatesEnabled(true);
}

void PresetTree::ApplyFilter(const QString& text) {
    const QString lowered = text.trimmed().toLower();

    _tree->setUpdatesEnabled(false);
    for (int i = 0; i < _tree->topLevelItemCount(); ++i) FilterItem(_tree->topLevelItem(i), lowered);
    _tree->setUpdatesEnabled(true);

    // Typing is a search; expanding everything is what makes the matches visible. Clearing the box
    // collapses again, so the tree does not stay blown open afterwards.
    if (lowered.isEmpty())
        _tree->collapseAll();
    else
        _tree->expandAll();
}

// Returns whether anything at or under this item survived the filter.
bool PresetTree::FilterItem(QTreeWidgetItem* item, const QString& lowered) {
    bool any = false;
    for (int i = 0; i < item->childCount(); ++i)
        if (FilterItem(item->child(i), lowered)) any = true;

    const bool self = lowered.isEmpty() || item->text(0).toLower().contains(lowered) ||
                      item->data(0, kIdRole).toString().toLower().contains(lowered);

    // A branch stays if anything under it stayed, even when its own name does not match.
    const bool keep = self || any;
    item->setHidden(!keep);
    return keep;
}

void PresetTree::Select(const QString& id) {
    if (id.isEmpty()) return;
    for (QTreeWidgetItemIterator it(_tree); *it; ++it) {
        if ((*it)->data(0, kIdRole).toString() != id) continue;
        _tree->setCurrentItem(*it);
        _tree->scrollToItem(*it, QAbstractItemView::PositionAtCenter);
        return;
    }
}

QString PresetTree::Current() const {
    QTreeWidgetItem* item = _tree->currentItem();
    return item ? item->data(0, kIdRole).toString() : QString();
}
