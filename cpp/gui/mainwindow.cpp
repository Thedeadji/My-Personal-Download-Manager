#include "mainwindow.hpp"

#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QSplitter>
#include <QVBoxLayout>

#include <cmath>

#include "mpdm/categories.hpp"

namespace mpdm::gui {

static QString human(double n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
    return QString::number(n, 'f', i ? 1 : 0) + u[i];
}

static QColor statusColor(const Palette& p, const std::string& st) {
    if (st == ST_DOWNLOADING) return p.accent;
    if (st == ST_PAUSED) return p.purple;
    if (st == ST_COMPLETED) return p.green;
    if (st == ST_ERROR) return p.red;
    return p.faint;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("MyPersonalDownloadManager");
    resize(1280, 800);
    buildUi();
    applyTheme();
    connect(&timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_.start(1000);
    refresh();
}

QFrame* MainWindow::card(const QString& objName) {
    auto* f = new QFrame;
    f->setObjectName(objName);
    return f;
}

QWidget* MainWindow::statCard(const QString& title, QLabel*& valueOut) {
    auto* f = card();
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(8);
    auto* lbl = new QLabel(title);
    lbl->setObjectName("h");
    valueOut = new QLabel("—");
    valueOut->setObjectName("mono");
    QFont bf = valueOut->font();
    bf.setPointSize(20);
    bf.setBold(true);
    valueOut->setFont(bf);
    v->addWidget(lbl);
    v->addWidget(valueOut);
    return f;
}

QWidget* MainWindow::categorySidebar() {
    auto* side = new QWidget;
    auto* v = new QVBoxLayout(side);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* cat = card();
    auto* cg = new QGridLayout(cat);
    cg->setContentsMargins(12, 12, 12, 12);
    const char* tiles[] = {"ALL", "VIDEO", "AUDIO", "DOCS", "ARCHIVE", "APPS", "OTHER", "PRIVATE"};
    for (int i = 0; i < 8; ++i) {
        auto* t = new QFrame;
        t->setObjectName("card");
        auto* tv = new QVBoxLayout(t);
        tv->setContentsMargins(10, 10, 10, 10);
        auto* name = new QLabel(tiles[i]);
        name->setObjectName("h");
        tv->addWidget(name);
        cg->addWidget(t, i / 2, i % 2);
    }
    v->addWidget(cat);

    auto* status = card();
    auto* sv = new QVBoxLayout(status);
    sv->setContentsMargins(12, 12, 12, 12);
    auto* sh = new QLabel("STATUS");
    sh->setObjectName("h");
    sv->addWidget(sh);
    for (const char* s : {"Downloading", "Queued", "Paused", "Finished", "Failed"})
        sv->addWidget(new QLabel(s));
    v->addWidget(status);
    v->addStretch(1);
    return side;
}

void MainWindow::buildUi() {
    auto* central = new QWidget;
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // Stat cards
    auto* cards = new QHBoxLayout;
    cards->setSpacing(12);
    cards->addWidget(statCard("TOTAL SPEED", statSpeed_));
    cards->addWidget(statCard("ACTIVE SLOTS", statSlots_));
    cards->addWidget(statCard("DOWNLOADED TODAY", statToday_));
    cards->addWidget(statCard("SCHEDULER", statSched_));
    root->addLayout(cards);

    // Toolbar
    auto* tb = new QHBoxLayout;
    tb->setSpacing(8);
    auto* add = new QPushButton("+ ADD");
    add->setObjectName("primary");
    connect(add, &QPushButton::clicked, this, &MainWindow::onAdd);
    tb->addWidget(add);
    auto mkAction = [&](const QString& label, auto fn) {
        auto* b = new QPushButton(label);
        connect(b, &QPushButton::clicked, this, fn);
        tb->addWidget(b);
        return b;
    };
    mkAction("RESUME", [this] { if (auto id = selectedId(); !id.empty()) { manager_.resume(id); refresh(); } });
    mkAction("PAUSE", [this] { if (auto id = selectedId(); !id.empty()) { manager_.pause(id); refresh(); } });
    mkAction("STOP", [this] { if (auto id = selectedId(); !id.empty()) { manager_.pause(id); refresh(); } });
    mkAction("REMOVE", [this] { if (auto id = selectedId(); !id.empty()) { manager_.remove(id, false); refresh(); } });
    tb->addStretch(1);
    themeBtn_ = new QPushButton("THEME");
    connect(themeBtn_, &QPushButton::clicked, this, [this] {
        pal_ = pal_.dark ? lightPalette() : darkPalette();
        applyTheme();
        refresh();
    });
    tb->addWidget(themeBtn_);
    root->addLayout(tb);

    // Main splitter: sidebar | center | right
    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(categorySidebar());

    auto* center = new QWidget;
    auto* cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(12);
    auto* chartCard = card();
    auto* ccv = new QVBoxLayout(chartCard);
    ccv->setContentsMargins(14, 14, 14, 14);
    auto* ct = new QLabel("THROUGHPUT");
    ct->setObjectName("h");
    ccv->addWidget(ct);
    chart_ = new ThroughputChart;
    ccv->addWidget(chart_);
    cv->addWidget(chartCard);

    table_ = new QTableWidget(0, 5);
    table_->setHorizontalHeaderLabels({"NAME", "SIZE", "PROGRESS", "SPEED", "STATE"});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        int r = table_->currentRow();
        if (r >= 0 && table_->item(r, 0))
            selectedId_ = table_->item(r, 0)->data(Qt::UserRole).toString();
        refresh();
    });
    cv->addWidget(table_, 1);
    split->addWidget(center);

    // Right: selected panel
    auto* right = card();
    right->setMinimumWidth(300);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(14, 14, 14, 14);
    rv->setSpacing(10);
    auto* rh = new QLabel("SELECTED");
    rh->setObjectName("h");
    rv->addWidget(rh);
    selName_ = new QLabel("—");
    QFont nf = selName_->font(); nf.setBold(true); selName_->setFont(nf);
    selHost_ = new QLabel(""); selHost_->setObjectName("faint");
    rv->addWidget(selName_);
    rv->addWidget(selHost_);
    selPct_ = new QLabel("—"); selPct_->setObjectName("mono");
    QFont pf = selPct_->font(); pf.setPointSize(26); pf.setBold(true); selPct_->setFont(pf);
    rv->addWidget(selPct_);
    selBar_ = new SegmentBar; selBar_->setFixedHeight(10);
    rv->addWidget(selBar_);
    auto* ch = new QLabel("CONNECTIONS"); ch->setObjectName("h");
    rv->addWidget(ch);
    conns_ = new ConnectionsPanel;
    rv->addWidget(conns_);
    selState_ = new QLabel(""); selState_->setObjectName("muted");
    selSpeed_ = new QLabel(""); selSpeed_->setObjectName("mono");
    selEta_ = new QLabel(""); selEta_->setObjectName("mono");
    selSize_ = new QLabel(""); selSize_->setObjectName("mono");
    selSaveTo_ = new QLabel(""); selSaveTo_->setObjectName("mono");
    for (auto* l : {selSpeed_, selEta_, selSize_, selSaveTo_, selState_}) rv->addWidget(l);
    rv->addStretch(1);
    split->addWidget(right);

    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({210, 760, 300});
    root->addWidget(split, 1);
}

void MainWindow::applyTheme() {
    qApp->setStyleSheet(stylesheet(pal_));
    chart_->setPalette(&pal_);
    conns_->setPalette(&pal_);
    selBar_->setPalette(&pal_);
    themeBtn_->setText(pal_.dark ? "LIGHT" : "DARK");
    update();
}

std::string MainWindow::selectedId() const { return selectedId_.toStdString(); }

void MainWindow::onAdd() {
    bool ok = false;
    QString text = QInputDialog::getMultiLineText(
        this, "Add downloads", "One link per line (ranges like [1-100] expand):", "", &ok);
    if (ok && !text.trimmed().isEmpty()) {
        manager_.add_batch(text.toStdString());
        refresh();
    }
}

void MainWindow::refresh() {
    auto items = manager_.ordered();

    double globalSpeed = 0;
    int active = 0;
    for (auto& it : items)
        if (it->status == ST_DOWNLOADING) { globalSpeed += it->speed; ++active; }
    statSpeed_->setText(human(globalSpeed) + "/s");
    statSlots_->setText(QString("%1/%2").arg(active).arg(manager_.max_concurrent));
    statToday_->setText("—");
    statSched_->setText(manager_.on_done_action == "none" ? "OFF" : "ON");
    chart_->push(globalSpeed);

    rowBars_.clear();
    table_->setRowCount(static_cast<int>(items.size()));
    int selRow = -1;
    for (int r = 0; r < static_cast<int>(items.size()); ++r) {
        auto& it = items[r];
        auto* name = new QTableWidgetItem(QString::fromStdString(
            it->filename.empty() ? it->url : it->filename));
        name->setData(Qt::UserRole, QString::fromStdString(it->id));
        table_->setItem(r, 0, name);
        table_->setItem(r, 1, new QTableWidgetItem(it->size ? human((double)*it->size) : "—"));
        auto* bar = new SegmentBar;
        bar->setPalette(&pal_);
        double frac = it->percent() ? *it->percent() / 100.0 : 0.0;
        bar->set(frac, statusColor(pal_, it->status));
        table_->setCellWidget(r, 2, bar);
        rowBars_.push_back(bar);
        table_->setItem(r, 3, new QTableWidgetItem(
            it->status == ST_DOWNLOADING ? human(it->speed) + "/s" : "—"));
        table_->setItem(r, 4, new QTableWidgetItem(QString::fromStdString(it->status)));
        if (QString::fromStdString(it->id) == selectedId_) selRow = r;
    }
    if (selRow >= 0) table_->selectRow(selRow);

    // Selected panel
    std::shared_ptr<DownloadItem> sel;
    for (auto& it : items) if (QString::fromStdString(it->id) == selectedId_) sel = it;
    if (!sel && !items.empty()) { sel = items.front(); selectedId_ = QString::fromStdString(sel->id); }
    if (sel) {
        selName_->setText(QString::fromStdString(sel->filename.empty() ? sel->url : sel->filename));
        selHost_->setText(QString::fromStdString(sel->url));
        int pct = sel->percent() ? static_cast<int>(*sel->percent()) : 0;
        selPct_->setText(QString::number(pct) + "%");
        selBar_->set(pct / 100.0, statusColor(pal_, sel->status));
        conns_->setData(sel->conn_stats);
        selState_->setText(QString::fromStdString(sel->status));
        selSpeed_->setText("SPEED   " + (sel->status == ST_DOWNLOADING ? human(sel->speed) + "/s" : "—"));
        selEta_->setText(QString("TIME LEFT   ") +
                         (sel->eta ? QString::number((int)(*sel->eta / 60)) + " min" : "—"));
        selSize_->setText("SIZE   " + (sel->size ? human((double)*sel->size) : "—"));
        selSaveTo_->setText("SAVE TO   " + QString::fromStdString(sel->dest_path));
    }
}

} // namespace mpdm::gui
