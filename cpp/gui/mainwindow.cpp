#include "mainwindow.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStyleHints>
#include <QSystemTrayIcon>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

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

const Palette* paletteForMode(const QString& mode) {
    if (mode == "light") return &kLight;
    if (mode == "dark") return &kDark;
    // system
    return qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light ? &kLight : &kDark;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("MyPersonalDownloadManager");
    QSettings s;
    pal_ = paletteForMode(s.value("appearance", "dark").toString());
    setWindowIcon(Logo::icon());

    // Resolution-friendly sizing: the full layout targets ~1300x860, but it must
    // also fit small screens without clipping. Open clamped to the available
    // screen area and centered; a low minimum size plus the scroll-wrapped body
    // (see buildUi) keep it usable from 800x600 up to 1920x1080 — below the
    // layout's natural size the content scrolls instead of being cut off.
    setMinimumSize(720, 500);
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
    buildUi();
    applyTheme();
    setupTray();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_.start(1000);
    refresh();
}

void MainWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;  // no tray -> close quits normally
    tray_ = new QSystemTrayIcon(Logo::icon(), this);
    tray_->setToolTip("MyPersonalDownloadManager");

    auto* menu = new QMenu(this);
    menu->addAction("Open MPDM", this, [this] { showFromTray(); });
    menu->addSeparator();
    menu->addAction("Resume all", this, [this] { manager_.resume_all(); refresh(); });
    menu->addAction("Pause all", this, [this] { manager_.pause_all(); refresh(); });
    menu->addSeparator();
    menu->addAction("Exit", this, [this] { quitApp(); });
    tray_->setContextMenu(menu);

    // Left-click (trigger) or double-click restores the window.
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) showFromTray();
    });
    tray_->show();
    // With a tray, the app lives on after the window is hidden; only the tray
    // "Exit" action (quitApp) ends it.
    qApp->setQuitOnLastWindowClosed(false);
}

void MainWindow::showFromTray() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitApp() {
    allowQuit_ = true;
    if (tray_) tray_->hide();
    qApp->quit();
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
    setCentralWidget(bodyScroll);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // ---- title bar ----
    auto* title = surf(Surface::TitleBar);
    title->setFixedHeight(44);
    auto* tl = new QHBoxLayout(title);
    tl->setContentsMargins(12, 0, 6, 0);
    tl->setSpacing(10);
    tl->addWidget(new Logo(22));
    tl->addWidget(lab("MyPersonalDownloadManager", chakra(15, 600, 0.0), P.text));
    tl->addWidget(label2("Download control", P.text3));
    tl->addStretch(1);
    root->addWidget(title);

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
    actionBtn("Stop", [this] { if (!selId_.isEmpty()) { manager_.pause(selId_.toStdString()); refresh(); } });
    actionBtn("Remove", [this] { if (!selId_.isEmpty()) { manager_.remove(selId_.toStdString(), false); refresh(); } });
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
    catsLay_ = new QGridLayout;
    catsLay_->setSpacing(6);
    cv->addLayout(catsLay_);
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
    auto* now = lab("NOW 0", mono(11), P.cyan); now->setObjectName("t_now"); thh->addWidget(now);
    auto* pk = lab("PEAK 0", mono(11), P.text3); pk->setObjectName("t_peak"); thh->addWidget(pk);
    thh->addStretch(1);
    for (auto r : {"1M", "10M", "1H", "10H"}) {
        auto* b = lab(r, mono(11), QString(r) == "10M" ? P.text : P.text3);
        thh->addWidget(b);
    }
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
    auto* selName = lab("No selection", barlow(14, 500), P.text); selName->setObjectName("sel_name"); rv->addWidget(selName);
    auto* selHost = lab("", mono(11), P.text3); selHost->setObjectName("sel_host"); rv->addWidget(selHost);
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
    if (ev->type() == QEvent::MouseButtonPress) {
        if (auto* w = qobject_cast<QWidget*>(obj)) {
            QVariant id = w->property("mpdmId");
            if (id.isValid()) { selId_ = id.toString(); refresh(); return false; }
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
    if (ok && !t.trimmed().isEmpty()) { manager_.add_batch(t.toStdString()); refresh(); }
}

void MainWindow::refresh() {
    const Palette& P = *pal_;
    // Browser downloads: persist completed ones into the store, and merge the
    // in-progress ones (written by each popup to active/) into the list.
    manager_.ingest_history_inbox();
    auto items = manager_.ordered();
    {
        std::set<std::string> have;
        for (auto& it : items) have.insert(it->id);
        for (auto& a : read_active_browser()) {
            if (a.id.empty() || have.count(a.id)) continue;
            items.push_back(std::make_shared<DownloadItem>(std::move(a)));
        }
    }
    auto setText = [&](const char* id, const QString& t) { if (auto* l = findChild<QLabel*>(id)) l->setText(t); };

    double g = 0; int active = 0; long long dl = 0; int fin = 0;
    for (auto& it : items) { if (it->status == ST_DOWNLOADING) { g += it->speed; active++; } dl += it->downloaded; if (it->status == ST_COMPLETED) fin++; }
    int mc = manager_.max_concurrent;

    // Notify from the tray when a download finishes while minimized.
    if (tray_ && fin > lastFinished_ && isHidden()) {
        int done = fin - lastFinished_;
        tray_->showMessage("Download complete",
                           done == 1 ? QString("1 download finished.")
                                     : QString("%1 downloads finished.").arg(done),
                           Logo::icon(), 4000);
    }
    lastFinished_ = fin;
    setText("s_speed", QString::number(g / 1048576.0, 'f', 1));
    setText("s_slots", QString("%1/%2").arg(active).arg(mc));
    { QString hd = human((double)dl); int sp = 0; while (sp < hd.size() && (hd[sp].isDigit() || hd[sp] == '.')) sp++;
      setText("s_today", hd.left(sp)); setText("s_today_u", hd.mid(sp)); }
    setText("s_files", QString("%1 files").arg(fin));
    setText("s_sched", "OFF");
    if (speedMeter_) speedMeter_->set(graph_ && graph_->peak() > 0 ? g / graph_->peak() : 0, P.cyan);
    if (graph_) { graph_->push(g); setText("t_now", "NOW " + human(g) + "/s"); setText("t_peak", "PEAK " + human(graph_->peak()) + "/s"); }
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

    // categories
    if (catsLay_) {
        clearLayout(catsLay_);
        std::map<std::string, int> cc;
        for (auto& it : items) cc[it->category()]++;
        struct C { const char* name; int n; };
        C cats[] = {{"All", (int)items.size()}, {"Video", cc["Video"]}, {"Audio", cc["Audio"]},
                    {"Docs", cc["Documents"]}, {"Archive", cc["Archives"]}, {"Apps", cc["Programs"]},
                    {"Other", cc["Other"]}, {"Private", 0}};
        for (int i = 0; i < 8; ++i) {
            bool on = i == 0;
            auto* slot = new QLabel;
            slot->setFixedHeight(58);
            slot->setAlignment(Qt::AlignCenter);
            slot->setText(QString(cats[i].name).toUpper() + (cats[i].n ? QString("  %1").arg(cats[i].n) : ""));
            slot->setFont(chakra(10, 600, 0.08));
            slot->setStyleSheet(QString("background:%1;color:%2;border:1px solid %3")
                                    .arg(P.well.name(QColor::HexArgb),
                                         (on ? P.amber : P.text2).name(QColor::HexArgb),
                                         (on ? P.amber.name() : "transparent")));
            catsLay_->addWidget(slot, i / 3, i % 3);
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

    // queue rows
    if (rowsLay_) {
        clearLayout(rowsLay_);
        int idx = 0;
        std::shared_ptr<DownloadItem> sel;
        for (auto& it : items) {
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
            iconWell->setStyleSheet(QString("background:%1").arg(P.well.name(QColor::HexArgb)));
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
        setText("qcount", QString("%1 ITEMS").arg(items.size()));

        // selected panel
        if (!sel && !items.empty()) { sel = items.front(); selId_ = QString::fromStdString(sel->id); }
        if (sel) {
            setText("sel_name", QString::fromStdString(sel->filename.empty() ? sel->url : sel->filename));
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
        }
    }
}

} // namespace mpdm::gui
