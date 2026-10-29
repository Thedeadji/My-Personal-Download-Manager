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

namespace mpdm {
class DownloadManager;
struct DownloadItem;
}

namespace mpdm::gui {

class LedMeter;
class IndBtn;

class PopupWindow : public QWidget {
    Q_OBJECT
public:
    PopupWindow(DownloadManager* mgr, const std::string& itemId, bool stream, const Palette* pal,
                QWidget* parent = nullptr);
    const std::string& itemId() const { return id_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    void buildFile();
    void buildStream();
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
    QPoint dragPos_;
    bool openedFolder_ = false;

    QLabel* titleL_ = nullptr;
    QLabel* nameL_ = nullptr;
    QLabel* pctL_ = nullptr;
    QLabel* amtL_ = nullptr;
    QLabel* etaL_ = nullptr;
    QLabel* speedL_ = nullptr;
    IndBtn* pauseBtn_ = nullptr;
    IndBtn* stopBtn_ = nullptr;
    IndBtn* closeFooterBtn_ = nullptr;

    // file
    LedMeter* fileMeter_ = nullptr;
    QLabel* noteL_ = nullptr;
    QLabel* kvResume_ = nullptr;
    QLabel* kvSaving_ = nullptr;
    QCheckBox* openChk_ = nullptr;

    // stream
    LedMeter* streamMeter_ = nullptr;
    QLabel* step1_ = nullptr, *step2_ = nullptr, *step3_ = nullptr;
};

} // namespace mpdm::gui
