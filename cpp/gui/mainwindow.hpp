// MPDM desktop GUI — Industrial main window (Qt Widgets), per DESIGN.md.
#pragma once
#include <QGridLayout>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <vector>

#include "mpdm/manager.hpp"
#include "theme.hpp"
#include "widgets.hpp"

class QSystemTrayIcon;
class QCloseEvent;

namespace mpdm::gui {

class PopupWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openSettings();
    void openScheduler();
    void openPopupFor(const std::string& id, bool stream);  // live popup for a queue item
    void devShotPopup(const QString& out, bool stream);      // dev: render a popup to PNG

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;  // close -> minimize to tray

private:
    void buildUi();
    void applyTheme();
    void refresh();
    void onAdd();
    void applyAppearance(const QString& mode);  // "system" | "light" | "dark"
    void setupTray();
    void showFromTray();
    void quitApp();  // real exit (from tray menu)
    void pollJobs();  // adopt browser jobs the native host dropped, open popups

    // register a widget that has setPalette(const Palette*)
    template <class W>
    W* themed(W* w) {
        themed_.push_back([w](const Palette* p) { w->setPalette(p); });
        return w;
    }

    mpdm::DownloadManager manager_;
    const Palette* pal_ = &kDark;
    std::vector<std::function<void(const Palette*)>> themed_;

    Graph* graph_ = nullptr;
    LedMeter* speedMeter_ = nullptr;
    LedMeter* selMeter_ = nullptr;
    Lanes* lanes_ = nullptr;
    QWidget* slotLamps_ = nullptr;
    QGridLayout* catsLay_ = nullptr;
    QVBoxLayout* statusLay_ = nullptr;
    QVBoxLayout* rowsLay_ = nullptr;

    QString selId_;
    QTimer timer_;
    QTimer jobTimer_;
    std::vector<PopupWindow*> popups_;

    QSystemTrayIcon* tray_ = nullptr;
    bool allowQuit_ = false;        // set true only when quitting for real
    bool trayHintShown_ = false;    // show the "still running" balloon once
    int lastFinished_ = 0;          // for finish notifications
};

} // namespace mpdm::gui
