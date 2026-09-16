/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

The preset picker.

3328 presets is too many for a list and almost too many for a tree, so the tree is the one the
catalogue ids already describe -- "crt/crt-geom", "bezel/Mega_Bezel/Presets/..." -- split on '/',
and a filter box narrows it. Filtering hides leaves that do not match and then any branch left with
nothing under it, because a tree of empty folders is worse than no tree.

The catalogue is read through the same loader the layer uses (layer/src/catalogue.h): four symbols
out of libShaderGlassPresets.so. If it cannot be opened the widget says so in place of the tree,
rather than presenting an empty picker that looks like a catalogue with nothing in it.
*/

#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

class PresetTree : public QWidget {
    Q_OBJECT
  public:
    explicit PresetTree(QWidget* parent = nullptr);

    // Select by catalogue id, expanding whatever is needed to show it. Does nothing if the id is
    // not in the catalogue.
    void Select(const QString& id);

    QString Current() const;
    int Count() const { return _count; }
    bool Usable() const { return _count > 0; }

  signals:
    void presetChosen(const QString& id);

  private:
    void Populate();
    void ApplyFilter(const QString& text);
    static bool FilterItem(QTreeWidgetItem* item, const QString& lowered);

    QLineEdit* _filter = nullptr;
    QTreeWidget* _tree = nullptr;
    QLabel* _status = nullptr;
    int _count = 0;
};
