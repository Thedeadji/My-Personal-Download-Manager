// MPDM desktop GUI entry point.
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSystemTrayIcon>
#include <QTimer>

#include <QAbstractItemView>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mainwindow.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("MyPersonalDownloadManager");
    app.setOrganizationName("MPDM");

    for (const char* f : {":/fonts/fonts/ChakraPetch-Medium.ttf", ":/fonts/fonts/ChakraPetch-SemiBold.ttf",
                          ":/fonts/fonts/Barlow-Regular.ttf", ":/fonts/fonts/Barlow-Medium.ttf",
                          ":/fonts/fonts/JetBrainsMono.ttf"})
        QFontDatabase::addApplicationFont(f);

    // Dev: `--export-png <dir>` writes the shield logo at icon sizes, then quits
    // (used to build the app .ico). Must run before creating the main window.
    // Dev: `--make-check <png>` paints the checkmark glyph (black) to a transparent
    // PNG we bake into resources — a PNG avoids depending on the Qt SVG plugin for
    // stylesheet images. Runs before any window is created.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--make-check") == 0 && i + 1 < argc) {
            const int SZ = 48;
            QPixmap pm(SZ, SZ); pm.fill(Qt::transparent);
            QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing, true);
            QPen pen(QColor("#000000"), 6); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
            g.setPen(pen);
            QPainterPath p; p.moveTo(11, 25); p.lineTo(20, 34); p.lineTo(38, 15);
            g.drawPath(p);
            g.end();
            return pm.save(argv[i + 1], "PNG") ? 0 : 1;
        }
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--export-png") == 0 && i + 1 < argc) {
            QString dir = argv[i + 1];
            for (int s : {16, 24, 32, 48, 64, 128, 256})
                mpdm::gui::Logo::pixmap(s).save(QString("%1/shield_%2.png").arg(dir).arg(s), "PNG");
            return 0;
        }
    }

    bool background = false, isShot = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--background")) background = true;
        if (!std::strncmp(argv[i], "--shot", 6)) isShot = true;
    }

    // Single instance: the native host launches the app for each browser download;
    // if one is already running it adopts the dropped job (jobs/) itself, so a
    // second launch just exits. (Screenshot/dev runs skip this.)
    const QString sockName = "mpdm_gui_singleton";
    if (!isShot) {
        QLocalSocket probe;
        probe.connectToServer(sockName);
        if (probe.waitForConnected(200)) { probe.disconnectFromServer(); return 0; }
        QLocalServer::removeServer(sockName);
        auto* server = new QLocalServer(&app);
        server->listen(sockName);  // held for the app's lifetime
    }

    mpdm::gui::MainWindow w;
    // Dev: MPDM_WIN_SIZE=1280x720 forces a window size (resolution testing).
    if (const char* ws = std::getenv("MPDM_WIN_SIZE")) {
        int ww = 0, hh = 0;
        if (std::sscanf(ws, "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0) w.resize(ww, hh);
    }
    // --background: stay in the tray; jobs open popups. Without a system tray (e.g.
    // stock GNOME) there'd be no way to reach a hidden window, so show it anyway.
    if (!background || !QSystemTrayIcon::isSystemTrayAvailable()) w.show();

    // Dev: `--shot <path>` renders the window; `--shot-settings`/`--shot-scheduler`
    // open the dialog and render it; `--shot-popup[-stream] <png>` renders a popup.
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--shot-popup") == 0 || std::strcmp(argv[i], "--shot-popup-stream") == 0) &&
            i + 1 < argc) {
            w.devShotPopup(argv[i + 1], std::strcmp(argv[i], "--shot-popup-stream") == 0);
        } else if (std::strcmp(argv[i], "--shot-graph") == 0 && i + 1 < argc) {
            w.devShotGraph(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-finish") == 0 && i + 1 < argc) {
            w.devShotFinish(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-traymenu") == 0 && i + 1 < argc) {
            w.devShotTrayMenu(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-quickpanel") == 0 && i + 1 < argc) {
            w.devShotQuickPanel(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-icons") == 0 && i + 1 < argc) {
            w.devShotIcons(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-deselect") == 0 && i + 1 < argc) {
            w.devShotDeselect(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-trayicons") == 0 && i + 1 < argc) {
            w.devShotTrayIcons(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--shot-check") == 0 && i + 1 < argc) {
            // Dev: confirm the checked-state checkmark SVG actually renders.
            QString out = argv[i + 1];
            auto* cb = new QCheckBox("Checked");
            cb->setChecked(true);
            cb->setStyleSheet("QCheckBox{color:#111;font-size:18px;spacing:12px}"
                              "QCheckBox::indicator{width:40px;height:40px;background:#EF9A1E;border:1px solid #EF9A1E;"
                              "image:url(:/icons/check.png)}");
            cb->move(40, 40); cb->resize(240, 60); cb->show();
            QTimer::singleShot(500, [cb, out] { cb->grab().save(out); qApp->quit(); });
        } else if (std::strcmp(argv[i], "--shot-combo") == 0 && i + 1 < argc) {
            // Dev: verify the combo-box popup (its own top-level window) is themed.
            QString out = argv[i + 1];
            auto* cb = new QComboBox;
            cb->setView(new QListView(cb));  // mirror the real settings combos
            cb->addItems({"System", "Light", "Dark"});
            cb->setCurrentIndex(1);
            cb->view()->setStyleSheet(
                "QAbstractItemView{background:#FFFFFF;color:#1C2630;border:1px solid #D3DAE2;outline:0;"
                "selection-background-color:#EF9A1E;selection-color:#3A2708}"
                "QAbstractItemView::item{min-height:26px;padding:4px 10px;color:#1C2630}");
            cb->move(60, 60); cb->resize(220, 36); cb->show();
            QTimer::singleShot(500, [cb] { cb->showPopup(); });
            QTimer::singleShot(1100, [cb, out] {
                QWidget* cont = cb->view() ? cb->view()->parentWidget() : nullptr;
                if (cont) cont->grab().save(out);
                else if (cb->view()) cb->view()->grab().save(out);
                qApp->quit();
            });
        } else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            QString out = argv[i + 1];
            QTimer::singleShot(900, [&w, out] { w.grab().save(out); qApp->quit(); });
        } else if (std::strcmp(argv[i], "--shot-help") == 0 && i + 1 < argc) {
            QString out = argv[i + 1];
            QTimer::singleShot(600, &w, [&w] { w.openHelp(); });
            QTimer::singleShot(1300, [out] { if (auto* m = QApplication::activeModalWidget()) { m->grab().save(out); m->close(); } });
            QTimer::singleShot(1700, [] { qApp->quit(); });
        } else if ((std::strcmp(argv[i], "--shot-settings") == 0 ||
                    std::strcmp(argv[i], "--shot-scheduler") == 0) && i + 1 < argc) {
            bool sched = std::strcmp(argv[i], "--shot-scheduler") == 0;
            QString out = argv[i + 1];
            QTimer::singleShot(600, &w, [&w, sched] { if (sched) w.openScheduler(); else w.openSettings(); });
            QTimer::singleShot(1300, [out] { if (auto* m = QApplication::activeModalWidget()) { m->grab().save(out); m->close(); } });
            QTimer::singleShot(1700, [] { qApp->quit(); });
        }
    }
    return app.exec();
}
