#include "popup.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>
#include <filesystem>

#include "mpdm/manager.hpp"
#include "fileicon.hpp"
#include "widgets.hpp"

namespace fs = std::filesystem;

namespace mpdm::gui {

static QString human(double n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + u[i];
}
static QString htime(double s) {
    if (s <= 0) return "";
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60;
    return h ? QString("About %1h %2m left").arg(h).arg(m)
             : (m ? QString("About %1 min left").arg(m) : QString("About %1s left").arg(t));
}
static QLabel* lab(const QString& t, QFont f, QColor c) {
    auto* l = new QLabel(t);
    l->setFont(f);
    l->setStyleSheet("color:" + c.name(QColor::HexArgb) + ";background:transparent");
    return l;
}
// A flat title-bar control button (matches the main window's chrome buttons).
static QPushButton* winBtn(const QString& glyph, const Palette& P, bool danger, int fontPx) {
    auto* b = new QPushButton(glyph);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedSize(40, 30);
    b->setFocusPolicy(Qt::NoFocus);
    QString hov = danger ? "QPushButton:hover{background:#D0452F;color:#ffffff}"
                         : "QPushButton:hover{background:" + P.panelHi.name(QColor::HexArgb) +
                               ";color:" + P.text.name(QColor::HexArgb) + "}";
    b->setStyleSheet("QPushButton{background:transparent;border:0;color:" +
                     P.text3.name(QColor::HexArgb) + ";font-size:" + QString::number(fontPx) + "px}" + hov);
    return b;
}

PopupWindow::PopupWindow(DownloadManager* mgr, const std::string& itemId, bool stream,
                         const Palette* pal, QWidget* parent)
    : QWidget(parent), mgr_(mgr), id_(itemId), stream_(stream), pal_(pal) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle("MPDM");
    setWindowIcon(Logo::icon());
    resize(440, stream_ ? 440 : 450);
    // Resizable (frameless): drag any edge/corner; shrinks down to this minimum,
    // below which the body scrolls so nothing is clipped on a small screen.
    setMinimumSize(300, 240);
    setMouseTracking(true);
    if (stream_) buildStream(); else buildFile();
    connect(&timer_, &QTimer::timeout, this, &PopupWindow::tick);
    timer_.start(300);
    tick();
}

std::shared_ptr<DownloadItem> PopupWindow::item() const {
    for (auto& it : mgr_->ordered())
        if (it->id == id_) return it;
    return nullptr;
}

// Put the body in a transparent, frameless scroll area so the window can be
// resized smaller than its content without clipping (the body scrolls instead).
QWidget* PopupWindow::wrapScroll(QWidget* body) {
    auto* scroll = new QScrollArea;
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->viewport()->setAutoFillBackground(false);
    body->setAutoFillBackground(false);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:0}");
    return scroll;
}

// ---- UI --------------------------------------------------------------------
// Frameless chrome shared by the file + stream popups: an outer frame (its 6px
// ring is the resize grip) wrapping the content; a draggable title bar carrying
// the minimize-to-background (down arrowhead) and close buttons. Returns the
// content layout to fill with the body + footer.
QVBoxLayout* PopupWindow::buildChrome(const QString& titleText) {
    const Palette& P = *pal_;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);

    auto* outerFrame = new QWidget;
    outerFrame->setObjectName("popupFrame");
    outerFrame->setStyleSheet("QWidget#popupFrame{background:" + P.frame.name(QColor::HexArgb) +
                              ";border:1px solid " + P.bevelLight.name() + "}");
    auto* ofl = new QVBoxLayout(outerFrame);
    ofl->setContentsMargins(6, 6, 6, 6); ofl->setSpacing(0);
    root->addWidget(outerFrame);

    // Content holder: an explicit arrow cursor here stops the outer frame's resize
    // cursor from being inherited by every interior widget (Qt inherits a parent's
    // cursor), so the resize cursor only shows on the 6px ring.
    auto* inner = new QWidget;
    inner->setCursor(Qt::ArrowCursor);
    auto* content = new QVBoxLayout(inner);
    content->setContentsMargins(0, 0, 0, 0); content->setSpacing(0);
    ofl->addWidget(inner);

    auto* title = new Surface(Surface::TitleBar); title->setPalette(pal_);
    title->setFixedHeight(48);
    auto* tl = new QHBoxLayout(title); tl->setContentsMargins(14, 0, 8, 0); tl->setSpacing(10);
    auto* logo = new Logo(22); logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    tl->addWidget(logo);
    titleL_ = lab(titleText, chakra(16, 600, 0.0), P.text);
    titleL_->setAttribute(Qt::WA_TransparentForMouseEvents);
    tl->addWidget(titleL_); tl->addStretch(1);
    // A thin divider so the three window controls read as separate buttons.
    auto sep = [&] {
        auto* s = new QWidget; s->setFixedSize(1, 18);
        s->setStyleSheet("background:" + P.bevelLight.name());
        return s;
    };
    auto* minN = winBtn(QString::fromUtf8("\xE2\x80\x95"), P, false, 14);  // ― normal minimize
    minN->setToolTip("Minimize");
    auto* minTray = winBtn(QString::fromUtf8("\xE2\x8C\x84"), P, false, 18);  // ⌄ minimize to background
    minTray->setToolTip("Minimize to background");
    auto* x = winBtn(QString::fromUtf8("\xE2\x9C\x95"), P, true, 14);    // ✕
    x->setToolTip("Close (download keeps running)");
    tl->setSpacing(6);
    tl->addWidget(minN); tl->addWidget(sep());
    tl->addWidget(minTray); tl->addWidget(sep());
    tl->addWidget(x);
    content->addWidget(title);

    connect(minN, &QPushButton::clicked, this, [this] { showMinimized(); });
    // Minimize-to-background: hide the view; the download keeps running in the app
    // (reopen from the queue row or the tray's "Open download popup").
    connect(minTray, &QPushButton::clicked, this, [this] { hide(); });
    connect(x, &QPushButton::clicked, this, [this] { close(); });

    // Drag the title bar to move; drag the outer frame's edge ring to resize.
    new FramelessChrome(this, title, outerFrame);
    return content;
}

void PopupWindow::buildFile() {
    const Palette& P = *pal_;
    auto* root = buildChrome("Downloading");

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body); v->setContentsMargins(16, 14, 16, 14); v->setSpacing(12);

    auto* fr = new QHBoxLayout; fr->setSpacing(12);
    auto* icon = new QLabel; icon->setFixedSize(52, 52); icon->setAlignment(Qt::AlignCenter);
    icon->setText("▣"); icon->setFont(mono(22)); iconL_ = icon;
    icon->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " + P.bevelDark.name() +
                        ";color:" + P.amber.name());
    fr->addWidget(icon);
    auto* nmBox = new QVBoxLayout; nmBox->setSpacing(3);
    nameL_ = lab("…", barlow(16, 500), P.text); nmBox->addWidget(nameL_);
    auto* host = lab("", mono(12), P.text3); host->setObjectName("host"); nmBox->addWidget(host);
    fr->addLayout(nmBox, 1);
    v->addLayout(fr);

    auto* pr = new QHBoxLayout; pr->setSpacing(14);
    pctL_ = lab("0%", mono(40, 500), P.text); pr->addWidget(pctL_, 0, Qt::AlignBottom);
    auto* midBox = new QVBoxLayout; midBox->setSpacing(2);
    amtL_ = lab("—", barlow(13, 500), P.text2); etaL_ = lab("", barlow(12), P.text3);
    midBox->addStretch(1); midBox->addWidget(amtL_); midBox->addWidget(etaL_);
    pr->addLayout(midBox); pr->addStretch(1);
    auto* spBox = new QVBoxLayout; spBox->setSpacing(0);
    speedL_ = lab("0 B/s", mono(26, 500), P.cyan); speedL_->setAlignment(Qt::AlignRight);
    spBox->addStretch(1); spBox->addWidget(speedL_);
    spBox->addWidget(lab("SPEED", chakra(11, 600, 0.10), P.text3), 0, Qt::AlignRight);
    pr->addLayout(spBox);
    v->addLayout(pr);

    fileMeter_ = new LedMeter(34, 0, 14, 3); fileMeter_->setPalette(pal_);
    v->addWidget(fileMeter_);

    noteL_ = lab("", barlow(12), P.text2); noteL_->setWordWrap(true);
    noteL_->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent;border-left:2px solid " +
                          P.amber.name() + ";padding-left:8px");
    noteL_->hide(); v->addWidget(noteL_);

    auto* well = new Surface(Surface::Well); well->setPalette(pal_);
    auto* kv = new QVBoxLayout(well); kv->setContentsMargins(12, 11, 12, 11); kv->setSpacing(8);
    auto kvRow = [&](const char* k, QColor vc) -> QLabel* {
        auto* r = new QHBoxLayout;
        r->addWidget(lab(QString(k).toUpper(), chakra(11, 600, 0.08), P.text3));
        r->addStretch(1);
        auto* vv = lab("—", mono(12), vc); r->addWidget(vv); kv->addLayout(r); return vv;
    };
    kvSaving_ = kvRow("Saving to", P.text);
    kvResume_ = kvRow("Resume", P.green);
    kvLimit_ = kvRow("Speed limit", P.text);
    v->addWidget(well); v->addStretch(1);
    root->addWidget(wrapScroll(body), 1);

    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(pal_);
    footer->setFixedHeight(64);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(14, 0, 10, 0); fl->setSpacing(10);
    openChk_ = new QCheckBox("Open folder when done");
    openChk_->setStyleSheet(QString("QCheckBox{color:%1;background:transparent;spacing:8px}"
                                    "QCheckBox::indicator{width:18px;height:18px;background:%2;border:1px solid %3}"
                                    "QCheckBox::indicator:checked{background:%4;border:1px solid %4}")
                                .arg(P.text2.name(), P.well.name(), P.bevelLight.name(), P.amber.name()));
    fl->addWidget(openChk_); fl->addStretch(1);
    stopBtn_ = new IndBtn("Cancel"); stopBtn_->setPalette(pal_);
    stopBtn_->setToolTip("Cancel this download (removes it from the queue)");
    pauseBtn_ = new IndBtn("Pause", true); pauseBtn_->setPalette(pal_);
    closeFooterBtn_ = new IndBtn("Close"); closeFooterBtn_->setPalette(pal_); closeFooterBtn_->hide();
    linkBtn_ = new IndBtn("New link…"); linkBtn_->setPalette(pal_); linkBtn_->hide();
    linkBtn_->setToolTip("Continue with a fresh link (when the old one expired)");
    fl->addWidget(linkBtn_); fl->addWidget(stopBtn_); fl->addWidget(pauseBtn_); fl->addWidget(closeFooterBtn_);
    root->addWidget(footer);

    connect(pauseBtn_, &QAbstractButton::clicked, this, &PopupWindow::onPauseResume);
    connect(stopBtn_, &QAbstractButton::clicked, this, &PopupWindow::onStop);
    connect(closeFooterBtn_, &QAbstractButton::clicked, this, [this] { close(); });
    connect(linkBtn_, &QAbstractButton::clicked, this, [this] { askNewLink(this, mgr_, id_); });
}

void PopupWindow::buildStream() {
    const Palette& P = *pal_;
    auto* root = buildChrome("Grabbing stream");

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body); v->setContentsMargins(16, 14, 16, 14); v->setSpacing(12);

    auto* fr = new QHBoxLayout; fr->setSpacing(12);
    auto* icon = new QLabel; icon->setFixedSize(52, 52); icon->setAlignment(Qt::AlignCenter);
    icon->setText("▤"); icon->setFont(mono(22)); iconL_ = icon;
    icon->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " + P.bevelDark.name() +
                        ";color:" + P.amber.name());
    fr->addWidget(icon);
    auto* nmBox = new QVBoxLayout; nmBox->setSpacing(3);
    nameL_ = lab("video.mp4", barlow(16, 500), P.text); nmBox->addWidget(nameL_);
    auto* host = lab("HLS/DASH stream", mono(12), P.text3); host->setObjectName("host"); nmBox->addWidget(host);
    fr->addLayout(nmBox, 1);
    v->addLayout(fr);

    auto* pr = new QHBoxLayout; pr->setSpacing(14);
    pctL_ = lab("0%", mono(40, 500), P.text); pr->addWidget(pctL_, 0, Qt::AlignBottom);
    auto* midBox = new QVBoxLayout; midBox->setSpacing(2);
    amtL_ = lab("Reading…", barlow(13, 500), P.text2); etaL_ = lab("", barlow(12), P.text3);
    midBox->addStretch(1); midBox->addWidget(amtL_); midBox->addWidget(etaL_);
    pr->addLayout(midBox); pr->addStretch(1);
    auto* spBox = new QVBoxLayout; spBox->setSpacing(0);
    speedL_ = lab("—", mono(26, 500), P.cyan); speedL_->setAlignment(Qt::AlignRight);
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
        h->addLayout(tb, 1); v->addLayout(h); return mark;
    };
    step1_ = stepRow("Read the playlist", "pick the best quality");
    step2_ = stepRow("Download and decrypt parts", "aes-128 when encrypted");
    step3_ = stepRow("Join parts into one MP4", "ffmpeg, no re-encoding");
    v->addStretch(1);
    root->addWidget(wrapScroll(body), 1);

    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(pal_);
    footer->setFixedHeight(64);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(14, 0, 10, 0); fl->setSpacing(10);
    fl->addWidget(lab("Stream grabs can't be paused", barlow(12), P.text3));
    fl->addStretch(1);
    pauseBtn_ = new IndBtn("Pause"); pauseBtn_->setPalette(pal_); pauseBtn_->setEnabled(false);
    stopBtn_ = new IndBtn("Cancel"); stopBtn_->setPalette(pal_);
    stopBtn_->setToolTip("Cancel this download (removes it from the queue)");
    closeFooterBtn_ = new IndBtn("Close"); closeFooterBtn_->setPalette(pal_); closeFooterBtn_->hide();
    linkBtn_ = new IndBtn("New link…"); linkBtn_->setPalette(pal_); linkBtn_->hide();
    linkBtn_->setToolTip("Grab again from a fresh playlist link");
    fl->addWidget(linkBtn_); fl->addWidget(pauseBtn_); fl->addWidget(stopBtn_); fl->addWidget(closeFooterBtn_);
    root->addWidget(footer);

    // Disabled while grabbing (no pause); becomes "Retry" if the grab fails.
    connect(pauseBtn_, &QAbstractButton::clicked, this, &PopupWindow::onPauseResume);
    connect(stopBtn_, &QAbstractButton::clicked, this, &PopupWindow::onStop);
    connect(closeFooterBtn_, &QAbstractButton::clicked, this, [this] { close(); });
    connect(linkBtn_, &QAbstractButton::clicked, this, [this] { askNewLink(this, mgr_, id_); });
}

// ---- actions (delegate to the manager) -------------------------------------
void PopupWindow::onPauseResume() {
    auto it = item(); if (!it) return;
    // A failed download continues from its saved progress, same as a paused one.
    if (it->status == ST_PAUSED || it->status == ST_ERROR) mgr_->resume(id_); else mgr_->pause(id_);
}
void PopupWindow::onStop() {
    mgr_->remove(id_, /*delete_file=*/false);  // abort + drop from queue (keep partial)
    close();
}
void PopupWindow::onOpenFolder() {
    auto it = item(); if (!it || it->dest_path.empty()) return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(fs::path(it->dest_path).parent_path().string())));
}

// ---- live view -------------------------------------------------------------
void PopupWindow::tick() {
    const Palette& P = *pal_;
    auto it = item();
    if (!it) {  // removed from the queue elsewhere -> close the view
        close();
        return;
    }
    // Show the download's real name. Browser jobs often arrive without a filename
    // ("automatic"); use the resolved name once the engine knows it, falling back
    // to the URL's last path segment so it's never the whole raw URL.
    QString name = QString::fromStdString(it->filename);
    if (name.isEmpty() && !it->dest_path.empty())
        name = QString::fromStdString(fs::path(it->dest_path).filename().string());
    if (name.isEmpty()) {
        QString seg = QUrl(QString::fromStdString(it->url)).fileName();
        name = seg.isEmpty() ? QString::fromStdString(it->url) : seg;
    }
    if (nameL_) nameL_->setText(name);
    // File-type icon: refresh when the real name arrives, and once more when a
    // finished program can show its own icon.
    QString iconKey = name + (it->status == ST_COMPLETED ? "|done" : "");
    if (iconL_ && iconKey != iconName_) {
        iconName_ = iconKey;
        QString iname = (stream_ && !name.contains('.')) ? name + ".mp4" : name;
        iconL_->setPixmap(fileIconFor(iname, it->status == ST_COMPLETED ? QString::fromStdString(it->dest_path)
                                                                         : QString(), 40, *pal_));
    }
    if (auto* h = findChild<QLabel*>("host")) {
        QString host = QString::fromStdString(it->url);
        QUrl u(QString::fromStdString(it->url)); if (!u.host().isEmpty()) host = u.host();
        h->setText(stream_ ? ("HLS/DASH stream · " + host) : host);
    }
    bool done = it->status == ST_COMPLETED;
    bool failed = it->status == ST_ERROR;
    bool paused = it->status == ST_PAUSED;
    bool hashing = it->hash_state == "checking";
    int pct = it->percent() ? (int)(*it->percent() + 0.5) : (done ? 100 : 0);
    QString failText = QString::fromStdString(it->error);
    if (QString rt = autoRetryText(mgr_, *it); failed && !rt.isEmpty()) failText += " · " + rt;

    if (stream_) {
        titleL_->setText(done ? "Stream saved" : failed ? "Stream failed" : QString("Grabbing stream %1%").arg(pct));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(it->seg_total ? QString("Part %1 of %2").arg(it->seg_done).arg(it->seg_total)
                                     : QString(done ? "Done" : "Reading…"));
        etaL_->setText(failed ? failText : "");
        if (streamMeter_) streamMeter_->set(pct / 100.0, done ? P.green : P.cyan);
        auto setStep = [&](QLabel* m, bool active, bool complete) {
            if (!m) return;
            m->setText(complete ? "✓" : active ? "↓" : "⏳");
            m->setStyleSheet("background:" + P.well.name(QColor::HexArgb) + ";border:1px solid " +
                             (active ? P.cyan.name() : P.bevelDark.name()) + ";color:" +
                             (complete ? P.green.name() : active ? P.cyan.name() : P.text3.name()));
        };
        setStep(step1_, it->seg_total == 0 && !done && !failed, it->seg_total > 0 || done);
        setStep(step2_, it->seg_total > 0 && !done, done);
        setStep(step3_, false, done);
    } else {
        titleL_->setText(hashing ? "Verifying checksum…" : done ? "Completed" : paused ? "Paused" : failed ? "Failed"
                         : it->status == ST_QUEUED ? "Queued" : QString("Downloading %1%").arg(pct));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(human((double)it->downloaded) + " of " +
                       (it->size ? human((double)*it->size) : QString("?")));
        etaL_->setText(failed ? failText
                       : hashing ? QString("Checking the finished file…")
                       : (!done && !paused && it->eta) ? htime(*it->eta)
                       : done ? (it->hash_state == "ok" ? "Done · checksum OK" : "Done") : "");
        speedL_->setText(it->status == ST_DOWNLOADING ? human(it->speed) + "/s" : "—");
        if (fileMeter_) fileMeter_->set(pct / 100.0, done ? P.green : failed ? P.red : P.cyan);
        if (noteL_) {
            const SegmentProgress* stolen = nullptr;
            for (auto& c : it->conn_stats) if (c.stole_from >= 0) { stolen = &c; break; }
            if (stolen) { noteL_->setText(QString("%1 connections. C%2 finished early and took over part of C%3's remaining range.")
                                              .arg((int)it->conn_stats.size()).arg(stolen->index + 1).arg(stolen->stole_from + 1)); noteL_->show(); }
            else noteL_->hide();
        }
        if (kvSaving_) kvSaving_->setText(QString::fromStdString(
            it->dest_path.empty() ? "—" : fs::path(it->dest_path).parent_path().string()));
        if (kvResume_) kvResume_->setText(done ? "COMPLETE" : (paused || failed) ? "SAVED" : "SUPPORTED");
        if (kvLimit_) {
            long long per = it->max_rate_bps;             // per-download limit (0 = none)
            double glob = mgr_ ? mgr_->global_rate_bps() : 0.0;  // manager-wide cap
            if (per > 0) kvLimit_->setText(human((double)per) + "/s");
            else if (glob > 0) kvLimit_->setText(human(glob) + "/s · global");
            else kvLimit_->setText("Unlimited");
        }
    }

    // Footer: finished -> Close; failed -> New link… / Cancel / Retry (continues
    // from the saved progress); otherwise Cancel / Pause|Resume. Re-evaluated every
    // tick so a retried download gets its normal buttons back.
    if (linkBtn_) linkBtn_->setVisible(failed);
    if (openChk_) openChk_->setVisible(!failed);  // make room for the three buttons
    if (done) {
        if (pauseBtn_) pauseBtn_->hide();
        if (stopBtn_) stopBtn_->hide();
        if (closeFooterBtn_) closeFooterBtn_->show();
        if (!openedFolder_ && openChk_ && openChk_->isChecked()) { openedFolder_ = true; onOpenFolder(); }
    } else if (pauseBtn_) {
        pauseBtn_->show();
        if (stopBtn_) stopBtn_->show();
        if (closeFooterBtn_) closeFooterBtn_->hide();
        pauseBtn_->setText(failed ? "Retry" : stream_ ? "Pause" : paused ? "Resume" : "Pause");
        pauseBtn_->setEnabled(failed || !stream_);
        pauseBtn_->setToolTip(failed ? (stream_ ? "Grab the stream again" : "Continue from where it stopped")
                                     : QString());
    }
}

bool askNewLink(QWidget* parent, DownloadManager* mgr, const std::string& id) {
    if (!mgr) return false;
    QString cur;
    for (auto& it : mgr->ordered()) if (it->id == id) { cur = QString::fromStdString(it->url); break; }
    bool ok = false;
    QString url = QInputDialog::getText(parent, "New link",
                                        "Paste a fresh link for this download. If it's the same file,\n"
                                        "it continues from where it stopped; otherwise it starts over.",
                                        QLineEdit::Normal, cur, &ok).trimmed();
    if (!ok || url.isEmpty() || url == cur) return false;
    QUrl u(url);
    const QString scheme = u.scheme().toLower();
    if (!u.isValid() || u.host().isEmpty() || (scheme != "http" && scheme != "https" && scheme != "ftp")) {
        QMessageBox::warning(parent, "New link", "That doesn't look like a download link (http, https or ftp).");
        return false;
    }
    if (!mgr->set_url(id, url.toStdString())) return false;
    mgr->resume(id);
    return true;
}

bool askChecksum(QWidget* parent, DownloadManager* mgr, const std::string& id) {
    if (!mgr) return false;
    QString cur;
    for (auto& it : mgr->ordered()) if (it->id == id) { cur = QString::fromStdString(it->expected_hash); break; }
    bool ok = false;
    QString text = QInputDialog::getText(parent, "Checksum",
                                         "Paste the checksum the website lists for this file\n"
                                         "(SHA-256, SHA-1, SHA-512 or MD5). Leave empty to remove it.",
                                         QLineEdit::Normal, cur, &ok);
    if (!ok) return false;
    if (!mgr->set_expected_hash(id, text.toStdString())) {
        QMessageBox::warning(parent, "Checksum",
                             "That doesn't look like a checksum. Paste the long string of letters and digits\n"
                             "(for example from a SHA256SUMS file).");
        return false;
    }
    return true;
}

QString autoRetryText(DownloadManager* mgr, const DownloadItem& it) {
    if (!mgr) return {};
    double in = mgr->auto_retry_in(it);
    if (in < 0) return {};
    if (in < 1) return "retrying now";
    if (in < 60) return QString("retrying in %1 s").arg((int)std::ceil(in));
    return QString("retrying in %1 min").arg((int)std::ceil(in / 60.0));
}

// ---- window chrome ---------------------------------------------------------
void PopupWindow::paintEvent(QPaintEvent*) {
    QPainter g(this);
    g.fillRect(rect(), pal_->frame);  // behind the outer frame (seamless)
}

} // namespace mpdm::gui
