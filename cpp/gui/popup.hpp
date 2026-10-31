// Per-download progress popup (Industrial theme) — a live *view* of a queue item
// the main app owns (IDM-style). Matches the "Download popup, file/stream"
// mockups. Closing it leaves the download running in the app; Stop/Pause act on
// the manager. One popup per download; the main window owns them.
#pragma once
#include <QPoint>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <string>

#include "theme.hpp"

class QLabel;
class QCheckBox;
class QPushButton;
class QVBoxLayout;

namespace mpdm {
class DownloadManager;
struct DownloadItem;
}

namespace mpdm::gui {

class LedMeter;
class IndBtn;

// Ask for a fresh link for a stopped download (expired / changed URL), point the
// item at it and queue it again. Returns true if the link was replaced.
bool askNewLink(QWidget* parent, DownloadManager* mgr, const std::string& id);
// Ask for the checksum a site publishes and attach it to the download (checked
// when it finishes, or right away if it already has). Returns true if set.
bool askChecksum(QWidget* parent, DownloadManager* mgr, const std::string& id);
// "retrying in 4 min" for a failed download that will be retried automatically,
// "" otherwise.
QString autoRetryText(DownloadManager* mgr, const DownloadItem& it);

class PopupWindow : public QWidget {
    Q_OBJECT
public:
    PopupWindow(DownloadManager* mgr, const std::string& itemId, bool stream, const Palette* pal,
                QWidget* parent = nullptr);
    const std::string& itemId() const { return id_; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void buildFile();
    void buildStream();
    // Build the frameless chrome (outer resize ring + draggable title bar with the
    // minimize-to-background and close buttons) and return the content layout the
    // caller fills with the body and footer. Shared by file + stream popups.
    QVBoxLayout* buildChrome(const QString& titleText);
    QWidget* wrapScroll(QWidget* body);  // let the body scroll when the window is shrunk
    void tick();
    void onPauseResume();
    void onStop();
    void onOpenFolder();
    std::shared_ptr<DownloadItem> item() const;

    DownloadManager* mgr_;
    std::string id_;
    bool stream_;
    const Palette* pal_;
    QTimer timer_;
    bool openedFolder_ = false;

    QLabel* titleL_ = nullptr;
    QLabel* iconL_ = nullptr;      // file-type icon (set once the name is known)
    QString iconName_;
    QLabel* nameL_ = nullptr;
    QLabel* pctL_ = nullptr;
    QLabel* amtL_ = nullptr;
    QLabel* etaL_ = nullptr;
    QLabel* speedL_ = nullptr;
    IndBtn* pauseBtn_ = nullptr;
    IndBtn* stopBtn_ = nullptr;
    IndBtn* closeFooterBtn_ = nullptr;
    IndBtn* linkBtn_ = nullptr;    // "New link…", shown only when the download failed

    // file
    LedMeter* fileMeter_ = nullptr;
    QLabel* noteL_ = nullptr;
    QLabel* kvResume_ = nullptr;
    QLabel* kvSaving_ = nullptr;
    QLabel* kvLimit_ = nullptr;
    QCheckBox* openChk_ = nullptr;

    // stream
    LedMeter* streamMeter_ = nullptr;
    QLabel* step1_ = nullptr, *step2_ = nullptr, *step3_ = nullptr;
};

} // namespace mpdm::gui
