/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "mainwindow.h"

#include "capture_view.h"
#include "game_probe.h"
#include "param_panel.h"
#include "preset_tree.h"
#include "shm_binder.h"

#include "../layer/src/catalogue.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace shaderglass;

namespace {

// The mapping lives where the layer puts it, which is under /tmp rather than $XDG_RUNTIME_DIR for
// the reason README.md gives: a Steam game runs inside pressure-vessel, whose private tmpfs makes
// a runtime-dir mapping invisible from inside the game.
QString DefaultShmPath() {
    if (const char* env = qgetenv("SHADERGLASS_SHM"); env && *env) return QString::fromUtf8(env);
    return QStringLiteral("/tmp/shaderglass-%1/shm.bin").arg(getuid());
}

QString HumanState(uint32_t state) {
    switch (state) {
        case kLayerDetached: return QObject::tr("not attached");
        case kLayerIdle: return QObject::tr("idle");
        case kLayerActive: return QObject::tr("running");
        case kLayerFailed: return QObject::tr("failed");
        default: return QObject::tr("unknown");
    }
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("ShaderGlass"));

    _shmPath = DefaultShmPath();
    AttachShm();

    auto* outer = new QVBoxLayout(this);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(BuildShaderTab(), tr("Shader"));
    tabs->addTab(BuildInputTab(), tr("Input"));

    _capture = new CaptureView(this);
    const int captureTab = tabs->addTab(_capture, tr("Capture"));
    outer->addWidget(tabs, 1);

    // Look for a new pair when the tab is opened, and while it is open -- a capture lands a frame
    // or two after the button is pressed, and the interface has no way to be told when.
    connect(tabs, &QTabWidget::currentChanged, this, [this, captureTab](int index) {
        _captureVisible = (index == captureTab);
        if (_captureVisible) _capture->Refresh(QString::fromStdString(ShmCaptureDir()));
    });

    // --- the row every tab shares -------------------------------------------------------------
    auto* bottom = new QHBoxLayout;

    _enabled = new QCheckBox(tr("Enabled"), this);
    _enabled->setToolTip(FormatTip(tr("Run the shader chain at all.\n"
                                      "With this off, or with no preset chosen, the game's own "
                                      "frame is presented untouched.")));
    bottom->addWidget(_enabled);

    _paused = new QCheckBox(tr("Paused"), this);
    _paused->setToolTip(FormatTip(tr("Freeze the chain on the frame it last composed.\n"
                                     "Settings still apply, so a parameter can be adjusted against "
                                     "a still picture.")));
    bottom->addWidget(_paused);

    bottom->addStretch(1);
    bottom->addWidget(new QLabel(tr("Profile:"), this));

    _profiles = new QComboBox(this);
    _profiles->setMinimumWidth(160);
    bottom->addWidget(_profiles);

    auto* save = new QPushButton(tr("Save…"), this);
    bottom->addWidget(save);

    auto* capture = new QPushButton(tr("Capture"), this);
    capture->setToolTip(FormatTip(tr("Ask the layer for one matched pair of frames: what the game "
                                     "presented, and what it presented after the chain ran.\n"
                                     "They appear on the Capture tab.")));
    bottom->addWidget(capture);

    outer->addLayout(bottom);

    _status = new QLabel(this);
    _status->setTextFormat(Qt::PlainText);
    outer->addWidget(_status);

    // Hidden until there is something to say. It carries launch options the user is meant to copy,
    // so the text is selectable and the label wraps rather than widening the window.
    _notice = new QLabel(this);
    _notice->setTextFormat(Qt::RichText);
    _notice->setWordWrap(true);
    _notice->setTextInteractionFlags(Qt::TextBrowserInteraction);
    _notice->setOpenExternalLinks(true);
    _notice->setStyleSheet(QStringLiteral("QLabel { border: 1px solid palette(mid);"
                                          " border-radius: 4px; padding: 6px; }"));
    _notice->hide();
    outer->addWidget(_notice);

    // --- wiring ---------------------------------------------------------------------------------
    if (_hdr) {
        _enabled->setChecked(ShmEnabled(_hdr));
        _paused->setChecked(_hdr->paused.load(std::memory_order_relaxed) != 0);
    }
    connect(_enabled, &QCheckBox::toggled, this, [this](bool on) {
        if (!_hdr) return;
        _hdr->enabled.store(on ? 1u : 0u, std::memory_order_relaxed);
        _hdr->controlSeq.fetch_add(1, std::memory_order_release);
    });
    connect(_paused, &QCheckBox::toggled, this, [this](bool on) {
        if (!_hdr) return;
        _hdr->paused.store(on ? 1u : 0u, std::memory_order_relaxed);
        _hdr->controlSeq.fetch_add(1, std::memory_order_release);
    });
    connect(save, &QPushButton::clicked, this, &MainWindow::SaveProfile);
    connect(_profiles, &QComboBox::currentTextChanged, this, &MainWindow::LoadProfile);
    connect(capture, &QPushButton::clicked, this, [this] {
        if (!_hdr) return;
        _hdr->captureRequest.fetch_add(1, std::memory_order_release);
        _hdr->controlSeq.fetch_add(1, std::memory_order_release);
        _status->setText(tr("Capture requested; it lands on the next frame the chain composes."));
    });

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &MainWindow::UpdateStatus);
    _timer->start(500);

    RefreshProfiles();
    LoadConfig();
    UpdateStatus();
    resize(940, 680);
}

MainWindow::~MainWindow() {
    if (_shmBase) munmap(_shmBase, ShmTotalBytes());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    SaveConfig();
    QWidget::closeEvent(event);
}

// The interface creates the mapping if it is not there yet, so settings can be chosen before a game
// is started; the layer does the same from its side, and whichever gets there first initialises it.
bool MainWindow::AttachShm() {
    QDir().mkpath(QFileInfo(_shmPath).absolutePath());

    // ::open and ::close, because QWidget has a close() of its own that would win here.
    const int fd = ::open(_shmPath.toUtf8().constData(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) return false;

    struct stat st {};
    if (fstat(fd, &st) != 0 || size_t(st.st_size) < ShmTotalBytes()) {
        if (ftruncate(fd, off_t(ShmTotalBytes())) != 0) {
            ::close(fd);
            return false;
        }
    }

    void* base = mmap(nullptr, ShmTotalBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED) return false;

    _shmBase = base;
    _hdr = static_cast<ShmHeader*>(base);

    // Only when there is nothing usable there. ShmInitDefaults memsets the whole header -- every
    // setting, the chosen preset, and the layer's status with them -- so calling it unconditionally
    // means opening the interface wipes the state of a game that is already running. It did: the
    // game then read an empty preset on every frame and composed nothing, which looks exactly like
    // an interface whose preset list does not work.
    //
    // Same test the layer uses, for the same reason: a mapping left by an older build has a
    // different magic or version, and re-initialising is the only safe reading of either.
    if (_hdr->magic.load() != kShmMagic || _hdr->version.load() != kShmVersion)
        ShmInitDefaults(_hdr);

    return true;
}

QWidget* MainWindow::BuildShaderTab() {
    auto* split = new QSplitter(Qt::Horizontal, this);

    _presets = new PresetTree(split);
    split->addWidget(_presets);

    auto* right = new QWidget(split);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    _presetLabel = new QLabel(tr("No preset selected."), right);
    _presetLabel->setWordWrap(true);
    rightLayout->addWidget(_presetLabel);

    _params = new ParamPanel(right);
    _params->SetHeader(_hdr);
    rightLayout->addWidget(_params, 1);

    auto* row = new QHBoxLayout;
    auto* none = new QPushButton(tr("No preset"), right);
    none->setToolTip(tr("Present the game's own frame untouched."));
    row->addWidget(none);
    auto* reset = new QPushButton(tr("Reset parameters"), right);
    row->addWidget(reset);
    row->addStretch(1);
    rightLayout->addLayout(row);

    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    connect(_presets, &PresetTree::presetChosen, this, &MainWindow::ChoosePreset);
    connect(none, &QPushButton::clicked, this, [this] { ChoosePreset(QString()); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (_params) _params->ResetAll();
    });
    return split;
}

QWidget* MainWindow::BuildInputTab() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    _binder = new ShmBinder(_hdr, page);

    _binder->AddChoice(form, "sourcemode", tr("Source resolution"), &ShmHeader::sourceMode,
                       {tr("Native"), tr("Divisor"), tr("Classic raster"), tr("Custom")},
                       tr("What raster the shader is shown.\n"
                          "Native gives it the game's own frame. The others reduce it first, which "
                          "is what a CRT shader authored for a console expects -- at Native its "
                          "mask and scanlines land on pixels far smaller than it assumes."),
                       ShmBinder::AtBuild);

    _binder->AddInt(form, "sourcedivisor", tr("Divisor"), &ShmHeader::sourceDivisor, 2, 8,
                    tr("Halve, third, quarter and so on, for the Divisor mode."),
                    ShmBinder::AtBuild);

    _binder->AddInt(form, "sourcewidth", tr("Source width"), &ShmHeader::sourceWidth, 0, 16384,
                    tr("For the Classic raster and Custom modes."), ShmBinder::AtBuild);
    _binder->AddInt(form, "sourceheight", tr("Source height"), &ShmHeader::sourceHeight, 0, 16384,
                    tr("For the Classic raster and Custom modes."), ShmBinder::AtBuild);

    auto* autoBox = _binder->AddBool(form, "autosource", tr("Detect automatically"),
                     &ShmHeader::autoSourceEnabled,
                     tr("Measure the game's own raster out of the frame and use that, instead of "
                        "the mode above.\n"
                        "A pixel-art game upscaled by a fraction -- 320x224 stretched to fill a "
                        "2562x1082 window is x8.01 across and x4.83 down -- has no divisor on the "
                        "list that fits it, and the nearest integer is not a near miss but a "
                        "different grid. Measuring finds the fractional answer.\n"
                        "Re-measured every few seconds, so a game that changes resolution is "
                        "followed."),
                     ShmBinder::AtBuild);

    // Turning it on means "measure what is on screen now". Without this the layer would put back
    // whatever it measured for an earlier scene, which is what made the toggle feel inert.
    connect(autoBox, &QCheckBox::toggled, this, [this](bool) { RequestSourceRefresh(); });

    auto* redetect = new QPushButton(tr("Measure again now"), page);
    redetect->setToolTip(FormatTip(tr("Take a fresh measurement of the frame on screen.\n"
                                      "It is measured again every couple of seconds anyway; this "
                                      "is for when the scene has just changed and waiting is "
                                      "tiresome.")));
    connect(redetect, &QPushButton::clicked, this, [this] { RequestSourceRefresh(); });
    form->addRow(QString(), redetect);

    _autoSourceLabel = new QLabel(page);
    _autoSourceLabel->setWordWrap(true);
    _autoSourceLabel->setEnabled(false);
    form->addRow(QString(), _autoSourceLabel);

    auto* note = new QLabel(
        tr("Pixel size, output policy, aspect, crop and frame skip arrive with the layer code that "
           "honours them."),
        page);
    note->setWordWrap(true);
    note->setEnabled(false);
    form->addRow(note);

    return page;
}

// Ask the layer to throw away what it measured and measure again. A counter rather than a flag: a
// flag would need clearing by whoever read it, and two presses before the next frame would be one
// request.
void MainWindow::RequestSourceRefresh() {
    if (!_hdr) return;
    _hdr->autoSourceRefresh.fetch_add(1, std::memory_order_release);
    _hdr->controlSeq.fetch_add(1, std::memory_order_release);
}

void MainWindow::ChoosePreset(const QString& id) {
    _currentPreset = id;

    if (_hdr) {
        // ShmStoreString bumps presetSeq itself, which is the sequence a reader checks either
        // side of the id.
        ShmStoreString(_hdr->presetSeq, _hdr->presetId, kPresetIdBytes, id.toUtf8().constData());
        _hdr->controlSeq.fetch_add(1, std::memory_order_release);
        _hdr->tuningSeq.fetch_add(1, std::memory_order_release);
    }

    const SgPreset* preset = id.isEmpty() ? nullptr : Catalogue::Instance().Find(id.toStdString());
    if (_params) _params->Show(preset);

    if (!preset) {
        _presetLabel->setText(id.isEmpty() ? tr("No preset selected.")
                                           : tr("'%1' is not in the catalogue.").arg(id));
        return;
    }
    _presetLabel->setText(tr("<b>%1</b><br>%2 pass%3, %4 texture%5")
                              .arg(id)
                              .arg(preset->pass_count)
                              .arg(preset->pass_count == 1 ? QString() : QStringLiteral("es"))
                              .arg(preset->texture_count)
                              .arg(preset->texture_count == 1 ? QString() : QStringLiteral("s")));
    _presetLabel->setTextFormat(Qt::RichText);
}

void MainWindow::UpdateStatus() {
    if (!_hdr) {
        _status->setText(tr("No shared mapping at %1").arg(_shmPath));
        return;
    }

    const uint32_t beat = _hdr->layerHeartbeat.load(std::memory_order_relaxed);
    if (beat == _lastHeartbeat)
        ++_idleTicks;
    else
        _idleTicks = 0;
    _lastHeartbeat = beat;

    const uint32_t state = _hdr->layerState.load(std::memory_order_relaxed);
    // A layer that has stopped writing has not told us it went away -- the game exited, or it was
    // killed. Two seconds of silence is long enough to say so and short enough to be useful.
    const bool stale = _idleTicks > 4;

    QString text = tr("Layer: %1").arg(stale ? tr("not running") : HumanState(state));

    const QString game =
        QString::fromStdString(ShmLoadString(_hdr->gameNameSeq, _hdr->gameName, kNameBytes));
    if (!game.isEmpty() && !stale) text += tr("  ·  %1").arg(game);

    if (state == kLayerActive && !stale) {
        text += tr("  ·  %1×%2")
                    .arg(_hdr->swapWidth.load(std::memory_order_relaxed))
                    .arg(_hdr->swapHeight.load(std::memory_order_relaxed));
        const uint32_t sw = _hdr->sourceActualWidth.load(std::memory_order_relaxed);
        const uint32_t sh = _hdr->sourceActualHeight.load(std::memory_order_relaxed);
        if (sw && sh) text += tr(" from %1×%2").arg(sw).arg(sh);
        text += tr("  ·  %1 pass%2")
                    .arg(_hdr->passCount.load(std::memory_order_relaxed))
                    .arg(_hdr->passCount.load(std::memory_order_relaxed) == 1
                             ? QString()
                             : QStringLiteral("es"));
        const float fps = ShmBitsFloat(_hdr->fpsBits.load(std::memory_order_relaxed));
        if (fps > 0.0f) text += tr("  ·  %1 fps").arg(double(fps), 0, 'f', 1);
    }

    // Only while the layer is actually writing. The reason is the last thing a layer said, and a
    // layer that has gone away leaves its last words behind -- showing them beside "not running"
    // reads as a live diagnosis of a game that is no longer being watched, which is worse than
    // saying nothing.
    if (!stale) {
        const QString reason = QString::fromStdString(
            ShmLoadString(_hdr->layerReasonSeq, _hdr->layerReason, kReasonBytes));
        if (!reason.isEmpty()) text += tr("  ·  %1").arg(reason);
    }

    _status->setText(text);
    UpdateNotice(stale, game);

    if (_autoSourceLabel) {
        if (!_hdr->autoSourceEnabled.load(std::memory_order_relaxed)) {
            _autoSourceLabel->clear();
        } else {
            const uint32_t aw = _hdr->autoSourceWidth.load(std::memory_order_relaxed);
            const uint32_t ah = _hdr->autoSourceHeight.load(std::memory_order_relaxed);
            if (!aw || !ah) {
                _autoSourceLabel->setText(
                    stale ? tr("Nothing measured: no game is running.")
                          : tr("Measuring\u2026 nothing found yet. A frame with no pixel grid "
                               "\u2014 a movie, a 3D scene, a blank menu \u2014 is left alone."));
            } else {
                _autoSourceLabel->setText(
                    tr("Measured %1\u00d7%2, from \u00d7%3 across and \u00d7%4 down "
                       "(confidence %5).")
                        .arg(aw)
                        .arg(ah)
                        .arg(double(ShmBitsFloat(
                                 _hdr->autoSourceScaleXBits.load(std::memory_order_relaxed))),
                             0, 'f', 2)
                        .arg(double(ShmBitsFloat(
                                 _hdr->autoSourceScaleYBits.load(std::memory_order_relaxed))),
                             0, 'f', 2)
                        .arg(double(ShmBitsFloat(
                                 _hdr->autoSourceConfidenceBits.load(std::memory_order_relaxed))),
                             0, 'f', 2));
            }
        }
    }

    if (_captureVisible && _capture) _capture->Refresh(QString::fromStdString(ShmCaptureDir()));
}

// A game that stops presenting looks exactly like a game that exited, from the mapping alone. It is
// worth telling the two apart: the second is nothing to report, and the first is usually a game
// that has handed the screen to OpenGL, which the layer cannot see but Zink can hand back.
void MainWindow::UpdateNotice(bool stale, const QString& game) {
    if (!stale) {
        _noticePid = 0;
        _notice->hide();
        return;
    }

    const uint32_t pid = _hdr->layerPid.load(std::memory_order_relaxed);
    if (!pid || !ProcessAlive(pid)) {  // the ordinary end of a game
        _noticePid = 0;
        _notice->hide();
        return;
    }
    if (pid == _noticePid) return;  // already answered for this game
    _noticePid = pid;

    const RenderApis apis = ProbeRenderApis(pid);
    const QString who = game.isEmpty() ? tr("The game") : game;

    if (apis.RendersWithOpenGL()) {
        _notice->setText(
            tr("<b>%1 is rendering with OpenGL.</b><br>"
               "ShaderGlass only sees Vulkan frames, so it cannot shade this game as it stands "
               "&mdash; which is why the effect stops once the game leaves its Vulkan intro.<br><br>"
               "Zink routes OpenGL through Vulkan. Put this in front of "
               "<code>%%command%%</code> in the launch options:<br>"
               "<code>%2</code><br><br>"
               "Running the game inside gamescope works too, and shades its composited output "
               "whatever the game renders with.")
                .arg(who.toHtmlEscaped(), QString::fromStdString(ZinkLaunchOptions())));
    } else {
        _notice->setText(tr("<b>%1 has stopped presenting through Vulkan.</b><br>"
                            "It is still running, so the effect will come back on its own if the "
                            "game is only minimised or paused.")
                            .arg(who.toHtmlEscaped()));
    }
    _notice->show();
}

// --- configuration and profiles ---------------------------------------------------------------

QString MainWindow::DataDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString MainWindow::ProfilesDir() const { return DataDir() + QStringLiteral("/profiles"); }

QString MainWindow::ConfigPath() const { return DataDir() + QStringLiteral("/shaderglass.conf"); }

QString MainWindow::Blob() const {
    QString out = QStringLiteral("preset=%1\n").arg(_currentPreset);
    if (_binder) out += _binder->Blob();
    if (_params) out += QStringLiteral("[parameters]\n") + _params->Blob();
    return out;
}

void MainWindow::SaveProfile() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Save profile"), tr("Name:"), QLineEdit::Normal,
                              _currentPreset.section(QLatin1Char('/'), -1), &ok);
    if (!ok || name.isEmpty()) return;

    QDir().mkpath(ProfilesDir());
    QFile file(ProfilesDir() + QStringLiteral("/") + name + QStringLiteral(".conf"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        _status->setText(tr("Could not write %1").arg(file.fileName()));
        return;
    }
    QTextStream(&file) << Blob();
    file.close();

    RefreshProfiles();
    _profiles->setCurrentText(name);
}

void MainWindow::RefreshProfiles() {
    const QString current = _profiles->currentText();
    QSignalBlocker block(_profiles);
    _profiles->clear();
    _profiles->addItem(QString());
    for (const QFileInfo& f :
         QDir(ProfilesDir()).entryInfoList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name))
        _profiles->addItem(f.completeBaseName());
    _profiles->setCurrentText(current);
}

void MainWindow::LoadProfile(const QString& name) {
    if (name.isEmpty()) return;

    QFile file(ProfilesDir() + QStringLiteral("/") + name + QStringLiteral(".conf"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QString blob = QString::fromUtf8(file.readAll());
    file.close();

    const int split = blob.indexOf(QStringLiteral("[parameters]\n"));
    const QString head = split < 0 ? blob : blob.left(split);
    const QString params = split < 0 ? QString() : blob.mid(split + 13);

    // The preset first: the parameter panel is built from it, so it has to exist before the
    // parameter overrides can land anywhere.
    for (const QString& line : head.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!line.startsWith(QStringLiteral("preset="))) continue;
        const QString id = line.mid(7);
        if (_presets) _presets->Select(id);
        ChoosePreset(id);
        break;
    }
    if (_params) _params->ApplyBlob(params);
    if (_binder) _binder->ApplyBlob(head);
}

void MainWindow::LoadConfig() {
    QSettings settings(ConfigPath(), QSettings::IniFormat);
    const QString preset = settings.value(QStringLiteral("preset")).toString();
    if (!preset.isEmpty()) {
        if (_presets) _presets->Select(preset);
        ChoosePreset(preset);
    }
    const QSize size = settings.value(QStringLiteral("size")).toSize();
    if (size.isValid()) resize(size);

    // Last, and deliberately: a profile is an explicit choice and outranks the loose preset above.
    const QString profile = settings.value(QStringLiteral("profile")).toString();
    if (!profile.isEmpty() && _profiles->findText(profile) >= 0)
        _profiles->setCurrentText(profile);  // the signal loads it
}

void MainWindow::SaveConfig() {
    QDir().mkpath(DataDir());
    QSettings settings(ConfigPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("preset"), _currentPreset);
    settings.setValue(QStringLiteral("size"), size());
    settings.setValue(QStringLiteral("profile"), _profiles->currentText());
}
