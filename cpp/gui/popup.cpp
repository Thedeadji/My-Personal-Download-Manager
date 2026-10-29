#include "popup.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

#include "mpdm/manager.hpp"
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

PopupWindow::PopupWindow(DownloadManager* mgr, const std::string& itemId, bool stream,
                         const Palette* pal, QWidget* parent)
    : QWidget(parent), mgr_(mgr), id_(itemId), stream_(stream), pal_(pal) {
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle("MPDM");
    setWindowIcon(Logo::icon());
    resize(440, stream_ ? 440 : 450);
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

// ---- UI --------------------------------------------------------------------
void PopupWindow::buildFile() {
    const Palette& P = *pal_;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1); root->setSpacing(0);

    auto* title = new Surface(Surface::TitleBar); title->setPalette(pal_);
    title->setFixedHeight(48);
    auto* tl = new QHBoxLayout(title); tl->setContentsMargins(14, 0, 10, 0); tl->setSpacing(10);
    tl->addWidget(new Logo(22));
    titleL_ = lab("Downloading", chakra(16, 600, 0.0), P.text);
    tl->addWidget(titleL_); tl->addStretch(1);
    auto* x = lab("✕", barlow(16), P.text3); x->setObjectName("xclose"); x->setCursor(Qt::PointingHandCursor);
    tl->addWidget(x);
    root->addWidget(title);

    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body); v->setContentsMargins(16, 14, 16, 14); v->setSpacing(12);

    auto* fr = new QHBoxLayout; fr->setSpacing(12);
    auto* icon = new QLabel; icon->setFixedSize(52, 52); icon->setAlignment(Qt::AlignCenter);
    icon->setText("▣"); icon->setFont(mono(22));
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
    kvRow("Speed limit", P.text)->setText("—");
    v->addWidget(well); v->addStretch(1);
    root->addWidget(body, 1);

    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(pal_);
    footer->setFixedHeight(64);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(14, 0, 10, 0); fl->setSpacing(10);
    openChk_ = new QCheckBox("Open folder when done");
    openChk_->setStyleSheet(QString("QCheckBox{color:%1;background:transparent;spacing:8px}"
                                    "QCheckBox::indicator{width:18px;height:18px;background:%2;border:1px solid %3}"
                                    "QCheckBox::indicator:checked{background:%4;border:1px solid %4}")
                                .arg(P.text2.name(), P.well.name(), P.bevelLight.name(), P.amber.name()));
    fl->addWidget(openChk_); fl->addStretch(1);
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
    auto* icon = new QLabel; icon->setFixedSize(52, 52); icon->setAlignment(Qt::AlignCenter);
    icon->setText("▤"); icon->setFont(mono(22));
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

// ---- actions (delegate to the manager) -------------------------------------
void PopupWindow::onPauseResume() {
    auto it = item(); if (!it) return;
    if (it->status == ST_PAUSED) mgr_->resume(id_); else mgr_->pause(id_);
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
    QString name = QString::fromStdString(it->filename.empty() ? it->url : it->filename);
    if (nameL_) nameL_->setText(name);
    if (auto* h = findChild<QLabel*>("host")) {
        QString host = QString::fromStdString(it->url);
        QUrl u(QString::fromStdString(it->url)); if (!u.host().isEmpty()) host = u.host();
        h->setText(stream_ ? ("HLS/DASH stream · " + host) : host);
    }
    bool done = it->status == ST_COMPLETED;
    bool failed = it->status == ST_ERROR;
    bool paused = it->status == ST_PAUSED;
    int pct = it->percent() ? (int)(*it->percent() + 0.5) : (done ? 100 : 0);

    if (stream_) {
        titleL_->setText(done ? "Stream saved" : failed ? "Stream failed" : QString("Grabbing stream %1%").arg(pct));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(it->seg_total ? QString("Part %1 of %2").arg(it->seg_done).arg(it->seg_total)
                                     : QString(done ? "Done" : "Reading…"));
        etaL_->setText(failed ? QString::fromStdString(it->error) : "");
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
        titleL_->setText(done ? "Completed" : paused ? "Paused" : failed ? "Failed"
                         : it->status == ST_QUEUED ? "Queued" : QString("Downloading %1%").arg(pct));
        pctL_->setText(QString("%1%").arg(pct));
        amtL_->setText(human((double)it->downloaded) + " of " +
                       (it->size ? human((double)*it->size) : QString("?")));
        etaL_->setText(failed ? QString::fromStdString(it->error)
                       : (!done && !paused && it->eta) ? htime(*it->eta) : done ? "Done" : "");
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
        if (kvResume_) kvResume_->setText(done ? "COMPLETE" : paused ? "SAVED" : "SUPPORTED");
    }

    bool ended = done || failed;
    if (ended) {
        if (pauseBtn_) pauseBtn_->hide();
        if (stopBtn_) stopBtn_->hide();
        if (closeFooterBtn_) closeFooterBtn_->show();
        if (done && !openedFolder_ && openChk_ && openChk_->isChecked()) { openedFolder_ = true; onOpenFolder(); }
    } else if (!stream_ && pauseBtn_) {
        pauseBtn_->setText(paused ? "Resume" : "Pause");
    }
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
        if (auto* x = findChild<QLabel*>("xclose")) {
            QPoint lp = x->mapFromGlobal(e->globalPosition().toPoint());
            if (x->rect().contains(lp)) { close(); return; }
        }
        dragPos_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
        e->accept();
    }
}
void PopupWindow::mouseMoveEvent(QMouseEvent* e) {
    if ((e->buttons() & Qt::LeftButton) && !dragPos_.isNull())
        move(e->globalPosition().toPoint() - dragPos_);
}

} // namespace mpdm::gui
