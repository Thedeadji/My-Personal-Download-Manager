// MPDM desktop GUI — Industrial main window (Qt Widgets), per DESIGN.md.
#pragma once
#include <QGridLayout>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "mpdm/manager.hpp"
#include "theme.hpp"
#include "widgets.hpp"

class QSystemTrayIcon;
class QCloseEvent;
class QComboBox;
class QMenu;
class QNetworkAccessManager;
class QLabel;
class QPushButton;

namespace mpdm::gui {

class PopupWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openSettings();
    void openScheduler();
    void openHelp(int page = 0);  // the (?) button: About / Connect a browser / Settings explained
    static QString appVersion();  // this build's version (MPDM_VERSION)
    void openPopupFor(const std::string& id, bool stream);  // live popup for a queue item
    void allowQuit() { allowQuit_ = true; }                 // closing really quits (no hide-to-tray)
    void devShotPopup(const QString& out, bool stream);      // dev: render a popup to PNG
    void devShotGraph(const QString& out);                   // dev: render the graph with data
    void devShotFinish(const QString& out);                  // dev: render the finish dialog to PNG
    void devShotTrayMenu(const QString& out);                // dev: render the tray right-click menu
    void devShotQuickPanel(const QString& out);              // dev: render the left-click quick panel
    void devShotTrayIcons(const QString& out);               // dev: render the tray icon states strip
    void devShotDeselect(const QString& out);                // dev: select an item, remove it, render
    void devShotIcons(const QString& out);                   // dev: queue of mixed file types
    void devTestUpdate(const QString& out);                  // dev: exercise the updater end to end
    void devShotRetry(const QString& out);                   // dev: failed download (popup + Selected panel)
    void devShotConflict(const QString& out);                // dev: "file on the server changed" dialog

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;  // close -> minimize to tray
    void changeEvent(QEvent* ev) override;       // keep the maximize glyph in sync
    // Windows: let the installer's Restart Manager (and OS shutdown) really close
    // the app instead of it hiding to tray, so updates don't hang.
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void applyTheme();
    void refresh();
    void onAdd();
    void onRemove();                                  // Remove with optional "delete file?" confirm
    void onClearDone();                               // drop completed items from the list
    std::shared_ptr<DownloadItem> findItem(const std::string& id);
    void showRowMenu(const std::string& id, const QPoint& at);  // right-click on a queue row
    void toggleMaximize();                            // maximize/restore (frameless)
    void showFinishDialog(const std::shared_ptr<DownloadItem>& it);  // IDM-style "download complete"
    QString askDuplicate(const QString& path);  // "file exists" prompt -> rename/skip/overwrite
    QString askResumeConflict(const ResumeConflict& c);  // server file changed -> continue/restart/keep
    void applyAppearance(const QString& mode);  // "system" | "light" | "dark"
    void setupTray();
    QMenu* buildTrayMenu();     // Industrial-styled right-click menu (rebuilt on theme change)
    void showQuickPanel();      // left-click quick panel (active downloads + quick actions)
    void showFromTray();
    void showDownloadPopups();  // open/raise the live popup(s) for current downloads
    void quitApp();  // real exit (from tray menu)

    // ---- updates (updater.cpp) ----
    void checkForUpdates(bool manual, std::function<void(const QString&)> report = {});
    void maybeAutoCheckUpdates(bool startup);
    QWidget* buildUpdateBar();
    void showUpdateBar();
    void startUpdate();
    void launchInstaller();
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
    int settingsStartPage_ = -1;   // Settings section to open on next openSettings()
    QString catFilter_ = "All";    // category dropdown filter for the queue list
    long long lastDiskBytes_ = 0;  // for disk-write-rate on the graph
    double lastDiskT_ = 0;
    QTimer timer_;
    QTimer jobTimer_;
    std::vector<PopupWindow*> popups_;

    QPushButton* maxBtn_ = nullptr;  // title-bar maximize/restore button (glyph toggles)

    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    struct UpdateInfo { QString version, page, notes, installer, installerName, sums; } upd_;
    QNetworkAccessManager* net_ = nullptr;
    QWidget* updateBar_ = nullptr;
    QLabel* updateText_ = nullptr;
    IndBtn* updateNowBtn_ = nullptr;
    QString updateFile_;            // verified installer waiting for the queue to go idle
    QString updateProgress_;
    bool updateBusy_ = false;
    bool updateDismissed_ = false;  // "Later": hidden for this session
    QTimer updateTimer_;      // owned context menu, re-themed on appearance change
    QWidget* quickPanel_ = nullptr;  // left-click quick panel (recreated each open)
    bool allowQuit_ = false;        // set true only when quitting for real
    bool trayHintShown_ = false;    // show the "still running" balloon once
    int lastFinished_ = 0;          // for finish notifications
    std::set<std::string> finishAnnounced_;  // ids we've already shown a finish dialog for

    // tray-icon state (reflects download activity on the shield)
    int trayState_ = -1;
    double trayFrac_ = -2;
    double trayIconAt_ = 0;         // last tray-icon update (throttle to ~1/s)
    bool trayDoneSeen_ = true;      // "all done" green badge clears when MPDM is opened
};

} // namespace mpdm::gui
