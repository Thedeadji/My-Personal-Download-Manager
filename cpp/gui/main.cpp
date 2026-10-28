// MPDM desktop GUI entry point.
#include <QApplication>
#include <QFontDatabase>
#include <QTimer>

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

    mpdm::gui::MainWindow w;
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
