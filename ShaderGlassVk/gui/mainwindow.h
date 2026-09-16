/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0

Derived in shape from DLSS5VKLayer's settings window (relicensed to GPL-3.0, see RELICENSE.md).

The interface is a second process that shares a file mapping with the layer. It does not launch the
game, attach to it, or know anything about it beyond what the layer writes into that mapping -- so
it can be started and stopped at any point, including while a game is running, and the game neither
notices nor cares.

Tabs are Shader and Input for now. Output and Advanced arrive in phase 6 alongside the layer code
that honours them; a tab of controls the layer ignores would look like a bug rather than a plan.
*/

#pragma once

#include "../common/shm_protocol.h"

#include <QString>
#include <QWidget>

class ParamPanel;
class PresetTree;
class ShmBinder;
class CaptureView;

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

class MainWindow : public QWidget {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    bool AttachShm();
    QWidget* BuildShaderTab();
    QWidget* BuildInputTab();
    void UpdateStatus();
    void ChoosePreset(const QString& id);

    QString DataDir() const;
    QString ProfilesDir() const;
    QString ConfigPath() const;
    void LoadConfig();
    void SaveConfig();
    void RefreshProfiles();
    void SaveProfile();
    void LoadProfile(const QString& name);
    QString Blob() const;

    QString _shmPath;
    void* _shmBase = nullptr;
    shaderglass::ShmHeader* _hdr = nullptr;

    PresetTree* _presets = nullptr;
    ParamPanel* _params = nullptr;
    CaptureView* _capture = nullptr;
    ShmBinder* _binder = nullptr;

    QCheckBox* _enabled = nullptr;
    QCheckBox* _paused = nullptr;
    QComboBox* _profiles = nullptr;
    QLabel* _status = nullptr;
    QLabel* _presetLabel = nullptr;
    QTimer* _timer = nullptr;

    QString _currentPreset;
    bool _captureVisible = false;
    uint32_t _lastHeartbeat = 0;
    int _idleTicks = 0;
};
