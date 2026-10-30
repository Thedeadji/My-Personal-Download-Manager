#include "mainwindow.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QUrl>
#include <QListView>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSessionManager>
#include <QScrollArea>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSizePolicy>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QThread>
#include <QWidgetAction>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "fileicon.hpp"
#include "popup.hpp"

namespace mpdm::gui {

static QString human(double n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + u[i];
}
static QString htime(double s) {
    if (s <= 0) return "—";
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60;
    return h ? QString("%1h %2m").arg(h).arg(m) : (m ? QString("%1 min").arg(m) : QString("%1s").arg(t));
}
// The file-type icon for a download: by its name (or the URL's last segment
// before the name is known); finished programs show their own icon.
static QPixmap itemIcon(const DownloadItem& it, int size, const Palette& P) {
    QString name = QString::fromStdString(it.filename);
    if (name.isEmpty() && !it.dest_path.empty())
        name = QString::fromStdString(std::filesystem::path(it.dest_path).filename().string());
    if (name.isEmpty()) name = QUrl(QString::fromStdString(it.url)).fileName();
    if (it.stream && !name.contains('.')) name += ".mp4";
    QString path = it.status == ST_COMPLETED ? QString::fromStdString(it.dest_path) : QString();
    return fileIconFor(name, path, size, P);
}

static double mono_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const Palette* paletteForMode(const QString& mode) {
    if (mode == "light") return &kLight;
    if (mode == "dark") return &kDark;
    // system
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light ? &kLight : &kDark;
#else
    // Qt < 6.5 (e.g. Ubuntu 24.04's 6.4) has no colorScheme(): infer it from the
    // platform palette's window color instead.
    return qApp->palette().color(QPalette::Window).lightness() > 128 ? &kLight : &kDark;
#endif
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("MyPersonalDownloadManager");
    QSettings s;
    pal_ = paletteForMode(s.value("appearance", "dark").toString());
    setWindowIcon(Logo::icon());
    // Frameless: the app draws its own title bar (shield + controls), per the
    // Industrial mockup — no duplicate native OS title bar. Move/resize/maximize
    // are provided by FramelessChrome + the title-bar buttons (see buildUi).
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

    // Resolution-friendly sizing: the full layout targets ~1300x860, but it must
    // also fit small screens without clipping. Open clamped to the available
    // screen area and centered; a low minimum size plus the scroll-wrapped body
    // (see buildUi) keep it usable from 800x600 up to 1920x1080 — below the
    // layout's natural size the content scrolls instead of being cut off.
    setMinimumSize(460, 360);
    if (auto* scr = QGuiApplication::primaryScreen()) {
        const QRect av = scr->availableGeometry();
        int w = std::min(1300, (int)(av.width() * 0.96));
        int h = std::min(860, (int)(av.height() * 0.96));
        resize(w, h);
        move(av.center() - QPoint(w / 2, h / 2));
    } else {
        resize(1300, 860);
    }
    manager_.start_scheduler();
    // Session logout/shutdown (all platforms, incl. Linux session managers): let
    // the app really quit instead of hiding to the tray and blocking the logout.
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this](QSessionManager&) { allowQuit_ = true; });
    // "Ask each time" duplicate policy: the engine (on a worker thread) asks us to
    // decide; marshal to the GUI thread and show the dialog there.
    manager_.set_duplicate_resolver([this](const std::string& path) -> std::string {
        std::string result = "rename";
        auto ask = [this, &path, &result] { result = askDuplicate(QString::fromStdString(path)).toStdString(); };
        if (QThread::currentThread() == this->thread()) ask();
        else QMetaObject::invokeMethod(this, ask, Qt::BlockingQueuedConnection);
        return result;
    });
    // Seed already-finished items so the "download complete" dialog only fires for
    // downloads that finish while the app is open (not for history on startup).
    for (auto& it : manager_.ordered())
        if (it->status == ST_COMPLETED) finishAnnounced_.insert(it->id);
    buildUi();
    applyTheme();
    setupTray();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_.start(1000);
    connect(&jobTimer_, &QTimer::timeout, this, &MainWindow::pollJobs);
    jobTimer_.start(500);
    refresh();
    pollJobs();  // adopt anything already waiting (e.g. we were launched for it)
}

void MainWindow::pollJobs() {
    bool showPopups = QSettings().value("show_popups", true).toBool();
    for (const auto& id : manager_.ingest_jobs()) {
        bool stream = false;
        for (auto& it : manager_.ordered())
            if (it->id == id) { stream = it->stream; break; }
        if (showPopups) openPopupFor(id, stream);
    }
}

void MainWindow::devShotGraph(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    if (graph_) {
        for (int i = 0; i < 64; ++i) {
            double t = i / 63.0;
            double net = (i < 6) ? 0.0 : 7.0e6 + 3.5e6 * std::sin(t * 6.2832 * 2.2) + (i > 20 && i < 58 ? 2.0e6 : 0);
            double disk = (net > 0) ? std::max(0.0, net * 0.82 + 7.0e5 * std::sin(t * 6.2832 * 3.3)) : 0.0;
            graph_->push(net, disk);
        }
        auto set = [&](const char* id, const QString& t) { if (auto* l = findChild<QLabel*>(id)) l->setText(t); };
        set("t_now", "NET 8.8MB/s"); set("t_disk", "DISK 7.2MB/s"); set("t_peak", "PEAK " + human(graph_->peak()) + "/s");
    }
    QTimer::singleShot(500, [this, out] { this->grab().save(out); qApp->quit(); });
}

void MainWindow::devShotPopup(const QString& out, bool stream) {
    // Create a synthetic, non-running queue item with representative progress and
    // open a bound popup for it, then grab it to PNG. Dev/verification only.
    jobTimer_.stop(); timer_.stop();
    auto it = manager_.add(stream ? "https://media.campus-lectures.example/lecture-07/x.m3u8"
                                  : "https://releases.ubuntu.com/24.04.1/ubuntu-24.04.1-desktop-amd64.iso",
                           stream ? "lecture-07-distributed-systems.mp4" : "ubuntu-24.04.1-desktop-amd64.iso",
                           0, {}, /*start=*/false, 0, false, stream);
    it->status = ST_DOWNLOADING;
    if (stream) { it->seg_done = 412; it->seg_total = 980; it->size = 980; it->downloaded = 412; }
    else {
        it->size = 6228000000LL; it->downloaded = 4000000000LL; it->speed = 9800000; it->eta = 240;
        it->max_rate_bps = 2LL * 1024 * 1024;  // 2 MB/s, to show the Speed limit row
        it->dest_path = "D:/Downloads/Programs/ubuntu-24.04.1-desktop-amd64.iso";
        SegmentProgress a; a.index = 0; a.start = 0; a.end = 100; a.downloaded = 95; a.speed_bps = 1300000; a.active = true; a.stole_from = -1;
        SegmentProgress b; b.index = 7; b.start = 700; b.end = 760; b.downloaded = 30; b.speed_bps = 1100000; b.active = true; b.stole_from = 7;
        it->conn_stats = {a, b};
    }
    openPopupFor(it->id, stream);
    auto* pw = popups_.back();
    QTimer::singleShot(600, pw, [pw] { pw->repaint(); });
    QTimer::singleShot(900, [pw, out] { pw->grab().save(out); qApp->quit(); });
}

void MainWindow::devShotTrayMenu(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    auto a = manager_.add("https://example.com/ubuntu-24.04.1.iso", "ubuntu-24.04.1.iso", 0, {}, false, 0, false, false);
    a->status = ST_DOWNLOADING; a->speed = 9.4e6; a->size = 1000; a->downloaded = 650;
    auto b = manager_.add("https://example.com/lecture.mp4", "lecture.mp4", 0, {}, false, 0, false, false);
    b->status = ST_DOWNLOADING; b->speed = 5.1e6;
    auto c = manager_.add("https://example.com/pack.zip", "pack.zip", 0, {}, false, 0, false, false);
    c->status = ST_PAUSED;
    auto* m = buildTrayMenu();
    m->popup(QPoint(16, 16));
    QTimer::singleShot(600, [m, out] { m->grab().save(out); qApp->quit(); });
}

void MainWindow::openPopupFor(const std::string& id, bool stream) {
    // don't open a second popup for the same item
    popups_.erase(std::remove(popups_.begin(), popups_.end(), nullptr), popups_.end());
    for (auto* p : popups_) if (p && p->itemId() == id) { p->show(); p->raise(); return; }
    auto* pw = new PopupWindow(&manager_, id, stream, pal_);
    pw->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(pw, &QObject::destroyed, this, [this, pw] {
        popups_.erase(std::remove(popups_.begin(), popups_.end(), pw), popups_.end());
    });
    popups_.push_back(pw);
    // center-ish on screen
    if (auto* scr = QGuiApplication::primaryScreen()) {
        QRect a = scr->availableGeometry();
        int off = (int)popups_.size() * 24;
        pw->move(a.center() - QPoint(pw->width() / 2 - off, pw->height() / 2 - off));
    }
    pw->show();
    pw->raise();
    pw->activateWindow();
}

// A Lucide-style line icon, hand-painted on a 24x24 grid (matches how Logo is
// drawn, so no Qt Svg dependency). Tinted `color`, round caps/joins.
static QIcon trayIcon(const QString& name, const QColor& color, int sz = 18) {
    qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(QSize(sz, sz) * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
    QPainter g(&pm);
    g.setRenderHint(QPainter::Antialiasing, true);
    g.scale(sz / 24.0, sz / 24.0);
    QPen pen(color, 2); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin);
    g.setPen(pen); g.setBrush(Qt::NoBrush);
    QPainterPath p;
    if (name == "plus") {
        g.drawLine(QPointF(5, 12), QPointF(19, 12));
        g.drawLine(QPointF(12, 5), QPointF(12, 19));
    } else if (name == "play") {
        p.moveTo(7, 4); p.lineTo(20, 12); p.lineTo(7, 20); p.closeSubpath(); g.drawPath(p);
    } else if (name == "pause") {
        g.drawRoundedRect(QRectF(6.5, 4, 3.5, 16), 1.2, 1.2);
        g.drawRoundedRect(QRectF(14, 4, 3.5, 16), 1.2, 1.2);
    } else if (name == "power") {
        g.drawLine(QPointF(12, 3), QPointF(12, 12));
        g.drawArc(QRectF(4, 5, 16, 16), 120 * 16, 300 * 16);  // ring open at the top
    } else if (name == "extlink") {
        g.drawRoundedRect(QRectF(4, 10, 11, 10), 2, 2);
        g.drawLine(QPointF(12, 12), QPointF(20, 4));
        p.moveTo(14, 4); p.lineTo(20, 4); p.lineTo(20, 10); g.drawPath(p);
    } else if (name == "download") {
        g.drawLine(QPointF(12, 3), QPointF(12, 15));
        p.moveTo(7, 11); p.lineTo(12, 16); p.lineTo(17, 11); g.drawPath(p);
        g.drawLine(QPointF(5, 21), QPointF(19, 21));
    }
    g.end();
    return QIcon(pm);
}

// Tray icon with a state overlay painted on the shield (per the tray design):
// downloading -> cyan progress bar along the bottom edge; paused -> violet dot;
// failed -> red dot; all done -> green dot; idle -> plain shield. Rendered at the
// standard small sizes so it stays sharp at every Windows scale.
enum { TrayIdle, TrayDownloading, TrayPaused, TrayFailed, TrayDone };
static QIcon makeTrayIcon(int state, double frac) {
    QIcon ic;
    for (int sz : {16, 20, 24, 32}) {
        QPixmap pm = Logo::pixmap(sz);
        if (state != TrayIdle) {
            QPainter g(&pm);
            g.setRenderHint(QPainter::Antialiasing, true);
            if (state == TrayDownloading) {
                qreal barH = std::max(2.0, sz * 5.0 / 64.0);
                g.fillRect(QRectF(0, sz - barH, sz * std::clamp(frac, 0.05, 1.0), barH), QColor("#3BE0FF"));
            } else {
                QColor c = state == TrayFailed ? QColor("#FF6B60")
                         : state == TrayPaused ? QColor("#A68CFF") : QColor("#6BD66F");
                qreal r = sz * 10.0 / 64.0;
                g.setPen(QPen(QColor("#0C1116"), std::max(1.0, sz * 3.0 / 64.0)));
                g.setBrush(c);
                g.drawEllipse(QPointF(sz * 50.0 / 64.0, sz * 50.0 / 64.0), r, r);
            }
            g.end();
        }
        ic.addPixmap(pm);
    }
    return ic;
}

QMenu* MainWindow::buildTrayMenu() {
    const Palette& P = *pal_;
    auto hx = [](const QColor& c) { return c.name(QColor::HexArgb); };
    auto* menu = new QMenu(this);
    // Industrial styling: dark panel, 40px rows, amber hover with a left edge,
    // inset separators (see the tray design reference).
    menu->setStyleSheet(QString(
        "QMenu{background:%1;border:1px solid %2;padding:6px}"
        "QMenu::item{padding:11px 16px 11px 12px;margin:0;color:%3;background:transparent;"
            "border-left:2px solid transparent}"
        "QMenu::item:selected{background:%4;color:%5;border-left:2px solid %5}"
        "QMenu::item:disabled{color:%6;background:transparent}"
        "QMenu::separator{height:1px;background:%7;margin:5px 4px}"
        "QMenu::icon{padding-left:8px}"
        "QMenu::indicator{width:16px;height:16px;left:10px}"
        "QMenu::indicator:checked{background:%5;border:1px solid %5}"
        "QMenu::indicator:unchecked{background:%8;border:1px solid %2}")
        .arg(hx(P.panel), hx(P.bevelLight), hx(P.text), hx(P.rowSelected),
             hx(P.amber), hx(P.text3), hx(P.well), hx(P.well)));

    // --- non-interactive status header (lamp + "N DOWNLOADING" + speed) ---
    auto* header = new QWidget;
    auto* hl = new QHBoxLayout(header); hl->setContentsMargins(10, 6, 12, 8); hl->setSpacing(8);
    auto* lamp = new QLabel; lamp->setFixedSize(7, 7);
    auto* hText = new QLabel("IDLE"); hText->setFont(mono(11));
    auto* hSpeed = new QLabel; hSpeed->setFont(mono(11));
    hl->addWidget(lamp); hl->addWidget(hText); hl->addStretch(1); hl->addWidget(hSpeed);
    auto* headerAct = new QWidgetAction(menu); headerAct->setDefaultWidget(header);
    headerAct->setEnabled(false);
    menu->addAction(headerAct);

    // --- items ---
    // Top item: open the live download popup(s). Keeps the amber "open" icon.
    // (Opening the app itself now lives on the left-click quick panel's Open button.)
    auto* openAct = menu->addAction(trayIcon("extlink", P.amber), "Open download popup", this,
                                    [this] { showDownloadPopups(); });
    { QFont f = openAct->font(); f.setWeight(QFont::Medium); openAct->setFont(f); }
    menu->addAction(trayIcon("plus", P.text2), "Add downloads…", this, [this] { showFromTray(); onAdd(); });
    menu->addSeparator();
    auto* resumeAct = menu->addAction(trayIcon("play", P.text2), "Resume all", this,
                                      [this] { manager_.resume_all(); refresh(); });
    auto* pauseAct = menu->addAction(trayIcon("pause", P.text2), "Pause all", this,
                                     [this] { manager_.pause_all(); refresh(); });
    menu->addSeparator();
    menu->addAction(trayIcon("power", P.red), "Exit MPDM", this, [this] { quitApp(); });

    // Refresh the live header + hints each time the menu opens.
    connect(menu, &QMenu::aboutToShow, this, [this, lamp, hText, hSpeed, resumeAct, pauseAct, hx] {
        int running = 0, paused = 0; double spd = 0;
        for (auto& it : manager_.ordered()) {
            if (it->status == ST_DOWNLOADING) { running++; spd += it->speed; }
            else if (it->status == ST_PAUSED) paused++;
        }
        const Palette& Pp = *pal_;
        lamp->setStyleSheet(QString("background:%1;border-radius:3px")
                                .arg(hx(running ? Pp.cyan : Pp.text3)));
        hText->setText(running ? QString("%1 DOWNLOADING").arg(running) : "IDLE");
        hText->setStyleSheet("color:" + hx(Pp.text2) + ";background:transparent");
        hSpeed->setText(running ? human(spd) + "/s" : "");
        hSpeed->setStyleSheet("color:" + hx(Pp.cyan) + ";background:transparent");
        resumeAct->setText(paused ? QString("Resume all\t%1 PAUSED").arg(paused) : "Resume all");
        pauseAct->setText(running ? QString("Pause all\t%1 RUNNING").arg(running) : "Pause all");
    });
    return menu;
}

void MainWindow::devShotTrayIcons(const QString& out) {
    const int states[] = {TrayIdle, TrayDownloading, TrayPaused, TrayFailed, TrayDone};
    const int n = 5, sz = 64, pad = 12;
    QPixmap strip((sz + pad) * n + pad, sz + 2 * pad);
    strip.fill(QColor("#15191E"));
    QPainter g(&strip);
    for (int i = 0; i < n; ++i)
        g.drawPixmap(pad + i * (sz + pad), pad, makeTrayIcon(states[i], 0.66).pixmap(sz, sz));
    g.end();
    strip.save(out);
    qApp->quit();
}

void MainWindow::devShotIcons(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    const char* names[] = {"holiday-video.mp4", "podcast-ep12.mp3", "report-2026.pdf", "project-backup.zip",
                           "setup-tool.exe", "thesis-draft.docx", "budget.xlsx", "dataset.xyz"};
    std::string first;
    for (const char* n : names) {
        auto a = manager_.add(std::string("https://example.com/") + n, n, 0, {}, false, 0, false, false);
        a->status = ST_PAUSED; a->size = 50000000LL; a->downloaded = 21000000LL;
        if (first.empty()) first = a->id;
    }
    selId_ = QString::fromStdString(first);
    refresh();
    QTimer::singleShot(500, [this, out] { this->grab().save(out); qApp->quit(); });
}

void MainWindow::devShotDeselect(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    auto a = manager_.add("https://example.com/mods.zip", "mods.zip", 0, {}, false, 0, false, false);
    a->status = ST_PAUSED; a->size = 1400000000LL; a->downloaded = 680000000LL;
    selId_ = QString::fromStdString(a->id);
    refresh();                       // panel shows mods.zip
    manager_.remove(a->id, false);   // ...then it's removed
    refresh();                       // panel must return to "No selection"
    QTimer::singleShot(400, [this, out] { this->grab().save(out); qApp->quit(); });
}

void MainWindow::devShotQuickPanel(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    auto a = manager_.add("https://example.com/ubuntu-24.04.1.iso", "ubuntu-24.04.1-desktop-amd64.iso", 0, {}, false, 0, false, false);
    a->status = ST_DOWNLOADING; a->speed = 9.4e6; a->size = 1000; a->downloaded = 650;
    auto b = manager_.add("https://example.com/lecture-07.mp4", "lecture-07-distributed-systems.mp4", 0, {}, false, 0, false, true);
    b->status = ST_DOWNLOADING; b->speed = 3.1e6; b->size = 1000; b->downloaded = 420;
    auto c = manager_.add("https://example.com/interview.mov", "interview-raw-take2.mov", 0, {}, false, 0, false, false);
    c->status = ST_PAUSED; c->size = 1000; c->downloaded = 180;
    showQuickPanel();
    if (quickPanel_) quickPanel_->grab().save(out);
    qApp->quit();
}

void MainWindow::showQuickPanel() {
    const Palette& P = *pal_;
    if (quickPanel_) { quickPanel_->close(); quickPanel_ = nullptr; }
    auto hx = [](const QColor& c) { return c.name(QColor::HexArgb); };
    auto lab = [](const QString& t, QFont f, QColor c) {
        auto* l = new QLabel(t); l->setFont(f);
        l->setStyleSheet("color:" + c.name(QColor::HexArgb) + ";background:transparent");
        return l;
    };
    auto itemById = [this](const std::string& id) -> std::shared_ptr<DownloadItem> {
        for (auto& it : manager_.ordered()) if (it->id == id) return it;
        return nullptr;
    };

    auto* panel = new QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint);
    panel->setAttribute(Qt::WA_DeleteOnClose, true);
    { QPalette pp = panel->palette(); pp.setColor(QPalette::Window, P.frame); panel->setPalette(pp); }
    panel->setAutoFillBackground(true);
    panel->setFixedWidth(380);
    connect(panel, &QObject::destroyed, this, [this] { quickPanel_ = nullptr; });
    auto* v = new QVBoxLayout(panel); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(6);

    auto updaters = std::make_shared<std::vector<std::function<void()>>>();

    // ---- header: logo, status counts, total speed ----
    auto* header = new Surface(Surface::TitleBar); header->setPalette(&P);
    auto* hl = new QHBoxLayout(header); hl->setContentsMargins(12, 10, 12, 10); hl->setSpacing(10);
    hl->addWidget(new Logo(22));
    auto* hcol = new QVBoxLayout; hcol->setSpacing(1);
    hcol->addWidget(lab("MPDM", chakra(14, 600, 0.0), P.text));
    auto* hStatus = lab("", mono(11), P.text2); hcol->addWidget(hStatus);
    hl->addLayout(hcol, 1);
    auto* scol = new QVBoxLayout; scol->setSpacing(1);
    auto* hSpeed = lab("0", mono(18, 500), P.cyan); hSpeed->setAlignment(Qt::AlignRight);
    scol->addWidget(hSpeed);
    scol->addWidget(lab("MB/s", mono(11), P.text3), 0, Qt::AlignRight);
    hl->addLayout(scol);
    v->addWidget(header);

    // ---- active downloads list ----
    auto items = manager_.ordered();
    std::vector<std::shared_ptr<DownloadItem>> active;
    for (auto& it : items)
        if (it->status != ST_COMPLETED) active.push_back(it);

    auto* listPanel = new Surface(Surface::Panel); listPanel->setPalette(&P);
    auto* ll = new QVBoxLayout(listPanel); ll->setContentsMargins(6, 6, 6, 6); ll->setSpacing(2);
    if (active.empty()) {
        ll->addWidget(lab("No active downloads", barlow(13), P.text3));
    }
    int shown = 0;
    for (auto& it : active) {
        if (shown++ >= 5) break;
        std::string id = it->id;
        auto* row = new QWidget;
        auto* rl = new QHBoxLayout(row); rl->setContentsMargins(6, 6, 4, 6); rl->setSpacing(10);
        auto* iconW = new Surface(Surface::Well); iconW->setPalette(&P); iconW->setFixedSize(28, 28);
        auto* iwl = new QHBoxLayout(iconW); iwl->setContentsMargins(0, 0, 0, 0);
        auto* fic = new QLabel; fic->setPixmap(itemIcon(*it, 20, P));
        fic->setStyleSheet("background:transparent");
        iwl->addWidget(fic, 0, Qt::AlignCenter);
        rl->addWidget(iconW);
        auto* tcol = new QVBoxLayout; tcol->setSpacing(4);
        auto* name = lab(QString::fromStdString(it->filename.empty() ? it->url : it->filename), barlow(13), P.text);
        name->setMinimumWidth(0);
        tcol->addWidget(name);
        auto* mrow = new QHBoxLayout; mrow->setSpacing(8);
        auto* meter = new LedMeter(16, 4, 6, 1); meter->setPalette(&P);
        auto* pct = lab("0%", mono(11), P.text2);
        auto* spd = lab("", mono(11), P.cyan);
        mrow->addWidget(meter); mrow->addWidget(pct); mrow->addStretch(1); mrow->addWidget(spd);
        tcol->addLayout(mrow);
        rl->addLayout(tcol, 1);
        auto* pbtn = new QPushButton; pbtn->setFixedSize(36, 36); pbtn->setCursor(Qt::PointingHandCursor);
        pbtn->setFocusPolicy(Qt::NoFocus);
        pbtn->setStyleSheet("QPushButton{background:" + hx(P.panelHi) + ";border:0;color:" + hx(P.text2) +
                            ";font-size:14px}QPushButton:hover{color:" + hx(P.text) + "}");
        rl->addWidget(pbtn);
        ll->addWidget(row);

        auto refreshRow = [=]() {
            auto d = itemById(id);
            if (!d) return;
            bool done = d->status == ST_COMPLETED, paused = d->status == ST_PAUSED;
            int p = d->percent() ? (int)(*d->percent() + 0.5) : (done ? 100 : 0);
            meter->set(p / 100.0, done ? P.green : paused ? P.violet : P.cyan);
            pct->setText(QString("%1%").arg(p));
            spd->setText(d->status == ST_DOWNLOADING ? human(d->speed) + "/s" : "");
            pbtn->setText(QString::fromUtf8(paused ? "\xE2\x96\xB6" : "\xE2\x8F\xB8"));  // ▶ resume / ⏸ pause
        };
        connect(pbtn, &QPushButton::clicked, this, [=] {
            auto d = itemById(id);
            if (!d) return;
            if (d->status == ST_PAUSED) manager_.resume(id); else manager_.pause(id);
            refresh();
            refreshRow();
        });
        refreshRow();
        updaters->push_back(refreshRow);
    }
    // "See all N in MPDM"
    auto* seeAll = new QPushButton(QString("See all %1 in MPDM").arg((int)items.size()));
    seeAll->setCursor(Qt::PointingHandCursor); seeAll->setFocusPolicy(Qt::NoFocus);
    seeAll->setStyleSheet("QPushButton{background:transparent;border:0;color:" + hx(P.amber) +
                          ";font-family:'Chakra Petch';font-weight:600;text-align:left;padding:6px}"
                          "QPushButton:hover{color:" + hx(P.text) + "}");
    connect(seeAll, &QPushButton::clicked, this, [this, panel] { panel->close(); showFromTray(); });
    ll->addWidget(seeAll, 0, Qt::AlignLeft);
    v->addWidget(listPanel);

    // ---- footer: Add / Pause all / Open ----
    auto* footer = new Surface(Surface::TitleBar); footer->setPalette(&P);
    auto* fl = new QHBoxLayout(footer); fl->setContentsMargins(6, 6, 6, 6); fl->setSpacing(6);
    auto* addB = new IndBtn("Add", true); addB->setPalette(&P);
    auto* pauseAllB = new IndBtn("Pause all"); pauseAllB->setPalette(&P);
    auto* openB = new IndBtn("Open"); openB->setPalette(&P);
    fl->addWidget(addB, 1); fl->addWidget(pauseAllB, 1); fl->addWidget(openB, 1);
    v->addWidget(footer);
    connect(addB, &QAbstractButton::clicked, this, [this, panel] { panel->close(); showFromTray(); onAdd(); });
    connect(pauseAllB, &QAbstractButton::clicked, this, [this] { manager_.pause_all(); refresh(); });
    connect(openB, &QAbstractButton::clicked, this, [this, panel] { panel->close(); showFromTray(); });

    // header + rows live-update while the panel is open
    auto headerUpd = [=] {
        int running = 0, queued = 0, paused = 0; double spd = 0;
        for (auto& it : manager_.ordered()) {
            if (it->status == ST_DOWNLOADING) { running++; spd += it->speed; }
            else if (it->status == ST_QUEUED) queued++;
            else if (it->status == ST_PAUSED) paused++;
        }
        hStatus->setText(QString("%1 RUNNING, %2 QUEUED, %3 PAUSED").arg(running).arg(queued).arg(paused));
        hSpeed->setText(QString::number(spd / 1048576.0, 'f', 1));
    };
    headerUpd();
    auto* t = new QTimer(panel);
    connect(t, &QTimer::timeout, panel, [headerUpd, updaters] {
        headerUpd();
        for (auto& f : *updaters) f();
    });
    t->start(400);

    // position above the tray icon (fall back to the screen corner)
    panel->adjustSize();
    int w = panel->width(), h = panel->sizeHint().height();
    QScreen* scr = QGuiApplication::primaryScreen();
    QRect av = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);
    QRect tg = tray_ ? tray_->geometry() : QRect();
    QPoint pos = tg.isEmpty() ? QPoint(av.right() - w - 12, av.bottom() - h - 12)
                              : QPoint(tg.center().x() - w / 2, tg.top() - h - 8);
    pos.setX(std::clamp(pos.x(), av.left() + 4, av.right() - w - 4));
    pos.setY(std::clamp(pos.y(), av.top() + 4, av.bottom() - h - 4));
    panel->move(pos);
    panel->show();
    panel->activateWindow();
    quickPanel_ = panel;
}

void MainWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;  // no tray -> close quits normally
    tray_ = new QSystemTrayIcon(Logo::icon(), this);
    tray_->setToolTip("MyPersonalDownloadManager");

    trayMenu_ = buildTrayMenu();
    tray_->setContextMenu(trayMenu_);

    // Left-click opens the quick panel; double-click restores the window.
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) showQuickPanel();
        else if (r == QSystemTrayIcon::DoubleClick) showFromTray();
    });
    tray_->show();
    // With a tray, the app lives on after the window is hidden; only the tray
    // "Exit MPDM" action (quitApp) ends it.
    qApp->setQuitOnLastWindowClosed(false);
}

void MainWindow::showFromTray() {
    showNormal();
    raise();
    activateWindow();
    trayDoneSeen_ = true;  // opening MPDM clears the "all done" badge
}

void MainWindow::showDownloadPopups() {
    // Open (or raise, if already open) the live popup for each current download so
    // the user can watch them from the tray. openPopupFor de-dupes and raises.
    int opened = 0;
    for (auto& it : manager_.ordered()) {
        if (it->status == ST_DOWNLOADING || it->status == ST_QUEUED || it->status == ST_PAUSED) {
            openPopupFor(it->id, it->stream);
            ++opened;
        }
    }
    if (opened == 0 && tray_)
        tray_->showMessage("MyPersonalDownloadManager", "No active downloads.",
                           Logo::icon(), 3000);
}

void MainWindow::quitApp() {
    allowQuit_ = true;
    if (tray_) tray_->hide();
    qApp->quit();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef _WIN32
    if (auto* msg = static_cast<MSG*>(message)) {
        // The installer's Restart Manager and a Windows shutdown/logoff both ask
        // us to close via WM_QUERYENDSESSION/WM_ENDSESSION (not the window's X).
        // Flip allowQuit_ so the pending close really quits instead of hiding to
        // the tray — otherwise a silent update hangs waiting for us to exit.
        if (msg->message == WM_QUERYENDSESSION || msg->message == WM_ENDSESSION) {
            allowQuit_ = true;
            if (tray_) tray_->hide();
        }
        // Frameless windows otherwise maximize over the taskbar; clamp the
        // maximized size/position to the screen's work area.
        if (msg->message == WM_GETMINMAXINFO) {
            if (QScreen* scr = screen()) {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(msg->lParam);
                const QRect av = scr->availableGeometry();
                const QRect sg = scr->geometry();
                const qreal dpr = scr->devicePixelRatio();
                mmi->ptMaxPosition.x = qRound((av.x() - sg.x()) * dpr);
                mmi->ptMaxPosition.y = qRound((av.y() - sg.y()) * dpr);
                mmi->ptMaxSize.x = qRound(av.width() * dpr);
                mmi->ptMaxSize.y = qRound(av.height() * dpr);
                mmi->ptMaxTrackSize = mmi->ptMaxSize;
                mmi->ptMinTrackSize.x = qRound(minimumWidth() * dpr);
                mmi->ptMinTrackSize.y = qRound(minimumHeight() * dpr);
                if (result) *result = 0;
                return true;
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::toggleMaximize() {
    if (isMaximized()) showNormal();
    else showMaximized();
}

void MainWindow::changeEvent(QEvent* ev) {
    if (ev->type() == QEvent::WindowStateChange && maxBtn_)
        maxBtn_->setText(QString::fromUtf8(isMaximized() ? "\xE2\x9D\x90" : "\xE2\x96\xA1"));  // ❐ / □
    QMainWindow::changeEvent(ev);
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    // With a tray present, the close button hides to tray instead of quitting
    // (IDM-style). Real exit goes through the tray "Exit" action / quitApp().
    if (tray_ && !allowQuit_) {
        hide();
        ev->ignore();
        if (!trayHintShown_) {
            trayHintShown_ = true;
            tray_->showMessage("MyPersonalDownloadManager",
                               "Still running in the background. Downloads continue; "
                               "right-click the tray icon to exit.",
                               Logo::icon(), 4000);
        }
        return;
    }
    QMainWindow::closeEvent(ev);
}

static void clearLayout(QLayout* l) {
    if (!l) return;
    while (QLayoutItem* it = l->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        if (it->layout()) clearLayout(it->layout());
        delete it;
    }
}

QColor stColor(const Palette& p, const std::string& st) {
    if (st == ST_DOWNLOADING) return p.cyan;
    if (st == ST_PAUSED) return p.violet;
    if (st == ST_COMPLETED) return p.green;
    if (st == ST_ERROR) return p.red;
    return p.queued;
}

void MainWindow::buildUi() {
    const Palette& P = *pal_;
    auto lab = [&](const QString& t, QFont f, QColor c) {
        auto* l = new QLabel(t);
        l->setFont(f);
        l->setStyleSheet("color:" + c.name(QColor::HexArgb) + ";background:transparent");
        return l;
    };
    auto label2 = [&](const QString& t, QColor c) {  // panel label (Chakra 11 uppercase)
        return lab(t.toUpper(), chakra(11, 600, 0.10), c);
    };
    auto lampW = [&](QColor c, int d = 8) {
        auto* w = new QLabel;
        w->setFixedSize(d, d);
        w->setStyleSheet(QString("background:%1;border-radius:%2px").arg(c.name(QColor::HexArgb)).arg(d / 2.0));
        return w;
    };
    auto surf = [&](Surface::Kind k) { auto* s = themed(new Surface(k)); return s; };

    themed_.clear();  // rebuildable (theme switch re-runs buildUi)
    auto* central = new QWidget;
    central->setAutoFillBackground(true);
    // Frameless resize cursors are set on the outer frame; a Qt widget without an
    // explicit cursor inherits its parent's, so that resize cursor would otherwise
    // leak onto every interior widget. Pin the content root to an arrow so only the
    // 6px outer ring shows resize cursors (buttons/inputs keep their own).
    central->setCursor(Qt::ArrowCursor);
    QPalette cpal = central->palette();
    cpal.setColor(QPalette::Window, P.frame);
    central->setPalette(cpal);
    // Scroll-wrap the whole body: at roomy resolutions the viewport is larger
    // than the content so no scrollbars show and the center column expands to
    // fill (up to 1920); at small resolutions the content keeps its minimum and
    // the user scrolls instead of having panels clipped.
    auto* bodyScroll = new QScrollArea;
    bodyScroll->setWidgetResizable(true);
    bodyScroll->setFrameShape(QFrame::NoFrame);
    bodyScroll->setWidget(central);
    bodyScroll->setStyleSheet("QScrollArea{border:0;background:" + P.frame.name(QColor::HexArgb) + "}");
    bodyScroll->viewport()->setAutoFillBackground(true);
    { QPalette vp = bodyScroll->viewport()->palette(); vp.setColor(QPalette::Window, P.frame); bodyScroll->viewport()->setPalette(vp); }
    // Outer frame: a 6px ring around the scroll area that always sits at the window
    // edge (even when the body scrolls), so the frameless resize grips never move.
    auto* outerFrame = new QWidget;
    outerFrame->setAutoFillBackground(true);
    { QPalette op = outerFrame->palette(); op.setColor(QPalette::Window, P.frame); outerFrame->setPalette(op); }
    auto* ofl = new QVBoxLayout(outerFrame);
    ofl->setContentsMargins(6, 6, 6, 6);
    ofl->setSpacing(6);
    ofl->addWidget(bodyScroll);
    setCentralWidget(outerFrame);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    // ---- title bar (frameless: also carries the window controls) ----
    auto* title = surf(Surface::TitleBar);
    title->setFixedHeight(44);
    auto* tl = new QHBoxLayout(title);
    tl->setContentsMargins(12, 0, 6, 0);
    tl->setSpacing(10);
    // Branding is click-through so a press anywhere on the bar starts a window drag.
    auto* logo = new Logo(22); logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    tl->addWidget(logo);
    auto* nameL = lab("MyPersonalDownloadManager", chakra(15, 600, 0.0), P.text);
    nameL->setAttribute(Qt::WA_TransparentForMouseEvents);
    tl->addWidget(nameL);
    auto* ctrlL = label2("Download control", P.text3);
    ctrlL->setAttribute(Qt::WA_TransparentForMouseEvents);
    tl->addWidget(ctrlL);
    tl->addStretch(1);
    // minimize / maximize-restore / close
    auto winBtn = [&](const QString& glyph, bool danger) {
        auto* b = new QPushButton(glyph);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(42, 30);
        b->setFocusPolicy(Qt::NoFocus);
        QString hov = danger ? "QPushButton:hover{background:#D0452F;color:#ffffff}"
                             : "QPushButton:hover{background:" + P.panelHi.name(QColor::HexArgb) +
                                   ";color:" + P.text.name(QColor::HexArgb) + "}";
        b->setStyleSheet("QPushButton{background:transparent;border:0;color:" +
                         P.text3.name(QColor::HexArgb) + ";font-size:13px}" + hov);
        return b;
    };
    auto* minBtn = winBtn(QString::fromUtf8("\xE2\x80\x95"), false);   // ―
    minBtn->setToolTip("Minimize");
    maxBtn_ = winBtn(QString::fromUtf8(isMaximized() ? "\xE2\x9D\x90" : "\xE2\x96\xA1"), false);  // ❐ / □
    maxBtn_->setToolTip("Maximize");
    auto* clsBtn = winBtn(QString::fromUtf8("\xE2\x9C\x95"), true);    // ✕
    clsBtn->setToolTip("Close (keeps running in the tray)");
    connect(minBtn, &QPushButton::clicked, this, [this] { showMinimized(); });
    connect(maxBtn_, &QPushButton::clicked, this, [this] { toggleMaximize(); });
    connect(clsBtn, &QPushButton::clicked, this, [this] { close(); });

    // Help: a "?" in a ring, left of the window controls (separated by a divider).
    auto* helpBtn = winBtn("", false);
    {
        const int sz = 20;
        qreal dpr = devicePixelRatioF();
        QPixmap pm(QSize(sz, sz) * dpr); pm.setDevicePixelRatio(dpr); pm.fill(Qt::transparent);
        QPainter g(&pm);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setPen(QPen(P.text2, 1.6));
        g.drawEllipse(QRectF(1.5, 1.5, sz - 3, sz - 3));
        QFont f = chakra(12, 600, 0.0); g.setFont(f);
        g.drawText(QRectF(0, 0, sz, sz), Qt::AlignCenter, "?");
        g.end();
        helpBtn->setIcon(QIcon(pm));
        helpBtn->setIconSize(QSize(sz, sz));
    }
    helpBtn->setToolTip("Help — what MPDM is, connecting a browser, settings");
    connect(helpBtn, &QPushButton::clicked, this, [this] { openHelp(); });
    auto* helpSep = new QWidget; helpSep->setFixedSize(1, 18);
    helpSep->setStyleSheet("background:" + P.bevelLight.name());
    tl->addWidget(helpBtn); tl->addSpacing(4); tl->addWidget(helpSep); tl->addSpacing(4);
    tl->addWidget(minBtn); tl->addWidget(maxBtn_); tl->addWidget(clsBtn);
    // The title bar sits in the outer frame (above the scroll area), so the window
    // controls stay full-width and reachable even when the body scrolls at small
    // window sizes. Let the long name shrink so the controls never get pushed off.
    // (Preferred = full width when there's room; minimum 0 = may shrink when not.)
    nameL->setMinimumWidth(0);
    nameL->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    ctrlL->setMinimumWidth(0);
    ctrlL->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    ofl->insertWidget(0, title);

    // Frameless move/resize: drag the title bar; resize from the outer frame's
    // 6px ring (always at the window edge, even while the body scrolls).
    new FramelessChrome(this, title, outerFrame, [this] { toggleMaximize(); });

    // ---- readout tiles ----
    auto* tiles = new QHBoxLayout;
    tiles->setSpacing(6);
    auto tile = [&](const QString& name, QColor valColor, QWidget* rightWidget, const QString& valId,
                    const QString& unit, const QString& rightId) {
        auto* s = surf(Surface::Panel);
        auto* v = new QVBoxLayout(s);
        v->setContentsMargins(12, 9, 12, 11);
        v->setSpacing(6);
        v->addWidget(label2(name, P.text3));
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* val = lab("0", mono(28, 500), valColor);
        val->setObjectName(valId);
        row->addWidget(val, 0, Qt::AlignBottom);
        if (!unit.isEmpty()) {
            auto* u = lab(unit, mono(13, 400), P.text2);
            u->setObjectName(valId + "_u");
            row->addWidget(u, 0, Qt::AlignBottom);
        }
        row->addStretch(1);
        if (rightWidget) { if (!rightId.isEmpty()) rightWidget->setObjectName(rightId); row->addWidget(rightWidget, 0, Qt::AlignBottom); }
        v->addLayout(row);
        tiles->addWidget(s, 1);
    };
    speedMeter_ = themed(new LedMeter(16, 4, 14, 2));
    tile("Total speed", P.cyan, speedMeter_, "s_speed", "MB/s", "");
    slotLamps_ = new QWidget;
    { auto* h = new QHBoxLayout(slotLamps_); h->setContentsMargins(0,0,0,0); h->setSpacing(5); }
    tile("Active slots", P.text, slotLamps_, "s_slots", "running", "");
    tile("Downloaded today", P.text, lab("0 files", mono(11, 400), P.text3), "s_today", "GB", "s_files");
    tile("Scheduler", P.amber, nullptr, "s_sched", "", "");
    root->addLayout(tiles);

    // ---- toolbar ----
    auto* toolbar = surf(Surface::Panel);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(6, 6, 6, 6);
    tb->setSpacing(6);
    auto divider = [&]() { auto* d = new QFrame; d->setFixedSize(2, 26); d->setStyleSheet("background:" + P.well.name() + ";"); return d; };
    auto* addBtn = themed(new IndBtn("Add", true));
    connect(addBtn, &QAbstractButton::clicked, this, &MainWindow::onAdd);
    tb->addWidget(addBtn);
    tb->addWidget(divider());
    auto actionBtn = [&](const QString& t, std::function<void()> fn) {
        auto* b = themed(new IndBtn(t));
        connect(b, &QAbstractButton::clicked, this, fn);
        tb->addWidget(b);
        return b;
    };
    actionBtn("Resume", [this] { if (!selId_.isEmpty()) { manager_.resume(selId_.toStdString()); refresh(); } });
    actionBtn("Pause", [this] { if (!selId_.isEmpty()) { manager_.pause(selId_.toStdString()); refresh(); } });
    actionBtn("Pause all", [this] { manager_.pause_all(); refresh(); });
    actionBtn("Remove", [this] { onRemove(); });
    actionBtn("Clear done", [this] { onClearDone(); });
    tb->addWidget(divider());
    actionBtn("Scheduler", [this] { openScheduler(); });
    actionBtn("Settings", [this] { openSettings(); });
    tb->addStretch(1);
    auto* searchWell = surf(Surface::Well);
    searchWell->setFixedSize(220, 44);
    auto* sh = new QHBoxLayout(searchWell);
    sh->setContentsMargins(10, 0, 10, 0);
    auto* search = new QLineEdit;
    search->setObjectName("search");
    search->setPlaceholderText("Search downloads");
    search->setFont(barlow(13));
    sh->addWidget(search);
    tb->addWidget(searchWell);
    tb->addWidget(label2("Limit", P.text3));
    auto* lim = new QComboBox;
    lim->setView(new QListView(lim));
    lim->addItems({"UNLIMITED", "500 KB/s", "1 MB/s", "2 MB/s", "5 MB/s"});
    lim->setFont(mono(11));
    lim->setStyleSheet(QString("QComboBox{background:%1;color:%2;border:0;padding:0 10px;min-height:44px}")
                           .arg(P.well.name(QColor::HexArgb), P.text.name(QColor::HexArgb)));
    connect(lim, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const char* r[] = {"", "500k", "1m", "2m", "5m"};
        manager_.set_global_rate((double)parse_rate(r[i]));
    });
    tb->addWidget(lim);
    root->addWidget(toolbar);

    // ---- main three columns ----
    auto* main = new QHBoxLayout;
    main->setSpacing(6);

    // left
    auto* aside = new QVBoxLayout;
    aside->setSpacing(6);
    auto* catPanel = surf(Surface::Panel);
    auto* cv = new QVBoxLayout(catPanel);
    cv->setContentsMargins(12, 10, 12, 12);
    cv->setSpacing(8);
    cv->addWidget(label2("Categories", P.text3));
    catsLay_ = nullptr;  // (was a tile grid; replaced by a compact dropdown)
    auto* catCombo = new QComboBox;
    catCombo->setView(new QListView(catCombo));
    catCombo->addItems({"All", "Video", "Audio", "Docs", "Archive", "Apps", "Other", "Private"});
    catCombo->setCurrentText(catFilter_);
    catCombo->setStyleSheet(
        QString("QComboBox{background:%1;color:%2;border:1px solid %3;padding:5px 10px;min-height:30px}"
                "QComboBox::drop-down{border:0;width:18px}"
                "QComboBox::down-arrow{image:none;border-left:4px solid transparent;"
                "border-right:4px solid transparent;border-top:5px solid %4;margin-right:7px}")
            .arg(P.well.name(QColor::HexArgb), P.text.name(QColor::HexArgb),
                 (P.dark ? P.bevelLight : P.bevelDark).name(QColor::HexArgb), P.text2.name(QColor::HexArgb)));
    connect(catCombo, &QComboBox::currentTextChanged, this, [this](const QString& t) { catFilter_ = t; refresh(); });
    cv->addWidget(catCombo);
    aside->addWidget(catPanel);
    auto* statusPanel = surf(Surface::Panel);
    auto* sv = new QVBoxLayout(statusPanel);
    sv->setContentsMargins(12, 10, 12, 12);
    sv->setSpacing(2);
    sv->addWidget(label2("Status", P.text3));
    statusLay_ = new QVBoxLayout;
    statusLay_->setSpacing(0);
    sv->addLayout(statusLay_);
    sv->addStretch(1);
    auto* outWell = surf(Surface::Well);
    auto* ow = new QVBoxLayout(outWell);
    ow->setContentsMargins(10, 8, 10, 8);
    ow->setSpacing(3);
    ow->addWidget(label2("Output", P.text3));
    ow->addWidget(lab(QString::fromStdString(manager_.settings.output_dir), mono(11), P.text));
    ow->addWidget(lab("Sorted by category", barlow(12), P.text3));
    sv->addWidget(outWell);
    aside->addWidget(statusPanel, 1);
    auto* asideW = new QWidget;
    asideW->setLayout(aside);
    asideW->setFixedWidth(216);
    main->addWidget(asideW);

    // center
    auto* center = new QVBoxLayout;
    center->setSpacing(6);
    auto* thru = surf(Surface::Panel);
    auto* thv = new QVBoxLayout(thru);
    thv->setContentsMargins(12, 10, 12, 12);
    thv->setSpacing(8);
    auto* thh = new QHBoxLayout;
    thh->setSpacing(10);
    thh->addWidget(label2("Throughput", P.text3));
    auto* now = lab("NET 0", mono(11), P.cyan); now->setObjectName("t_now"); thh->addWidget(now);
    auto* dsk = lab("DISK 0", mono(11), P.amber); dsk->setObjectName("t_disk"); thh->addWidget(dsk);
    auto* pk = lab("PEAK 0", mono(11), P.text3); pk->setObjectName("t_peak"); thh->addWidget(pk);
    auto* du = lab("", mono(11), P.text3); du->setObjectName("t_diskfree"); thh->addWidget(du);
    thh->addStretch(1);
    thv->addLayout(thh);
    graph_ = themed(new Graph);
    thv->addWidget(graph_);
    center->addWidget(thru);

    auto* queue = surf(Surface::Panel);
    auto* qv = new QVBoxLayout(queue);
    qv->setContentsMargins(0, 10, 0, 0);
    qv->setSpacing(6);
    auto* qh = new QHBoxLayout;
    qh->setContentsMargins(12, 0, 12, 0);
    qh->addWidget(label2("Queue", P.text3));
    auto* qc = lab("0 ITEMS", mono(11), P.text3); qc->setObjectName("qcount"); qh->addWidget(qc);
    qh->addStretch(1);
    qh->addWidget(lab("Newest first", barlow(12), P.text2));
    qv->addLayout(qh);
    // column header
    auto* colh = new QWidget;
    colh->setStyleSheet("background:" + P.well.name(QColor::HexArgb));
    auto* ch = new QHBoxLayout(colh);
    ch->setContentsMargins(10, 5, 10, 5);
    ch->setSpacing(12);
    auto colLbl = [&](const QString& t, int w) {
        auto* l = lab(t.toUpper(), chakra(11, 600, 0.10), P.text3);
        if (w) l->setFixedWidth(w);
        return l;
    };
    { auto* sp = new QWidget; sp->setFixedWidth(28); ch->addWidget(sp); }
    ch->addWidget(colLbl("Name", 0), 1);
    ch->addWidget(colLbl("Size", 64));
    ch->addWidget(colLbl("Progress", 140));
    ch->addWidget(colLbl("Speed", 70));
    ch->addWidget(colLbl("State", 96));
    qv->addWidget(colh);
    // rows scroll
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("background:transparent");
    auto* rowsHost = new QWidget;
    rowsHost->setStyleSheet("background:transparent");
    rowsLay_ = new QVBoxLayout(rowsHost);
    rowsLay_->setContentsMargins(0, 0, 0, 0);
    rowsLay_->setSpacing(0);
    rowsLay_->addStretch(1);
    scroll->setWidget(rowsHost);
    qv->addWidget(scroll, 1);
    center->addWidget(queue, 1);
    main->addLayout(center, 1);

    // right (selected)
    auto* rightPanel = surf(Surface::Panel);
    rightPanel->setFixedWidth(306);
    auto* rv = new QVBoxLayout(rightPanel);
    rv->setContentsMargins(12, 10, 12, 12);
    rv->setSpacing(10);
    auto* rh = new QHBoxLayout;
    rh->addWidget(label2("Selected", P.text3));
    rh->addStretch(1);
    auto* selState = lab("", barlow(12), P.text2); selState->setObjectName("sel_state"); rh->addWidget(selState);
    rv->addLayout(rh);
    auto* selHead = new QHBoxLayout; selHead->setSpacing(10);
    auto* selIcon = new QLabel; selIcon->setObjectName("sel_icon"); selIcon->setFixedSize(40, 40);
    selIcon->setAlignment(Qt::AlignCenter);
    selIcon->setStyleSheet("background:" + P.well.name(QColor::HexArgb));
    selIcon->hide();
    selHead->addWidget(selIcon, 0, Qt::AlignTop);
    auto* selTexts = new QVBoxLayout; selTexts->setSpacing(2);
    auto* selName = lab("No selection", barlow(14, 500), P.text); selName->setObjectName("sel_name"); selTexts->addWidget(selName);
    auto* selHost = lab("", mono(11), P.text3); selHost->setObjectName("sel_host"); selTexts->addWidget(selHost);
    selName->setMinimumWidth(0); selHost->setMinimumWidth(0);
    selHead->addLayout(selTexts, 1);
    rv->addLayout(selHead);
    auto* pctRow = new QHBoxLayout;
    auto* selPct = lab("—", mono(28, 500), P.text); selPct->setObjectName("sel_pct"); pctRow->addWidget(selPct, 0, Qt::AlignBottom);
    pctRow->addStretch(1);
    auto* selAmt = lab("", mono(11), P.text2); selAmt->setObjectName("sel_amt"); pctRow->addWidget(selAmt, 0, Qt::AlignBottom);
    rv->addLayout(pctRow);
    selMeter_ = themed(new LedMeter(46, 0, 10, 1));
    rv->addWidget(selMeter_);
    rv->addWidget(label2("Connections", P.text3));
    lanes_ = themed(new Lanes);
    rv->addWidget(lanes_);
    auto* note = lab("", barlow(12), P.text2);
    note->setObjectName("sel_note");
    note->setWordWrap(true);
    note->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent;border-left:2px solid " + P.amber.name() + ";padding-left:8px");
    note->hide();
    rv->addWidget(note);
    auto* kvWell = surf(Surface::Well);
    auto* kv = new QVBoxLayout(kvWell);
    kv->setContentsMargins(10, 10, 10, 10);
    kv->setSpacing(7);
    for (auto k : {"Speed", "Time left", "Limit", "Resume", "Save to"}) {
        auto* r = new QHBoxLayout;
        r->addWidget(lab(QString(k).toUpper(), chakra(11, 600, 0.08), P.text3));
        r->addStretch(1);
        auto* vv = lab("—", mono(11), P.text);
        vv->setObjectName(QString("kv_") + QString(k).toLower().replace(' ', '_'));
        r->addWidget(vv);
        kv->addLayout(r);
    }
    rv->addWidget(kvWell);
    rv->addStretch(1);
    auto* selBtns = new QHBoxLayout;
    selBtns->setSpacing(6);
    auto* selPause = themed(new IndBtn("Pause"));
    connect(selPause, &QAbstractButton::clicked, this, [this] {
        if (selId_.isEmpty()) return;
        auto items = manager_.ordered();
        for (auto& it : items) if (QString::fromStdString(it->id) == selId_) {
            if (it->status == ST_PAUSED) manager_.resume(selId_.toStdString()); else manager_.pause(selId_.toStdString());
        }
        refresh();
    });
    selBtns->addWidget(selPause, 1);
    auto* selOpen = themed(new IndBtn("Open folder"));
    connect(selOpen, &QAbstractButton::clicked, this, [this] {
        // The selected file's folder; with nothing selected, the download folder.
        QString dir = QString::fromStdString(manager_.settings.output_dir);
        for (auto& it : manager_.ordered())
            if (QString::fromStdString(it->id) == selId_ && !it->dest_path.empty())
                dir = QString::fromStdString(std::filesystem::path(it->dest_path).parent_path().string());
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    selBtns->addWidget(selOpen, 1);
    rv->addLayout(selBtns);
    main->addWidget(rightPanel);

    root->addLayout(main, 1);

    // ---- status bar ----
    auto* sb = new QHBoxLayout;
    sb->setContentsMargins(8, 0, 8, 0);
    sb->setSpacing(18);
    auto sbItem = [&](QColor c, const QString& id) {
        auto* h = new QHBoxLayout;
        h->setSpacing(7);
        h->addWidget(lampW(c, 7));
        auto* t = lab("", chakra(11, 600, 0.08), P.text3);
        t->setObjectName(id);
        h->addWidget(t);
        sb->addLayout(h);
    };
    sbItem(P.green, "sb_net");
    sbItem(P.cyan, "sb_slots");
    sbItem(P.amber, "sb_sched");
    sb->addStretch(1);
    sb->addWidget(lab("PROXY OFF", chakra(11, 600, 0.08), P.text3));
    auto* sbW = new QWidget;
    sbW->setLayout(sb);
    sbW->setFixedHeight(24);
    root->addWidget(sbW);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick) {
        if (auto* w = qobject_cast<QWidget*>(obj)) {
            QVariant id = w->property("mpdmId");
            if (id.isValid()) {
                selId_ = id.toString();
                // Double-click (re)opens the item's live popup — the restore path
                // for a popup the user minimized to the tray.
                if (ev->type() == QEvent::MouseButtonDblClick) {
                    bool stream = false;
                    for (auto& it : manager_.ordered())
                        if (QString::fromStdString(it->id) == selId_) { stream = it->stream; break; }
                    openPopupFor(selId_.toStdString(), stream);
                }
                refresh();
                return false;
            }
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::applyTheme() {
    qApp->setStyleSheet(baseStylesheet(*pal_));
    for (auto& f : themed_) f(pal_);
}

void MainWindow::onAdd() {
    bool ok = false;
    QString t = QInputDialog::getMultiLineText(this, "Add downloads",
        "One link per line (ranges like [1-100] expand):", "", &ok);
    if (ok && !t.trimmed().isEmpty()) {
        bool autostart = QSettings().value("autostart", true).toBool();
        auto added = manager_.add_batch(t.toStdString(), 0, autostart);
        refresh();
        // Open the same live, app-owned download popup the browser extension uses,
        // so both entry points behave identically (one in-app popup per download,
        // never a separate running app). A pasted wildcard batch (e.g. [1-100])
        // would flood the screen, so only auto-open for a handful; the rest stay
        // visible in the queue. The tray's "Show download popups" toggle gates this.
        if (QSettings().value("show_popups", true).toBool() && added.size() <= 8)
            for (auto& it : added)
                if (it) openPopupFor(it->id, it->stream);
    }
}

void MainWindow::onClearDone() {
    manager_.clear_finished();
    // Drop announce-state for items that are gone so the ids can be reused cleanly.
    std::set<std::string> live;
    for (auto& it : manager_.ordered()) live.insert(it->id);
    for (auto i = finishAnnounced_.begin(); i != finishAnnounced_.end();)
        i = live.count(*i) ? std::next(i) : finishAnnounced_.erase(i);
    refresh();
}

void MainWindow::refresh() {
    const Palette& P = *pal_;
    auto items = manager_.ordered();
    auto setText = [&](const char* id, const QString& t) { if (auto* l = findChild<QLabel*>(id)) l->setText(t); };

    double g = 0; int active = 0; long long dl = 0; int fin = 0;
    int paused = 0, queued = 0, failed = 0; long long dsum = 0, ssum = 0;
    for (auto& it : items) {
        if (it->status == ST_DOWNLOADING) {
            g += it->speed; active++;
            dsum += it->downloaded; if (it->size) ssum += *it->size;
        } else if (it->status == ST_PAUSED) paused++;
        else if (it->status == ST_QUEUED) queued++;
        else if (it->status == ST_ERROR) failed++;
        dl += it->downloaded;
        if (it->status == ST_COMPLETED) fin++;
    }
    int mc = manager_.max_concurrent;

    bool newlyFinished = fin > lastFinished_;
    // Notify from the tray when a download finishes while minimized.
    if (tray_ && newlyFinished && isHidden()) {
        int done = fin - lastFinished_;
        // One file: name it and show its file-type icon (it isn't announced yet).
        std::shared_ptr<DownloadItem> just;
        if (done == 1)
            for (auto& it : items)
                if (it->status == ST_COMPLETED && !finishAnnounced_.count(it->id)) { just = it; break; }
        if (just)
            tray_->showMessage("Download complete",
                               QString::fromStdString(just->filename.empty() ? just->url : just->filename),
                               QIcon(itemIcon(*just, 48, P)), 4000);
        else
            tray_->showMessage("Download complete",
                               done == 1 ? QString("1 download finished.")
                                         : QString("%1 downloads finished.").arg(done),
                               Logo::icon(), 4000);
    }
    lastFinished_ = fin;

    // Tray icon reflects overall state. A new completion re-arms the green "all
    // done" badge; opening the window clears it.
    if (tray_) {
        if (!isHidden()) trayDoneSeen_ = true;
        if (newlyFinished) trayDoneSeen_ = false;
        int st = active > 0 ? TrayDownloading
               : failed > 0 ? TrayFailed
               : paused > 0 ? TrayPaused
               : (fin > 0 && queued == 0 && !trayDoneSeen_) ? TrayDone
               : TrayIdle;
        double frac = ssum > 0 ? (double)dsum / (double)ssum : 0.0;
        double nowt = mono_seconds();
        bool stateChanged = st != trayState_;
        bool fracChanged = st == TrayDownloading && std::abs(frac - trayFrac_) >= 0.01;
        if (stateChanged || (fracChanged && nowt - trayIconAt_ >= 1.0)) {
            trayState_ = st; trayFrac_ = frac; trayIconAt_ = nowt;
            tray_->setIcon(makeTrayIcon(st, frac));
        }
    }

    // IDM-style "download complete" dialog for each newly-finished item (opt-out
    // via the dialog's checkbox / Settings). Seeded in the ctor so it never fires
    // for downloads already finished when the app opened.
    if (QSettings().value("finish_popup", true).toBool()) {
        for (auto& it : items) {
            if (it->status != ST_COMPLETED || finishAnnounced_.count(it->id)) continue;
            finishAnnounced_.insert(it->id);
            // The live popup (if any) is replaced by the completion dialog.
            for (auto* p : popups_) if (p && p->itemId() == it->id) p->close();
            showFinishDialog(it);
        }
    } else {
        for (auto& it : items) if (it->status == ST_COMPLETED) finishAnnounced_.insert(it->id);
    }

    setText("s_speed", QString::number(g / 1048576.0, 'f', 1));
    setText("s_slots", QString("%1/%2").arg(active).arg(mc));
    { QString hd = human((double)dl); int sp = 0; while (sp < hd.size() && (hd[sp].isDigit() || hd[sp] == '.')) sp++;
      setText("s_today", hd.left(sp)); setText("s_today_u", hd.mid(sp)); }
    setText("s_files", QString("%1 files").arg(fin));
    setText("s_sched", "OFF");
    // Disk write rate = bytes committed to disk since the last tick.
    double nowT = mono_seconds();
    double diskRate = 0;
    if (lastDiskT_ > 0 && nowT > lastDiskT_) {
        long long delta = dl - lastDiskBytes_;
        if (delta > 0) diskRate = (double)delta / (nowT - lastDiskT_);
    }
    lastDiskBytes_ = dl; lastDiskT_ = nowT;
    if (active == 0) diskRate = 0;  // nothing active -> flat (keeps the graph idle)

    if (speedMeter_) speedMeter_->set(graph_ && graph_->peak() > 0 ? g / graph_->peak() : 0, P.cyan);
    if (graph_) {
        graph_->push(g, diskRate);
        setText("t_now", "NET " + human(g) + "/s");
        setText("t_disk", "DISK " + human(diskRate) + "/s");
        setText("t_peak", "PEAK " + human(graph_->peak()) + "/s");
    }
    // Free space on the download drive.
    {
        std::error_code ec;
        auto sp = std::filesystem::space(manager_.settings.output_dir, ec);
        if (!ec && sp.capacity > 0) {
            int usedPct = (int)((sp.capacity - sp.available) * 100 / sp.capacity);
            setText("t_diskfree", QString("· %1% used · %2 free").arg(usedPct).arg(human((double)sp.available)));
        } else {
            setText("t_diskfree", "");
        }
    }
    setText("sb_net", "NET " + human(g) + "/s");
    setText("sb_slots", QString("SLOTS %1/%2").arg(active).arg(mc));
    setText("sb_sched", "SCHED OFF");

    // slot lamps
    if (slotLamps_) {
        clearLayout(slotLamps_->layout());
        auto* h = static_cast<QHBoxLayout*>(slotLamps_->layout());
        for (int i = 0; i < mc; ++i) {
            auto* d = new QLabel; d->setFixedSize(7, 7);
            d->setStyleSheet(QString("background:%1;border-radius:3px").arg((i < active ? P.cyan : P.lampOff).name()));
            h->addWidget(d);
        }
    }

    // status list
    if (statusLay_) {
        clearLayout(statusLay_);
        struct S { const char* name; const char* st; QColor c; };
        S ss[] = {{"Downloading", ST_DOWNLOADING, P.cyan}, {"Queued", ST_QUEUED, P.queued},
                  {"Paused", ST_PAUSED, P.violet}, {"Finished", ST_COMPLETED, P.green}, {"Failed", ST_ERROR, P.red}};
        for (auto& s : ss) {
            int n = 0; for (auto& it : items) if (it->status == s.st) n++;
            auto* row = new QWidget; auto* h = new QHBoxLayout(row);
            h->setContentsMargins(4, 6, 4, 6); h->setSpacing(10);
            auto* d = new QLabel; d->setFixedSize(7, 7); d->setStyleSheet(QString("background:%1;border-radius:3px").arg(s.c.name()));
            h->addWidget(d);
            auto* nm = new QLabel(s.name); nm->setFont(barlow(13)); nm->setStyleSheet("color:" + P.text.name(QColor::HexArgb) + ";background:transparent");
            h->addWidget(nm, 1);
            auto* cnt = new QLabel(QString::number(n)); cnt->setFont(mono(11)); cnt->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent");
            h->addWidget(cnt);
            statusLay_->addWidget(row);
        }
    }

    // queue rows (filtered by the Categories dropdown)
    if (rowsLay_) {
        clearLayout(rowsLay_);
        std::vector<std::shared_ptr<DownloadItem>> shown;
        for (auto& it : items) {
            bool keep = true;
            if (catFilter_ == "All") keep = true;
            else if (catFilter_ == "Private") keep = it->incognito;
            else {
                std::string want = catFilter_ == "Docs"      ? "Documents"
                                   : catFilter_ == "Archive"  ? "Archives"
                                   : catFilter_ == "Apps"     ? "Programs"
                                                              : catFilter_.toStdString();
                keep = !it->incognito && it->category() == want;
            }
            if (keep) shown.push_back(it);
        }
        int idx = 0;
        std::shared_ptr<DownloadItem> sel;
        for (auto& it : shown) {
            bool isSel = QString::fromStdString(it->id) == selId_;
            if (isSel) sel = it;
            QColor bg = isSel ? P.rowSelected : (idx % 2 ? P.rowAlt : P.row);
            auto* row = new QWidget;
            row->setAutoFillBackground(true);
            QPalette rp = row->palette(); rp.setColor(QPalette::Window, bg); row->setPalette(rp);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(10, 7, 10, 7); h->setSpacing(12);
            if (isSel) { auto* edge = new QLabel; edge->setFixedWidth(2); edge->setStyleSheet("background:" + P.amber.name()); h->insertWidget(0, edge); h->setContentsMargins(8, 7, 10, 7); }
            auto* iconWell = new QLabel; iconWell->setFixedSize(28, 28);
            iconWell->setAlignment(Qt::AlignCenter);
            iconWell->setStyleSheet(QString("background:%1").arg(P.well.name(QColor::HexArgb)));
            iconWell->setPixmap(itemIcon(*it, 22, P));
            h->addWidget(iconWell);
            auto* nmBox = new QVBoxLayout; nmBox->setSpacing(2);
            auto* nm = new QLabel(QString::fromStdString(it->filename.empty() ? it->url : it->filename));
            nm->setFont(barlow(13)); nm->setStyleSheet("color:" + (isSel ? P.amberText : P.text).name(QColor::HexArgb) + ";background:transparent");
            nmBox->addWidget(nm);
            if (!it->error.empty()) { auto* e = new QLabel(QString::fromStdString(it->error)); e->setFont(barlow(12)); e->setStyleSheet("color:" + P.red.name() + ";background:transparent"); nmBox->addWidget(e); }
            h->addLayout(nmBox, 1);
            auto* sz = new QLabel(it->size ? human((double)*it->size) : "—"); sz->setFixedWidth(64); sz->setFont(mono(11)); sz->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent");
            h->addWidget(sz);
            auto* progWrap = new QWidget; progWrap->setFixedWidth(140); auto* pw = new QHBoxLayout(progWrap); pw->setContentsMargins(0,0,0,0); pw->setSpacing(6);
            auto* meter = new LedMeter(20, 4, 8, 1); meter->setPalette(pal_);
            double frac = it->percent() ? *it->percent() / 100.0 : 0.0;
            meter->set(frac, stColor(P, it->status)); pw->addWidget(meter);
            auto* pct = new QLabel(QString("%1%").arg(it->percent() ? (int)*it->percent() : 0)); pct->setFont(mono(11)); pct->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent"); pw->addWidget(pct);
            h->addWidget(progWrap);
            auto* spd = new QLabel(it->status == ST_DOWNLOADING && it->speed ? human(it->speed) + "/s" : "—"); spd->setFixedWidth(70); spd->setFont(mono(11));
            spd->setStyleSheet("color:" + (it->status == ST_DOWNLOADING ? P.cyan : P.text2).name(QColor::HexArgb) + ";background:transparent");
            h->addWidget(spd);
            auto* stWrap = new QWidget; stWrap->setFixedWidth(96); auto* sw = new QHBoxLayout(stWrap); sw->setContentsMargins(0,0,0,0); sw->setSpacing(6);
            auto* lampd = new QLabel; lampd->setFixedSize(7, 7); lampd->setStyleSheet(QString("background:%1;border-radius:3px").arg(stColor(P, it->status).name()));
            sw->addWidget(lampd);
            auto* stl = new QLabel(QString::fromStdString(it->status)); stl->setFont(barlow(12)); stl->setStyleSheet("color:" + P.text2.name(QColor::HexArgb) + ";background:transparent");
            sw->addWidget(stl); sw->addStretch(1);
            h->addWidget(stWrap);
            std::string id = it->id;
            row->installEventFilter(this);
            row->setProperty("mpdmId", QString::fromStdString(id));
            rowsLay_->addWidget(row);
            idx++;
        }
        rowsLay_->addStretch(1);
        setText("qcount", QString("%1 ITEMS").arg(shown.size()));

        // selected panel
        // No auto-pick: a removed (or never-made) selection leaves the panel empty
        // until the user clicks a row.
        if (sel) {
            setText("sel_name", QString::fromStdString(sel->filename.empty() ? sel->url : sel->filename));
            if (auto* ic = findChild<QLabel*>("sel_icon")) { ic->setPixmap(itemIcon(*sel, 32, P)); ic->show(); }
            setText("sel_host", QString::fromStdString(sel->url));
            int pct = sel->percent() ? (int)*sel->percent() : 0;
            setText("sel_pct", QString("%1%").arg(pct));
            setText("sel_amt", human((double)sel->downloaded) + " / " + human(sel->size ? (double)*sel->size : 0));
            if (selMeter_) selMeter_->set(pct / 100.0, stColor(P, sel->status));
            if (lanes_) lanes_->setData(sel->conn_stats);
            if (auto* st = findChild<QLabel*>("sel_state")) st->setText(QString::fromStdString(sel->status));
            if (auto* nt = findChild<QLabel*>("sel_note")) {
                const SegmentProgress* stolen = nullptr;
                for (auto& c : sel->conn_stats) if (c.stole_from >= 0) { stolen = &c; break; }
                if (stolen) { nt->setText(QString("C%1 finished its own range early and took over part of C%2's remainder.").arg(stolen->index + 1).arg(stolen->stole_from + 1)); nt->show(); }
                else nt->hide();
            }
            setText("kv_speed", sel->status == ST_DOWNLOADING ? human(sel->speed) + "/s" : "—");
            setText("kv_time_left", sel->status == ST_DOWNLOADING && sel->eta ? htime(*sel->eta) : "—");
            setText("kv_limit", sel->max_rate_bps ? human((double)sel->max_rate_bps) + "/s" : "Unlimited");
            setText("kv_resume", sel->status == ST_COMPLETED ? "Done" : "Saved");
            setText("kv_save_to", QString::fromStdString(sel->dest_path.empty() ? "—" : sel->dest_path));
        } else {
            // Nothing to select (queue empty / item removed): back to the idle state
            // instead of leaving the last item's details on screen.
            selId_.clear();
            setText("sel_name", "No selection");
            if (auto* ic = findChild<QLabel*>("sel_icon")) { ic->clear(); ic->hide(); }
            setText("sel_host", "");
            setText("sel_pct", "—");
            setText("sel_amt", "");
            if (auto* st = findChild<QLabel*>("sel_state")) st->setText("");
            if (auto* nt = findChild<QLabel*>("sel_note")) nt->hide();
            if (selMeter_) selMeter_->set(0.0, P.cyan);
            if (lanes_) lanes_->setData({});
            for (const char* k : {"kv_speed", "kv_time_left", "kv_limit", "kv_resume", "kv_save_to"}) setText(k, "—");
        }
    }
}

} // namespace mpdm::gui
