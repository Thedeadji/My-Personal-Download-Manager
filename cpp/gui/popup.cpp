#include "popup.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "mpdm/categories.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/manager.hpp"   // state_dir()
#include "mpdm/storage.hpp"
#include "widgets.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm::gui {

// ---- small format helpers (match the main window) --------------------------
static QString human(double n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + u[i];
}
static QString htime(double s) {
    if (s <= 0) return "—";
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60;
    return h ? QString("%1h %2m").arg(h).arg(m) : (m ? QString("About %1 min left").arg(m) : QString("About %1s left").arg(t));
}
static std::string activeDir() { return (fs::path(state_dir()) / "active").string(); }
static std::string inboxDir() { return (fs::path(state_dir()) / "history_inbox").string(); }

// ---- ctor ------------------------------------------------------------------
PopupWindow::PopupWindow(const std::string& jobJson, const Palette* pal, QWidget* parent)
    : QWidget(parent), pal_(pal) {
    json job;
    try { job = json::parse(jobJson); } catch (...) { job = json::object(); }

    id_ = job.value("id", "");
    if (id_.empty()) id_ = std::to_string((long long)(QDateTime::currentMSecsSinceEpoch()));
    url_ = job.value("url", "");
    filename_ = job.value("filename", "");
    outputDir_ = job.value("output_dir", "");
    connections_ = job.value("connections", 8);
    stream_ = job.value("stream", false);
    if (!stream_) {
        std::string low = url_;
        for (auto& c : low) c = (char)::tolower(c);
        stream_ = low.find(".m3u8") != std::string::npos || low.find(".mpd") != std::string::npos;
    }
    if (job.contains("headers") && job["headers"].is_object())
        for (auto it = job["headers"].begin(); it != job["headers"].end(); ++it)
            if (it.value().is_string()) headers_[it.key()] = it.value().get<std::string>();

    settings_ = settings_from_env();
    if (connections_ > 0) settings_.connections = std::max(1, connections_);
    if (!outputDir_.empty()) settings_.output_dir = outputDir_;

    // Resolve the destination up front so the popup can show the name/path.
    if (!filename_.empty()) {
        std::string fname = sanitize_filename(filename_);
        std::string outDir = categorized_dir(settings_.output_dir, fname, settings_.categorize,
                                             settings_.category_dirs);
        std::error_code ec;
        fs::create_directories(outDir, ec);
        dest_ = unique_dest(outDir, fname);
    }
    if (stream_ && !dest_.empty()) {
        std::string low = dest_;
        for (auto& c : low) c = (char)::tolower(c);
        if (low.size() < 4 || (low.rfind(".mp4") != low.size() - 4 && low.rfind(".mkv") != low.size() - 4 &&
                               low.rfind(".webm") != low.size() - 5))
            dest_ += ".mp4";
    } else if (stream_ && dest_.empty()) {
        dest_ = (fs::path(settings_.output_dir) / "video.mp4").string();
    }

    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle("MPDM");
    setWindowIcon(Logo::icon());
    resize(440, stream_ ? 440 : 450);

    if (stream_) buildStream(); else buildFile();

    startWorker();
    connect(&timer_, &QTimer::timeout, this, &PopupWindow::tick);
    timer_.start(250);
    tick();
}

PopupWindow::~PopupWindow() { joinWorker(); }

// ---- UI construction -------------------------------------------------------
static QLabel* lab(const QString& t, QFont f, QColor c) {
    auto* l = new QLabel(t);
    l->setFont(f);
    l->setStyleSheet("color:" + c.name(QColor::HexArgb) + ";background:transparent");
    return l;
}

void PopupWindow::buildFile() {
    const Palette& P = *pal_;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);

    // title bar
    auto* title = new Surface(Surface::TitleBar); title->setPalette(pal_);
    title->setFixedHeight(48);
    auto* tl = new QHBoxLayout(title); tl->setContentsMargins(14, 0, 10, 0); tl->setSpacing(10);
    tl->addWidget(new Logo(22));
    titleL_ = lab("Downloading", chakra(16, 600, 0.0), P.text);
    tl->addWidget(titleL_);
    tl->addStretch(1);
    auto* x = lab("✕", barlow(16), P.text3); x->setCursor(Qt::PointingHandCursor);
    x->setObjectName("xclose");
    tl->addWidget(x);
    root->addWidget(title);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body); v->setContentsMargins(16, 14, 16, 14); v->setSpacing(12);

    // file row
    auto* fr = new QHBoxLayout; fr->setSpacing(12);
    auto* icon = new QLabel; icon->setFixedSize(52, 52);
    icon->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " + P.bevelDark.name());
    icon->setAlignment(Qt::AlignCenter); icon->setText("▣");
    icon->setFont(mono(22)); icon->setStyleSheet(icon->styleSheet() + ";color:" + P.amber.name());
    fr->addWidget(icon);
    auto* nmBox = new QVBoxLayout; nmBox->setSpacing(3);
    QString nm = QString::fromStdString(dest_.empty() ? filename_ : fs::path(dest_).filename().string());
    if (nm.isEmpty()) nm = QString::fromStdString(url_);
    auto* nameL = lab(nm, barlow(16, 500), P.text); nameL->setObjectName("name");
    nmBox->addWidget(nameL);
    QString host = QString::fromStdString(url_);
    { QUrl u(QString::fromStdString(url_)); if (!u.host().isEmpty()) host = u.host(); }
    nmBox->addWidget(lab(host, mono(12), P.text3));
    fr->addLayout(nmBox, 1);
    v->addLayout(fr);

    // big % + speed
    auto* pr = new QHBoxLayout; pr->setSpacing(14);
    pctL_ = lab("0%", mono(40, 500), P.text);
    pr->addWidget(pctL_, 0, Qt::AlignBottom);
    auto* midBox = new QVBoxLayout; midBox->setSpacing(2);
    amtL_ = lab("—", barlow(13, 500), P.text2);
    etaL_ = lab("—", barlow(12), P.text3);
    midBox->addStretch(1); midBox->addWidget(amtL_); midBox->addWidget(etaL_);
    pr->addLayout(midBox); pr->addStretch(1);
    auto* spBox = new QVBoxLayout; spBox->setSpacing(0);
    speedL_ = lab("0 B/s", mono(26, 500), P.cyan); speedL_->setAlignment(Qt::AlignRight);
    spBox->addStretch(1); spBox->addWidget(speedL_);
    spBox->addWidget(lab("SPEED", chakra(11, 600, 0.10), P.text3), 0, Qt::AlignRight);
    pr->addLayout(spBox);
    v->addLayout(pr);

    // single LED progress strip (overall), matching the file mockup
    fileMeter_ = new LedMeter(34, 0, 14, 3); fileMeter_->setPalette(pal_);
    v->addWidget(fileMeter_);

    // note
    noteL_ = lab("", barlow(12), P.text2);
    noteL_->setWordWrap(true);
    noteL_->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent;border-left:2px solid " +
                          P.amber.name() + ";padding-left:8px");
    noteL_->hide();
    v->addWidget(noteL_);

    // kv well
    auto* well = new Surface(Surface::Well); well->setPalette(pal_);
    auto* kv = new QVBoxLayout(well); kv->setContentsMargins(12, 11, 12, 11); kv->setSpacing(8);
    auto kvRow = [&](const char* k, const QString& val, QColor vc) -> QLabel* {
        auto* r = new QHBoxLayout;
        r->addWidget(lab(QString(k).toUpper(), chakra(11, 600, 0.08), P.text3));
        r->addStretch(1);
        auto* vv = lab(val, mono(12), vc);
        r->addWidget(vv); kv->addLayout(r); return vv;
    };
    kvRow("Saving to", QString::fromStdString(dest_.empty() ? settings_.output_dir : fs::path(dest_).parent_path().string()), P.text);
    kvResume_ = kvRow("Resume", "—", P.green);
    kvRow("Speed limit", settings_.max_rate_bps ? human((double)settings_.max_rate_bps) + "/s FOR THIS FILE" : "NONE", P.text);
    v->addWidget(well);
    v->addStretch(1);
    root->addWidget(body, 1);

    // footer
    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(pal_);
    footer->setFixedHeight(64);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(14, 0, 10, 0); fl->setSpacing(10);
    openChk_ = new QCheckBox("Open folder when done");
    openChk_->setStyleSheet(QString("QCheckBox{color:%1;background:transparent;spacing:8px}"
                                    "QCheckBox::indicator{width:18px;height:18px;background:%2;border:1px solid %3}"
                                    "QCheckBox::indicator:checked{background:%4;border:1px solid %4}")
                                .arg(P.text2.name(), P.well.name(), P.bevelLight.name(), P.amber.name()));
    fl->addWidget(openChk_);
    fl->addStretch(1);
    stopBtn_ = new IndBtn("Stop"); stopBtn_->setPalette(pal_);
    pauseBtn_ = new IndBtn("Pause", true); pauseBtn_->setPalette(pal_);
    closeFooterBtn_ = new IndBtn("Close"); closeFooterBtn_->setPalette(pal_); closeFooterBtn_->hide();
    fl->addWidget(stopBtn_); fl->addWidget(pauseBtn_); fl->addWidget(closeFooterBtn_);
    root->addWidget(footer);

    connect(pauseBtn_, &QAbstractButton::clicked, this, &PopupWindow::onPauseResume);
    connect(stopBtn_, &QAbstractButton::clicked, this, &PopupWindow::onStop);
    connect(closeFooterBtn_, &QAbstractButton::clicked, this, [this] { close(); });
}

void PopupWindow::buildStream() {
    const Palette& P = *pal_;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1); root->setSpacing(0);

    auto* title = new Surface(Surface::TitleBar); title->setPalette(pal_);
    title->setFixedHeight(48);
    auto* tl = new QHBoxLayout(title); tl->setContentsMargins(14, 0, 10, 0); tl->setSpacing(10);
    tl->addWidget(new Logo(22));
    titleL_ = lab("Grabbing stream", chakra(16, 600, 0.0), P.text);
    tl->addWidget(titleL_); tl->addStretch(1);
    auto* x = lab("✕", barlow(16), P.text3); x->setObjectName("xclose"); x->setCursor(Qt::PointingHandCursor);
    tl->addWidget(x);
    root->addWidget(title);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body); v->setContentsMargins(16, 14, 16, 14); v->setSpacing(12);

    auto* fr = new QHBoxLayout; fr->setSpacing(12);
    auto* icon = new QLabel; icon->setFixedSize(52, 52);
    icon->setAlignment(Qt::AlignCenter); icon->setText("▤"); icon->setFont(mono(22));
    icon->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " + P.bevelDark.name() +
                        ";color:" + P.amber.name());
    fr->addWidget(icon);
    auto* nmBox = new QVBoxLayout; nmBox->setSpacing(3);
    QString nm = QString::fromStdString(dest_.empty() ? filename_ : fs::path(dest_).filename().string());
    if (nm.isEmpty()) nm = "video.mp4";
    nmBox->addWidget(lab(nm, barlow(16, 500), P.text));
    QString host = QString::fromStdString(url_);
    { QUrl u(QString::fromStdString(url_)); if (!u.host().isEmpty()) host = u.host(); }
    nmBox->addWidget(lab("HLS/DASH stream · " + host, mono(12), P.text3));
    fr->addLayout(nmBox, 1);
    v->addLayout(fr);

    auto* pr = new QHBoxLayout; pr->setSpacing(14);
    pctL_ = lab("0%", mono(40, 500), P.text); pr->addWidget(pctL_, 0, Qt::AlignBottom);
    auto* midBox = new QVBoxLayout; midBox->setSpacing(2);
    amtL_ = lab("—", barlow(13, 500), P.text2);
    etaL_ = lab("—", barlow(12), P.text3);
    midBox->addStretch(1); midBox->addWidget(amtL_); midBox->addWidget(etaL_);
    pr->addLayout(midBox); pr->addStretch(1);
    auto* spBox = new QVBoxLayout; spBox->setSpacing(0);
    speedL_ = lab("0 B/s", mono(26, 500), P.cyan); speedL_->setAlignment(Qt::AlignRight);
    spBox->addStretch(1); spBox->addWidget(speedL_);
    spBox->addWidget(lab("SPEED", chakra(11, 600, 0.10), P.text3), 0, Qt::AlignRight);
    pr->addLayout(spBox);
    v->addLayout(pr);

    streamMeter_ = new LedMeter(28, 0, 10, 2); streamMeter_->setPalette(pal_);
    v->addWidget(streamMeter_);

    auto stepRow = [&](const QString& t, const QString& sub) -> QLabel* {
        auto* h = new QHBoxLayout; h->setSpacing(10);
        auto* mark = new QLabel; mark->setFixedSize(30, 30); mark->setAlignment(Qt::AlignCenter);
        mark->setFont(mono(14));
        mark->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " + P.bevelDark.name() +
                            ";color:" + P.text3.name());
        h->addWidget(mark);
        auto* tb = new QVBoxLayout; tb->setSpacing(1);
        tb->addWidget(lab(t, barlow(14, 500), P.text));
        tb->addWidget(lab(sub.toUpper(), mono(11), P.text3));
        h->addLayout(tb, 1);
        v->addLayout(h);
        return mark;  // caller keeps the mark label to update its state
    };
    step1_ = stepRow("Read the playlist", "pick the best quality");
    step2_ = stepRow("Download and decrypt parts", "aes-128 when encrypted");
    step3_ = stepRow("Join parts into one MP4", "ffmpeg, no re-encoding");
    v->addStretch(1);
    root->addWidget(body, 1);

    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(pal_);
    footer->setFixedHeight(64);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(14, 0, 10, 0); fl->setSpacing(10);
    fl->addWidget(lab("Stream grabs can't be paused", barlow(12), P.text3));
    fl->addStretch(1);
    pauseBtn_ = new IndBtn("Pause"); pauseBtn_->setPalette(pal_); pauseBtn_->setEnabled(false);
    stopBtn_ = new IndBtn("Stop"); stopBtn_->setPalette(pal_);
    closeFooterBtn_ = new IndBtn("Close"); closeFooterBtn_->setPalette(pal_); closeFooterBtn_->hide();
    fl->addWidget(pauseBtn_); fl->addWidget(stopBtn_); fl->addWidget(closeFooterBtn_);
    root->addWidget(footer);

    connect(stopBtn_, &QAbstractButton::clicked, this, &PopupWindow::onStop);
    connect(closeFooterBtn_, &QAbstractButton::clicked, this, [this] { close(); });
}

// ---- worker ----------------------------------------------------------------
void PopupWindow::startWorker() {
    joinWorker();
    { std::lock_guard<std::mutex> lk(mu_); snap_.finished = false; snap_.error.clear(); }
    if (stream_) {
        snap_.status = "downloading";
        worker_ = std::thread([this] {
            GrabProgressCb cb = [this](int done, int total) {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.segDone = done; snap_.segTotal = total;
                snap_.percent = total ? done * 100.0 / total : 0;
                snap_.status = "downloading";
            };
            try {
                std::string p = mpdm::grab(url_, dest_, settings_, "auto", "best", cb);
                std::lock_guard<std::mutex> lk(mu_);
                snap_.finalPath = p; snap_.status = "completed"; snap_.percent = 100; snap_.finished = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(mu_);
                snap_.status = "failed"; snap_.error = e.what(); snap_.finished = true;
            }
        });
        worker_.detach();   // grab has no cancel; the process exit aborts it
        return;
    }
    // file download
    dl_ = std::make_unique<Download>(
        url_, settings_, dest_,
        [this](const Progress& p) {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.downloaded = p.downloaded;
            snap_.total = p.total;
            snap_.speed = p.speed_bps;
            snap_.eta = p.eta_seconds;
            snap_.percent = p.percent() ? *p.percent() : 0;
            snap_.conns = p.connections;
            if (!p.filename.empty()) snap_.dest = p.filename;
            snap_.status = p.status == Status::Paused ? "paused"
                           : p.status == Status::Completed ? "completed"
                           : p.status == Status::Failed ? "failed"
                           : p.status == Status::Probing ? "probing"
                           : "downloading";
        },
        headers_);
    worker_ = std::thread([this] {
        try {
            std::string p = dl_->start();
            std::lock_guard<std::mutex> lk(mu_);
            snap_.finalPath = p; snap_.status = "completed"; snap_.percent = 100; snap_.finished = true;
        } catch (const DownloadError& e) {
            std::string m = e.what();
            std::lock_guard<std::mutex> lk(mu_);
            if (m.find("paused") != std::string::npos) snap_.status = "paused";
            else if (m.find("cancelled") != std::string::npos) snap_.status = "stopped";
            else { snap_.status = "failed"; snap_.error = m; }
            snap_.finished = true;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mu_);
            snap_.status = "failed"; snap_.error = e.what(); snap_.finished = true;
        }
    });
}

void PopupWindow::joinWorker() {
    if (worker_.joinable()) {
        if (dl_) dl_->cancel();
        worker_.join();
    }
    // keep the resolved dest for a later resume
    if (dl_ && !dl_->dest_path().empty()) dest_ = dl_->dest_path();
}

void PopupWindow::onPauseResume() {
    std::string st;
    { std::lock_guard<std::mutex> lk(mu_); st = snap_.status; }
    if (st == "paused" || st == "stopped") {
        startWorker();                       // resume (new Download continues from sidecar)
    } else {
        if (dl_) dl_->pause();
        if (worker_.joinable()) worker_.join();
        if (dl_ && !dl_->dest_path().empty()) dest_ = dl_->dest_path();
        std::lock_guard<std::mutex> lk(mu_); snap_.status = "paused"; snap_.speed = 0;
    }
}

void PopupWindow::onStop() {
    if (stream_) { close(); return; }        // no cancel for grab -> just close (process exits)
    if (dl_) dl_->cancel();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(mu_); snap_.status = "stopped"; snap_.speed = 0;
}

void PopupWindow::onOpenFolder() {
    std::string d = snap_.finalPath.empty() ? dest_ : snap_.finalPath;
    if (d.empty()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(fs::path(d).parent_path().string())));
}

// ---- tick: refresh widgets + write the active state file --------------------
void PopupWindow::tick() {
    Snap s;
    { std::lock_guard<std::mutex> lk(mu_); s = snap_; }
    const Palette& P = *pal_;

    int pct = (int)(s.percent + 0.5);
    bool done = (s.status == "completed");
    bool ended = done || s.status == "failed" || s.status == "stopped";

    if (stream_) {
        titleL_->setText(done ? "Stream saved" : QString("Grabbing stream %1%").arg(pct));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(s.segTotal ? QString("Part %1 of %2").arg(s.segDone).arg(s.segTotal) : QString("Reading…"));
        etaL_->setText(done ? "Done" : (s.status == "failed" ? "Failed" : ""));
        speedL_->setText("—");
        if (streamMeter_) streamMeter_->set(s.percent / 100.0, done ? P.green : P.cyan);
        auto setStep = [&](QLabel* m, bool active, bool complete) {
            if (!m) return;
            m->setText(complete ? "✓" : (active ? "↓" : "⏳"));
            QColor c = complete ? P.green : (active ? P.cyan : P.text3);
            m->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " +
                             (active ? P.cyan.name() : P.bevelDark.name()) + ";color:" + c.name());
        };
        setStep(step1_, s.segTotal == 0 && !ended, s.segTotal > 0 || done);
        setStep(step2_, s.segTotal > 0 && !done, done);
        setStep(step3_, false, done);
    } else {
        titleL_->setText(done ? "Completed" : (s.status == "paused" ? "Paused" :
                         s.status == "stopped" ? "Stopped" : s.status == "failed" ? "Failed" :
                         QString("Downloading %1%").arg(pct)));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(human((double)s.downloaded) + " of " + (s.total ? human((double)*s.total) : QString("?")));
        etaL_->setText(done ? "Done" : (s.status == "downloading" && s.eta ? htime(*s.eta) : ""));
        speedL_->setText(s.status == "downloading" ? human(s.speed) + "/s" : "—");
        if (fileMeter_) fileMeter_->set(s.percent / 100.0, done ? P.green : P.cyan);
        if (noteL_) {
            const SegmentProgress* stolen = nullptr;
            for (auto& c : s.conns) if (c.stole_from >= 0) { stolen = &c; break; }
            if (stolen) { noteL_->setText(QString("%1 connections. C%2 finished early and took over part of C%3's remaining range.")
                                              .arg((int)s.conns.size()).arg(stolen->index + 1).arg(stolen->stole_from + 1)); noteL_->show(); }
            else noteL_->hide();
        }
        if (kvResume_) kvResume_->setText(done ? "COMPLETE" : (s.status == "downloading" ? "SUPPORTED, SAVING" : "SAVED"));
    }

    // button states
    if (ended) {
        if (pauseBtn_) pauseBtn_->hide();
        if (stopBtn_) stopBtn_->hide();
        if (closeFooterBtn_) closeFooterBtn_->show();
    } else if (!stream_ && pauseBtn_) {
        pauseBtn_->setText(s.status == "paused" ? "Resume" : "Pause");
    }

    // persist live state for the main app
    writeActive();

    // on completion, hand a record to the app's history inbox (once), open folder
    if (done && !historyWritten_.exchange(true)) {
        appendHistory();
        if (openChk_ && openChk_->isChecked()) onOpenFolder();
    }
}

// ---- disk integration ------------------------------------------------------
void PopupWindow::writeActive() {
    Snap s;
    { std::lock_guard<std::mutex> lk(mu_); s = snap_; }
    try {
        std::error_code ec; fs::create_directories(activeDir(), ec);
        json j;
        j["id"] = id_;
        j["url"] = url_;
        j["filename"] = dest_.empty() ? filename_ : fs::path(dest_).filename().string();
        j["dest_path"] = dest_;
        j["downloaded"] = s.downloaded;
        j["total"] = s.total ? json(*s.total) : json(nullptr);
        j["speed"] = s.speed;
        j["percent"] = s.percent;
        j["status"] = s.status;
        j["stream"] = stream_;
        j["seg_done"] = s.segDone; j["seg_total"] = s.segTotal;
        j["error"] = s.error;
        json conns = json::array();
        for (auto& c : s.conns)
            conns.push_back({{"index", c.index}, {"start", c.start}, {"end", c.end},
                             {"downloaded", c.downloaded}, {"speed_bps", c.speed_bps},
                             {"active", c.active}, {"stole_from", c.stole_from}});
        j["connections"] = conns;
        std::string p = (fs::path(activeDir()) / (id_ + ".json")).string();
        std::string tmp = p + ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << j.dump(); }
        fs::rename(tmp, p, ec);
    } catch (...) {}
}

void PopupWindow::appendHistory() {
    try {
        std::error_code ec; fs::create_directories(inboxDir(), ec);
        std::string path = snap_.finalPath.empty() ? dest_ : snap_.finalPath;
        long long size = 0;
        { std::error_code e2; if (fs::exists(path, e2)) size = (long long)fs::file_size(path, e2); }
        json j;  // DownloadItem shape the manager's loader understands
        j["id"] = id_;
        j["url"] = url_;
        j["filename"] = path.empty() ? filename_ : fs::path(path).filename().string();
        j["dest_path"] = path;
        j["size"] = size ? json(size) : json(nullptr);
        j["downloaded"] = size;
        j["status"] = "completed";
        j["connections"] = connections_;
        j["max_rate_bps"] = (long long)settings_.max_rate_bps;
        j["incognito"] = false;
        j["headers"] = json::object();
        j["error"] = "";
        j["added_at"] = (double)QDateTime::currentSecsSinceEpoch();
        std::string p = (fs::path(inboxDir()) / (id_ + ".json")).string();
        std::string tmp = p + ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << j.dump(2); }
        fs::rename(tmp, p, ec);
    } catch (...) {}
}

void PopupWindow::removeActive() {
    std::error_code ec;
    fs::remove(fs::path(activeDir()) / (id_ + ".json"), ec);
}

// ---- window chrome ---------------------------------------------------------
void PopupWindow::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), pal_->frame);
    g.setPen(QPen(pal_->bevelLight, 1));
    g.drawRect(rect().adjusted(0, 0, -1, -1));
}

void PopupWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && e->position().y() < 48) {
        dragPos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
        // click on the ✕ in the title closes
        if (auto* x = findChild<QLabel*>("xclose")) {
            QPoint lp = x->mapFromGlobal(e->globalPosition().toPoint());
            if (x->rect().contains(lp)) { close(); return; }
        }
        e->accept();
    }
}

void PopupWindow::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton && !dragPos_.isNull())
        move(e->globalPosition().toPoint() - dragPos_);
}

void PopupWindow::closeEvent(QCloseEvent* e) {
    joinWorker();
    removeActive();
    e->accept();
    qApp->quit();
}

} // namespace mpdm::gui
