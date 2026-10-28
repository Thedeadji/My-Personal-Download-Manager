// MPDM desktop GUI entry point.
#include <QApplication>
#include <QFontDatabase>
#include <QTimer>

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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--export-png") == 0 && i + 1 < argc) {
            QString dir = argv[i + 1];
            for (int s : {16, 24, 32, 48, 64, 128, 256})
                mpdm::gui::Logo::pixmap(s).save(QString("%1/shield_%2.png").arg(dir).arg(s), "PNG");
            return 0;
        }
    }

    mpdm::gui::MainWindow w;
    // Dev: MPDM_WIN_SIZE=1280x720 forces a window size (resolution testing).
    if (const char* ws = std::getenv("MPDM_WIN_SIZE")) {
        int ww = 0, hh = 0;
        if (std::sscanf(ws, "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0) w.resize(ww, hh);
    }
    w.show();

    // Dev: `--shot <path>` renders the window; `--shot-settings`/`--shot-scheduler`
    // open the dialog and render it. Then quit.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            QString out = argv[i + 1];
            QTimer::singleShot(900, [&w, out] { w.grab().save(out); qApp->quit(); });
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
