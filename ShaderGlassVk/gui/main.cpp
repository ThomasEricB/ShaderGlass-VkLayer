/*
ShaderGlassVk: ShaderGlass on a Vulkan layer
Copyright (C) 2026 Thomas Eric, bmitch87
GNU General Public License v3.0
*/

#include "mainwindow.h"

#include <QApplication>
#include <QPixmap>
#include <QTabWidget>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ShaderGlass"));
    QCoreApplication::setApplicationName(QStringLiteral("ShaderGlassVk"));

    MainWindow window;
    window.show();

    // SHADERGLASS_GUI_GRAB=<path> renders the window once and exits. The interface is the one part
    // of this tree that cannot be checked by running it and reading a log, and a screenshot taken
    // by hand is not something a build can do; this makes "does it lay out" answerable the same way
    // everything else here is.
    if (const QByteArray grab = qgetenv("SHADERGLASS_GUI_GRAB"); !grab.isEmpty()) {
        // SHADERGLASS_GUI_TAB picks which tab to show first, so every tab can be checked.
        if (const QByteArray tab = qgetenv("SHADERGLASS_GUI_TAB"); !tab.isEmpty())
            if (auto* tabs = window.findChild<QTabWidget*>()) tabs->setCurrentIndex(tab.toInt());

        QTimer::singleShot(400, &window, [&window, grab] {
            window.grab().save(QString::fromUtf8(grab));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
