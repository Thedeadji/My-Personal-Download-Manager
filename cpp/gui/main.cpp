// MPDM desktop GUI entry point.
#include <QApplication>

#include "mainwindow.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("MyPersonalDownloadManager");
    mpdm::gui::MainWindow w;
    w.show();
    return app.exec();
}
