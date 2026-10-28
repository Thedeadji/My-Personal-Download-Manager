// Per-download progress popup (Industrial theme), launched by the native host
// for browser downloads: `mpdm_gui --popup <job.json>`. Matches the "Download
// popup, file" and "Download popup, stream" mockups. It is the sole downloader
// for the job (file -> engine, stream -> grabber) and writes its live state to
// ~/.mypersonaldownloadmanager/active/<id>.json so the main app can list it.
#pragma once
#include <QWidget>
#include <QTimer>
#include <QPoint>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/models.hpp"
#include "theme.hpp"

class QLabel;
class QCheckBox;

namespace mpdm::gui {

class LedMeter;
class Lanes;
class IndBtn;

class PopupWindow : public QWidget {
    Q_OBJECT
public:
    PopupWindow(const std::string& jobJson, const Palette* pal, QWidget* parent = nullptr);
    ~PopupWindow() override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void closeEvent(QCloseEvent*) override;

private:
    void buildFile();
    void buildStream();
    void startWorker();
    void joinWorker();
    void tick();            // UI refresh + active-file write from the snapshot
    void onPauseResume();
    void onStop();
    void onOpenFolder();
    void writeActive();
    void appendHistory();
    void removeActive();

    // ---- job ----
    std::string id_, url_, filename_, dest_, outputDir_;
    int connections_ = 8;
    bool stream_ = false;
    std::map<std::string, std::string> headers_;

    const Palette* pal_;
    Settings settings_;

    // ---- worker + thread-safe snapshot ----
    std::thread worker_;
    std::unique_ptr<Download> dl_;
    std::mutex mu_;
    struct Snap {
        long long downloaded = 0;
        std::optional<long long> total;
        double speed = 0.0;
        std::optional<double> eta;
        double percent = 0.0;
        std::string status = "pending";   // probing|downloading|paused|stopped|completed|failed
        std::string error;
        std::string dest;
        std::vector<SegmentProgress> conns;
        int segDone = 0, segTotal = 0;    // stream
        bool finished = false;            // worker has exited
        std::string finalPath;
    } snap_;
    std::atomic<bool> historyWritten_{false};

    QTimer timer_;
    QPoint dragPos_;

    // ---- widgets (shared) ----
    QLabel* titleL_ = nullptr;
    QLabel* pctL_ = nullptr;
    QLabel* amtL_ = nullptr;
    QLabel* etaL_ = nullptr;
    QLabel* speedL_ = nullptr;
    IndBtn* pauseBtn_ = nullptr;
    IndBtn* stopBtn_ = nullptr;
    IndBtn* closeFooterBtn_ = nullptr;

    // file-mode
    LedMeter* fileMeter_ = nullptr;
    QLabel* noteL_ = nullptr;
    QLabel* kvResume_ = nullptr;
    QCheckBox* openChk_ = nullptr;

    // stream-mode
    LedMeter* streamMeter_ = nullptr;
    QLabel* step1_ = nullptr, *step2_ = nullptr, *step3_ = nullptr;
};

} // namespace mpdm::gui
