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

    // Dev: `--shot <path>` renders the window to a PNG after it settles, then quits.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            QString out = argv[i + 1];
            QTimer::singleShot(900, [&w, out] {
                w.grab().save(out);
                qApp->quit();
            });
        }
    }
    return app.exec();
}
