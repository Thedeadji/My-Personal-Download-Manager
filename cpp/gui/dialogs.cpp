// Settings and Scheduler dialogs for the MPDM GUI, styled to the Industrial theme.
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QSystemTrayIcon>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "mainwindow.hpp"
#include "mpdm/config.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/ratelimit.hpp"

namespace mpdm::gui {

const Palette* paletteForMode(const QString& mode);  // defined in mainwindow.cpp

static QString hx(const QColor& c) { return c.name(QColor::HexRgb); }

// A combo box whose popup is a real QListView. Qt's Windows 11 style draws the
// default combo popup natively and ignores the stylesheet (dark-on-dark in the
// light theme); giving it an explicit item view makes the popup respect our QSS.
static QComboBox* mkCombo() {
    auto* c = new QComboBox((QWidget*)nullptr);
    c->setView(new QListView(c));
    return c;
}

// Stylesheet for the plain Qt controls inside a dialog. Fully palette-driven so
// every field, dropdown, checkbox and list reads correctly in BOTH themes — the
// light theme gives inputs a visible border and a near-black value text, and the
// combo-box POPUP (its own top-level window) is styled explicitly so its options
// are never dark-on-dark.
static QString fieldQss(const Palette& P) {
    // In light mode draw inputs on the white panel with a crisp border so they're
    // not flat/invisible; in dark mode keep the recessed well look.
    QString inputBg  = P.dark ? hx(P.well) : hx(P.panel);
    QString inputBrd = P.dark ? hx(P.bevelLight) : hx(P.bevelDark);
    QString popupBg  = P.dark ? hx(P.panel) : hx(P.panel);
    return QString(
               "QLabel{color:%TEXT%;background:transparent}"
               "QLineEdit,QComboBox,QSpinBox{background:%IN%;color:%TEXT%;border:1px solid %BRD%;"
               "padding:6px 10px;min-height:32px;selection-background-color:%AMBER%;selection-color:%INK%}"
               "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border:1px solid %AMBER%}"
               "QComboBox::drop-down{border:0;width:20px}"
               "QComboBox::down-arrow{image:none;border-left:4px solid transparent;border-right:4px solid transparent;"
               "border-top:5px solid %TEXT2%;margin-right:7px}"
               "QComboBox QAbstractItemView{background:%POP%;color:%TEXT%;border:1px solid %BRD%;outline:0;"
               "selection-background-color:%AMBER%;selection-color:%INK%}"
               "QComboBox QAbstractItemView::item{min-height:26px;padding:3px 8px}"
               "QComboBox QAbstractItemView::item:hover{background:%HI%}"
               "QSpinBox::up-button,QSpinBox::down-button{background:%HI%;border:0;width:16px}"
               "QCheckBox{color:%TEXT%;spacing:8px;background:transparent}"
               "QCheckBox::indicator{width:16px;height:16px;background:%IN%;border:1px solid %BRD%}"
               "QCheckBox::indicator:checked{background:%AMBER%;border:1px solid %AMBER%;image:url(:/icons/check.png)}"
               "QTabWidget::pane{border:1px solid %BRD%;background:%PANEL%;top:-1px}"
               "QTabBar{background:transparent}"
               "QTabBar::tab{background:%IN%;color:%TEXT3%;padding:8px 16px;margin-right:2px;"
               "border:1px solid transparent;font-family:'Chakra Petch';font-size:12px}"
               "QTabBar::tab:selected{color:%AMBER%;background:%PANEL%;border-color:%BRD%;border-bottom-color:%PANEL%}"
               "QTabBar::tab:!selected:hover{color:%TEXT%}")
        .replace("%IN%", inputBg).replace("%POP%", popupBg)
        .replace("%BRD%", inputBrd).replace("%TEXT2%", hx(P.text2)).replace("%TEXT3%", hx(P.text3))
        .replace("%TEXT%", hx(P.text)).replace("%AMBER%", hx(P.amber)).replace("%INK%", hx(P.amberInk))
        .replace("%PANEL%", hx(P.panel)).replace("%HI%", hx(P.panelHi));
}

void MainWindow::applyAppearance(const QString& mode) {
    QSettings().setValue("appearance", mode);
    pal_ = paletteForMode(mode);
    buildUi();
    applyTheme();
    // Rebuild the tray menu so its colors + icons follow the new theme.
    if (tray_) {
        if (trayMenu_) trayMenu_->deleteLater();
        trayMenu_ = buildTrayMenu();
        tray_->setContextMenu(trayMenu_);
    }
    refresh();
}

// The OS-configured proxy, as a URL ("" if none). Windows: the Internet Settings
// registry; elsewhere the http_proxy env var.
static std::string system_proxy_url() {
#ifdef _WIN32
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                  QSettings::NativeFormat);
    if (reg.value("ProxyEnable", 0).toInt() == 0) return "";
    QString ps = reg.value("ProxyServer").toString();
    if (ps.isEmpty()) return "";
    QString hostport = ps;
    if (ps.contains('=')) {  // "http=h:p;https=h:p;..." -> prefer the http entry
        hostport.clear();
        for (const QString& part : ps.split(';'))
            if (part.startsWith("http=")) { hostport = part.mid(5); break; }
        if (hostport.isEmpty()) hostport = ps.split(';').first().section('=', 1);
    }
    return hostport.isEmpty() ? std::string() : ("http://" + hostport).toStdString();
#else
    if (const char* e = std::getenv("http_proxy")) return e;
    if (const char* e = std::getenv("HTTP_PROXY")) return e;
    return "";
#endif
}

// --- "launch on startup" (cross-platform) -----------------------------------
// Windows: an HKCU Run-key value. Linux: a ~/.config/autostart/*.desktop entry.
static QString mpdm_launch_cmd() {
    QString exe = QCoreApplication::applicationFilePath();
#ifdef _WIN32
    exe.replace('/', '\\');
    return "\"" + exe + "\" --background";
#else
    return exe + " --background";
#endif
}

static bool autostart_enabled() {
#ifdef _WIN32
    QSettings k("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                QSettings::NativeFormat);
    return !k.value("MPDM").toString().isEmpty();
#else
    QString home = qEnvironmentVariable("HOME");
    return !home.isEmpty() && QFile::exists(home + "/.config/autostart/mpdm.desktop");
#endif
}

static void set_autostart(bool on) {
#ifdef _WIN32
    QSettings k("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                QSettings::NativeFormat);
    if (on) k.setValue("MPDM", mpdm_launch_cmd());
    else k.remove("MPDM");
#else
    QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty()) return;
    QString path = home + "/.config/autostart/mpdm.desktop";
    if (on) {
        QDir().mkpath(home + "/.config/autostart");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(&f) << "[Desktop Entry]\nType=Application\n"
                               "Name=MyPersonalDownloadManager\n"
                               "Exec=" << mpdm_launch_cmd() << "\n"
                               "Icon=mpdm\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
        }
    } else {
        QFile::remove(path);
    }
#endif
}

// A small themed modal frame used by the remove/confirm dialogs.
static Surface* dialogShell(QDialog& dlg, const Palette& P, const char* title, QVBoxLayout*& bodyOut) {
    dlg.setWindowTitle(title);
    QPalette dp = dlg.palette(); dp.setColor(QPalette::Window, P.frame); dlg.setPalette(dp); dlg.setAutoFillBackground(true);
    dlg.setStyleSheet(fieldQss(P));
    auto* outer = new QVBoxLayout(&dlg); outer->setContentsMargins(6, 6, 6, 6);
    auto* panel = new Surface(Surface::Panel); panel->setPalette(&P);
    outer->addWidget(panel);
    auto* v = new QVBoxLayout(panel); v->setContentsMargins(16, 16, 16, 16); v->setSpacing(12);
    auto* titleL = new QLabel(title); titleL->setFont(chakra(15, 600, 0.0));
    titleL->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    v->addWidget(titleL);
    bodyOut = v;
    return panel;
}

// "File already exists" prompt for the "ask" duplicate policy. Returns the chosen
// action: "rename", "skip" or "overwrite" (defaults to rename if dismissed).
QString MainWindow::askDuplicate(const QString& path) {
    const Palette& P = *pal_;
    QDialog dlg(this);
    QVBoxLayout* v = nullptr;
    dialogShell(dlg, P, "File already exists", v);
    dlg.setMinimumWidth(440);

    int k = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    QString name = k >= 0 ? path.mid(k + 1) : path;
    auto* msg = new QLabel("\"" + name + "\" already exists in that folder. What should MPDM do?");
    msg->setWordWrap(true); msg->setFont(barlow(13));
    msg->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
    v->addWidget(msg);

    QString choice = "rename";
    auto* btns = new QHBoxLayout();
    auto* renameB = new IndBtn("Rename", true); renameB->setPalette(pal_);
    auto* skipB = new IndBtn("Skip"); skipB->setPalette(pal_);
    auto* overB = new IndBtn("Overwrite"); overB->setPalette(pal_);
    btns->addWidget(renameB); btns->addStretch(1); btns->addWidget(skipB); btns->addWidget(overB);
    v->addLayout(btns);
    connect(renameB, &QAbstractButton::clicked, &dlg, [&] { choice = "rename"; dlg.accept(); });
    connect(skipB, &QAbstractButton::clicked, &dlg, [&] { choice = "skip"; dlg.accept(); });
    connect(overB, &QAbstractButton::clicked, &dlg, [&] { choice = "overwrite"; dlg.accept(); });
    dlg.exec();
    return choice;
}

// Remove the selected item, asking first whether to also delete the file on disk
// (with a "remember my choice" opt-out that mirrors the Settings checkbox).
void MainWindow::onRemove() {
    if (selId_.isEmpty()) return;
    std::string id = selId_.toStdString();
    std::shared_ptr<DownloadItem> it;
    for (auto& x : manager_.ordered()) if (x->id == id) { it = x; break; }
    if (!it) return;

    QSettings qs;
    bool ask = qs.value("ask_on_remove", true).toBool();
    bool deleteFile = qs.value("remove_delete_default", false).toBool();

    if (ask) {
        const Palette& P = *pal_;
        QDialog dlg(this);
        QVBoxLayout* v = nullptr;
        dialogShell(dlg, P, "Remove download", v);
        dlg.setMinimumWidth(380);

        QString nm = QString::fromStdString(it->filename.empty() ? it->url : it->filename);
        auto* msg = new QLabel("Remove \"" + nm + "\" from the list?");
        msg->setWordWrap(true); msg->setFont(barlow(13));
        msg->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
        v->addWidget(msg);

        auto* delChk = new QCheckBox("Also delete the downloaded file from disk");
        delChk->setChecked(deleteFile);
        v->addWidget(delChk);
        auto* remember = new QCheckBox("Remember my choice (don't ask again)");
        v->addWidget(remember);

        auto* btns = new QHBoxLayout(); btns->addStretch(1);
        auto* cancel = new IndBtn("Cancel"); cancel->setPalette(pal_);
        auto* ok = new IndBtn("Remove", true); ok->setPalette(pal_);
        connect(cancel, &QAbstractButton::clicked, &dlg, &QDialog::reject);
        connect(ok, &QAbstractButton::clicked, &dlg, &QDialog::accept);
        btns->addWidget(cancel); btns->addWidget(ok);
        v->addLayout(btns);

        if (dlg.exec() != QDialog::Accepted) return;
        deleteFile = delChk->isChecked();
        if (remember->isChecked()) {
            qs.setValue("ask_on_remove", false);
            qs.setValue("remove_delete_default", deleteFile);
        }
    }

    manager_.remove(id, deleteFile);
    finishAnnounced_.erase(id);
    selId_.clear();
    refresh();
}

// IDM-style "download complete" dialog: Open / Open folder / Close, with a
// "don't show this again" opt-out that flips the Settings pref.
void MainWindow::showFinishDialog(const std::shared_ptr<DownloadItem>& it) {
    if (!it) return;
    const Palette& P = *pal_;
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    QVBoxLayout* v = nullptr;
    dialogShell(*dlg, P, "Download complete", v);
    dlg->setMinimumWidth(360);

    QString path = QString::fromStdString(it->dest_path);
    QString nm = QString::fromStdString(it->filename.empty() ? it->url : it->filename);

    auto* row = new QHBoxLayout(); row->setSpacing(12);
    auto* icon = new QLabel("✓"); icon->setFixedSize(40, 40); icon->setAlignment(Qt::AlignCenter);
    icon->setFont(mono(20));
    icon->setStyleSheet("background:" + hx(P.well) + ";border:1px solid " + hx(P.bevelDark) + ";color:" + hx(P.green));
    row->addWidget(icon);
    auto* col = new QVBoxLayout(); col->setSpacing(2);
    auto* nameL = new QLabel(nm); nameL->setFont(barlow(14, 500));
    nameL->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    long long bytes = it->size ? *it->size : it->downloaded;
    auto human = [](double n) { const char* u[] = {"B","KB","MB","GB","TB"}; int i=0; while(n>=1024&&i<4){n/=1024;++i;} return QString::number(n,'f',i?1:0)+u[i]; };
    auto* amtL = new QLabel("Downloaded " + human((double)bytes) + QString(" (%1 bytes)").arg(bytes));
    amtL->setFont(mono(11)); amtL->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    col->addWidget(nameL); col->addWidget(amtL);
    row->addLayout(col, 1);
    v->addLayout(row);

    auto* saved = new QLabel("Saved to  " + (path.isEmpty() ? "—" : path));
    saved->setWordWrap(true); saved->setFont(mono(11));
    saved->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
    v->addWidget(saved);

    auto* dontShow = new QCheckBox("Don't show this dialog again");
    v->addWidget(dontShow);

    auto* btns = new QHBoxLayout();
    auto* openF = new IndBtn("Open"); openF->setPalette(pal_);
    auto* openD = new IndBtn("Open folder"); openD->setPalette(pal_);
    btns->addWidget(openF); btns->addWidget(openD); btns->addStretch(1);
    auto* close = new IndBtn("Close", true); close->setPalette(pal_);
    btns->addWidget(close);
    v->addLayout(btns);

    bool exists = !path.isEmpty() && std::filesystem::exists(path.toStdString());
    openF->setEnabled(exists); openD->setEnabled(exists);
    connect(openF, &QAbstractButton::clicked, dlg, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    connect(openD, &QAbstractButton::clicked, dlg, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(std::filesystem::path(path.toStdString()).parent_path().string())));
    });
    auto* dontPtr = dontShow;
    connect(close, &QAbstractButton::clicked, dlg, [dlg, dontPtr] {
        if (dontPtr->isChecked()) QSettings().setValue("finish_popup", false);
        dlg->close();
    });

    dlg->show();
    dlg->raise();
}

void MainWindow::devShotFinish(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    auto it = std::make_shared<DownloadItem>();
    it->id = "demo";
    it->filename = "ubuntu-24.04.1-desktop-amd64.iso";
    it->url = "https://releases.ubuntu.com/24.04.1/ubuntu-24.04.1-desktop-amd64.iso";
    it->size = 10385264; it->downloaded = 10385264; it->status = ST_COMPLETED;
    it->dest_path = "D:/Downloads/Programs/ubuntu-24.04.1-desktop-amd64.iso";
    showFinishDialog(it);
    QTimer::singleShot(600, this, [out] {
        for (QWidget* w : qApp->topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(w))
                if (d->windowTitle() == "Download complete") { d->grab().save(out); break; }
        qApp->quit();
    });
}

// Settings — a left nav of sections + a stacked content area + a sticky footer
// (Reset / Cancel / Save changes), styled to the Industrial mockup. Fully
// palette-driven and resizable down to a small window (each page scrolls).
void MainWindow::openSettings() {
    const Palette& P = *pal_;
    auto& S = manager_.settings;
    QDialog dlg(this);
    dlg.setWindowTitle("Settings");
    dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);  // our own title bar only
    dlg.setMinimumSize(540, 400);  // resizable / small-screen friendly (pages scroll)
    dlg.resize(780, 600);
    QPalette dp = dlg.palette(); dp.setColor(QPalette::Window, P.frame); dlg.setPalette(dp); dlg.setAutoFillBackground(true);

    const QString brd = P.dark ? hx(P.bevelLight) : hx(P.bevelDark);
    QString navQss = QString(
        "QListWidget{background:%WELL%;color:%TEXT2%;border:1px solid %BRD%;outline:0;padding:4px;"
        "font-family:'Chakra Petch';font-size:13px}"
        "QListWidget::item{padding:11px 12px;border-left:3px solid transparent}"
        "QListWidget::item:selected{background:%SEL%;color:%AMBERTEXT%;border-left:3px solid %AMBER%}"
        "QListWidget::item:hover:!selected{color:%TEXT%}"
        "QTableWidget{background:%WELL%;color:%TEXT%;border:1px solid %BRD%;gridline-color:%GRID%;outline:0}"
        "QHeaderView::section{background:%HI%;color:%TEXT3%;border:0;border-bottom:1px solid %BRD%;"
        "padding:6px 10px;font-family:'Chakra Petch';font-size:11px}"
        "QTableWidget::item{padding:6px 10px}"
        "QTableWidget::item:selected{background:%SEL%;color:%AMBERTEXT%}")
        .replace("%WELL%", hx(P.well)).replace("%TEXT2%", hx(P.text2)).replace("%TEXT3%", hx(P.text3))
        .replace("%BRD%", brd).replace("%SEL%", hx(P.rowSelected)).replace("%AMBERTEXT%", hx(P.amberText))
        .replace("%AMBER%", hx(P.amber)).replace("%GRID%", hx(P.grid)).replace("%HI%", hx(P.panelHi))
        .replace("%TEXT%", hx(P.text));
    dlg.setStyleSheet(fieldQss(P) + navQss);

    auto* outer = new QVBoxLayout(&dlg); outer->setContentsMargins(6, 6, 6, 6);
    auto* panel = new Surface(Surface::Panel); panel->setPalette(pal_);
    panel->setCursor(Qt::ArrowCursor);  // keep the frameless resize cursor to the 6px ring only
    outer->addWidget(panel);
    auto* root = new QVBoxLayout(panel); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);

    // --- title bar ---
    auto* bar = new QWidget; bar->setStyleSheet("background:transparent");
    auto* barL = new QHBoxLayout(bar);
    barL->setContentsMargins(16, 13, 12, 13); barL->setSpacing(10);
    auto* barLogo = new Logo(20); barLogo->setAttribute(Qt::WA_TransparentForMouseEvents);
    barL->addWidget(barLogo);
    auto* tt = new QLabel("Settings"); tt->setFont(chakra(15, 600, 0.0));
    tt->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    tt->setAttribute(Qt::WA_TransparentForMouseEvents);
    barL->addWidget(tt); barL->addStretch(1);
    auto* xBtn = new QPushButton("✕");
    xBtn->setCursor(Qt::PointingHandCursor); xBtn->setFixedSize(28, 28);
    xBtn->setStyleSheet("QPushButton{background:transparent;border:0;color:" + hx(P.text3) +
                        ";font-size:15px}QPushButton:hover{color:" + hx(P.text) + "}");
    connect(xBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    barL->addWidget(xBtn);
    root->addWidget(bar);
    // Frameless: drag the title bar to move, drag the dialog's edge ring to resize.
    new FramelessChrome(&dlg, bar, &dlg);
    auto* sep = new QFrame; sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color:" + brd + ";background:" + brd + ";max-height:1px");
    root->addWidget(sep);

    // --- nav + stacked pages ---
    auto* mid = new QHBoxLayout(); mid->setContentsMargins(16, 14, 16, 14); mid->setSpacing(16);
    auto* nav = new QListWidget(); nav->setFixedWidth(196);
    nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav->setFocusPolicy(Qt::NoFocus);
    for (const char* n : {"General", "Save locations", "Connections", "Speed", "Scheduler", "Proxy", "Browser"})
        nav->addItem(n);
    mid->addWidget(nav);
    auto* stack = new QStackedWidget(); stack->setStyleSheet("background:transparent");
    mid->addWidget(stack, 1);
    root->addLayout(mid, 1);
    connect(nav, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);

    // A page = a scrollable column. Returns the scroll-area; `col` is its layout.
    auto makePage = [&](QVBoxLayout*& col) -> QWidget* {
        auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame);
        sa->setStyleSheet("QScrollArea{background:transparent;border:0}");
        sa->viewport()->setStyleSheet("background:transparent");
        auto* page = new QWidget; page->setStyleSheet("background:transparent");
        col = new QVBoxLayout(page); col->setContentsMargins(2, 0, 6, 0); col->setSpacing(12);
        sa->setWidget(page);
        return sa;
    };
    auto sectionHead = [&](const QString& t) {
        auto* l = new QLabel(t); l->setFont(chakra(11, 600, 0.10));
        l->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
        return l;
    };
    auto makeForm = [&]() {
        auto* f = new QFormLayout(); f->setSpacing(10); f->setContentsMargins(0, 0, 0, 0);
        f->setLabelAlignment(Qt::AlignLeft);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return f;
    };

    // ========== General ==========
    QVBoxLayout* gc = nullptr; stack->addWidget(makePage(gc));
    gc->addWidget(sectionHead("APPEARANCE"));
    auto* fg = makeForm(); gc->addLayout(fg);
    auto* appearance = mkCombo(); appearance->addItems({"System", "Light", "Dark"});
    QString curMode = QSettings().value("appearance", "dark").toString();
    appearance->setCurrentText(curMode.left(1).toUpper() + curMode.mid(1));
    fg->addRow("Theme", appearance);
    auto* maxConc = new QSpinBox(); maxConc->setRange(1, 10); maxConc->setValue(manager_.max_concurrent);
    fg->addRow("Max concurrent downloads", maxConc);
    gc->addSpacing(4);
    gc->addWidget(sectionHead("BEHAVIOR"));
    auto* fg2 = makeForm(); gc->addLayout(fg2);
    auto* askRemove = new QCheckBox("Always ask when removing items");
    askRemove->setChecked(QSettings().value("ask_on_remove", true).toBool());
    fg2->addRow("Removing", askRemove);
    auto* finishPopup = new QCheckBox("Show a popup when a download finishes");
    finishPopup->setChecked(QSettings().value("finish_popup", true).toBool());
    fg2->addRow("On finish", finishPopup);
    auto* onDone = mkCombo(); onDone->addItems({"Do nothing", "Run a command", "Sleep", "Shut down"});
    const char* acts[] = {"none", "run", "sleep", "shutdown"};
    for (int i = 0; i < 4; ++i) if (manager_.on_done_action == acts[i]) onDone->setCurrentIndex(i);
    auto* onCmd = new QLineEdit(QString::fromStdString(manager_.on_done_command));
    onCmd->setPlaceholderText("command to run"); onCmd->setEnabled(onDone->currentIndex() == 1);
    connect(onDone, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [onCmd](int i) { onCmd->setEnabled(i == 1); });
    fg2->addRow("When all finish", onDone);
    fg2->addRow("Command", onCmd);

    gc->addSpacing(4);
    gc->addWidget(sectionHead("DOWNLOADS"));
    auto* showPopups = new QCheckBox("Show the download popup when a download starts");
    showPopups->setChecked(QSettings().value("show_popups", true).toBool());
    gc->addWidget(showPopups);
    auto* autostart = new QCheckBox("Start new downloads right away (uncheck to only add them to the queue)");
    autostart->setChecked(QSettings().value("autostart", true).toBool());
    gc->addWidget(autostart);
    auto* ignoreMtime = new QCheckBox("Ignore the server's file time when resuming a download");
    ignoreMtime->setChecked(S.resume_ignore_mtime);
    gc->addWidget(ignoreMtime);

    gc->addSpacing(4);
    gc->addWidget(sectionHead("STARTUP"));
    auto* runStartup = new QCheckBox("Launch MPDM when the system starts (in the background)");
    runStartup->setChecked(autostart_enabled());
    gc->addWidget(runStartup);
    gc->addStretch(1);

    // ========== Save locations ==========
    QVBoxLayout* sc = nullptr; stack->addWidget(makePage(sc));
    sc->addWidget(sectionHead("DEFAULT FOLDER"));
    auto* outRow = new QHBoxLayout();
    auto* outEdit = new QLineEdit(QString::fromStdString(S.output_dir));
    auto* browse = new IndBtn("Browse"); browse->setPalette(pal_);
    outRow->addWidget(outEdit, 1); outRow->addWidget(browse);
    sc->addLayout(outRow);
    auto* categorize = new QCheckBox("Move each finished file into its category's folder");
    categorize->setChecked(S.categorize); sc->addWidget(categorize);
    auto* dupRow = makeForm();
    auto* dupPolicy = mkCombo();
    dupPolicy->addItems({"Rename — save as name(1).ext", "Overwrite existing", "Skip — keep existing file",
                         "Ask each time"});
    const char* dupVals[] = {"rename", "overwrite", "skip", "ask"};
    for (int i = 0; i < 4; ++i) if (S.duplicate_policy == dupVals[i]) dupPolicy->setCurrentIndex(i);
    dupRow->addRow("If a file already exists", dupPolicy);
    sc->addLayout(dupRow);

    sc->addSpacing(4);
    sc->addWidget(sectionHead("CATEGORIES"));
    struct CatDef { const char* name; const char* sub; const char* exts; };
    static const CatDef BUILTIN[] = {
        {"Video", "Video", "mp4 mkv webm avi mov flv m4v ts wmv mpg mpeg"},
        {"Audio", "Music", "mp3 m4a aac flac wav ogg opus wma"},
        {"Documents", "Documents", "pdf doc docx xls xlsx ppt pptx txt epub csv"},
        {"Archives", "Compressed", "zip rar 7z tar gz tgz bz2 xz iso cab"},
        {"Programs", "Programs", "exe msi msix dmg pkg deb rpm apk appimage"},
        {"Other", "", ""},
    };
    auto catDirs = std::make_shared<std::map<std::string, std::string>>(S.category_dirs);
    auto catExts = std::make_shared<std::map<std::string, std::string>>(S.category_exts);
    auto remember = std::make_shared<std::map<std::string, bool>>(S.remember_category_folder);
    auto isBuiltin = [](const QString& n) { for (auto& c : BUILTIN) if (n == c.name) return true; return false; };
    auto builtinExts = [](const QString& n) -> QString {
        for (auto& c : BUILTIN) if (n == c.name) return c.exts;
        return "";
    };
    auto subOf = [](const QString& n) -> QString {
        for (auto& c : BUILTIN) if (n == c.name) return c.sub;  // "" for Other
        return n;  // custom category -> a folder of its own name
    };
    auto defaultFolder = [=](const QString& n) -> QString {
        QString sub = subOf(n);
        return sub.isEmpty() ? outEdit->text() : outEdit->text() + "/" + sub;
    };
    auto prettyExts = [](QString s) -> QString {
        s.replace(',', ' ').replace(';', ' ');
        return s.split(' ', Qt::SkipEmptyParts).join("  \xC2\xB7  ");  // " · "
    };

    auto* catTop = new QHBoxLayout();
    auto* catCombo = mkCombo();
    auto* newBtn = new IndBtn("New"); newBtn->setPalette(pal_);
    auto* editBtn = new IndBtn("Edit types"); editBtn->setPalette(pal_);
    catTop->addWidget(catCombo, 1); catTop->addWidget(newBtn); catTop->addWidget(editBtn);
    sc->addLayout(catTop);

    auto* typesCap = new QLabel("Automatically put these file types in this category");
    typesCap->setFont(barlow(12)); typesCap->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    sc->addWidget(typesCap);
    auto* typesLbl = new QLabel(); typesLbl->setWordWrap(true); typesLbl->setFont(mono(11));
    typesLbl->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
    sc->addWidget(typesLbl);

    auto* folderCap = new QLabel("Default download folder for this category");
    folderCap->setFont(barlow(13, 500)); folderCap->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
    sc->addWidget(folderCap);
    auto* catRow = new QHBoxLayout();
    auto* catPath = new QLineEdit();
    auto* catBrowse = new IndBtn("Browse"); catBrowse->setPalette(pal_);
    catRow->addWidget(catPath, 1); catRow->addWidget(catBrowse);
    sc->addLayout(catRow);
    auto* rememberChk = new QCheckBox("Remember the folder I last choose for this category");
    sc->addWidget(rememberChk);

    auto rebuildCombo = std::make_shared<std::function<void()>>();
    auto showCat = std::make_shared<std::function<void()>>();
    auto applyFolder = std::make_shared<std::function<void()>>();
    *rebuildCombo = [=]() {
        QString cur = catCombo->currentText();
        QSignalBlocker b(catCombo);
        catCombo->clear();
        for (auto& c : BUILTIN) catCombo->addItem(c.name);
        for (auto& kv : *catExts) { QString n = QString::fromStdString(kv.first); if (!isBuiltin(n)) catCombo->addItem(n); }
        int idx = catCombo->findText(cur);
        catCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    *showCat = [=]() {
        QString n = catCombo->currentText();
        std::string key = n.toStdString();
        if (n == "Other") {
            typesLbl->setText("The file types that are not listed in any other category.");
            editBtn->setEnabled(false);
        } else {
            QString all = builtinExts(n);
            auto it = catExts->find(key);
            if (it != catExts->end() && !it->second.empty())
                all = (all.isEmpty() ? QString() : all + " ") + QString::fromStdString(it->second);
            typesLbl->setText(prettyExts(all));
            editBtn->setEnabled(true);
        }
        QSignalBlocker b(catPath);
        auto dit = catDirs->find(key);
        catPath->setText(dit != catDirs->end() ? QString::fromStdString(dit->second) : QString());
        catPath->setPlaceholderText(defaultFolder(n));
        QSignalBlocker rb(rememberChk);
        auto rit = remember->find(key);
        rememberChk->setChecked(rit != remember->end() && rit->second);
    };
    *applyFolder = [=]() {
        std::string key = catCombo->currentText().toStdString();
        QString t = catPath->text().trimmed();
        if (t.isEmpty()) catDirs->erase(key);
        else (*catDirs)[key] = t.toStdString();
    };
    connect(catCombo, &QComboBox::currentTextChanged, &dlg, [=](const QString&) { (*showCat)(); });
    connect(catPath, &QLineEdit::editingFinished, &dlg, [=] { (*applyFolder)(); });
    connect(rememberChk, &QCheckBox::toggled, &dlg, [=](bool on) { (*remember)[catCombo->currentText().toStdString()] = on; });
    connect(catBrowse, &QAbstractButton::clicked, &dlg, [=, &dlg] {
        QString start = catPath->text().isEmpty() ? defaultFolder(catCombo->currentText()) : catPath->text();
        QString d = QFileDialog::getExistingDirectory(&dlg, "Category folder", start);
        if (!d.isEmpty()) { catPath->setText(d); (*applyFolder)(); }
    });
    connect(newBtn, &QAbstractButton::clicked, &dlg, [=, &dlg] {
        bool ok = false;
        QString name = QInputDialog::getText(&dlg, "New category", "Category name:", QLineEdit::Normal, "", &ok).trimmed();
        if (!ok || name.isEmpty() || isBuiltin(name)) return;
        QString types = QInputDialog::getText(&dlg, "File types for " + name,
                            "Extensions (space-separated, e.g. iso img bin):", QLineEdit::Normal, "", &ok).trimmed();
        (*catExts)[name.toStdString()] = types.toStdString();
        (*rebuildCombo)(); catCombo->setCurrentText(name); (*showCat)();
    });
    connect(editBtn, &QAbstractButton::clicked, &dlg, [=, &dlg] {
        QString n = catCombo->currentText();
        if (n == "Other") return;
        std::string key = n.toStdString();
        QString cur = QString::fromStdString(catExts->count(key) ? (*catExts)[key] : std::string());
        QString prompt = isBuiltin(n) ? "Extra extensions to also put here (added to the built-in list):"
                                      : "Extensions (space-separated):";
        bool ok = false;
        QString types = QInputDialog::getText(&dlg, "File types for " + n, prompt, QLineEdit::Normal, cur, &ok).trimmed();
        if (!ok) return;
        if (types.isEmpty() && !isBuiltin(n)) return;
        (*catExts)[key] = types.toStdString();
        (*showCat)();
    });
    connect(browse, &QAbstractButton::clicked, &dlg, [=, &dlg] {
        QString d = QFileDialog::getExistingDirectory(&dlg, "Default download folder", outEdit->text());
        if (!d.isEmpty()) { outEdit->setText(d); (*showCat)(); }
    });
    (*rebuildCombo)(); catCombo->setCurrentIndex(0); (*showCat)();

    sc->addSpacing(4);
    sc->addWidget(sectionHead("FILE DATES"));
    auto* fileDateChk = new QCheckBox("Set each file's date to the one the server reports");
    fileDateChk->setChecked(S.set_file_times);
    sc->addWidget(fileDateChk);

    sc->addSpacing(4);
    sc->addWidget(sectionHead("TEMPORARY DIRECTORY"));
    auto* tempRow = new QHBoxLayout();
    auto* tempEdit = new QLineEdit(QString::fromStdString(S.temp_dir));
    tempEdit->setPlaceholderText("Download straight into the destination (default)");
    auto* tempBrowse = new IndBtn("Browse"); tempBrowse->setPalette(pal_);
    tempRow->addWidget(tempEdit, 1); tempRow->addWidget(tempBrowse);
    sc->addLayout(tempRow);
    auto* tempNote = new QLabel("Parts download here, then the finished file is moved to its folder. "
                                "Using a directory on a different drive keeps downloads off your main disk.");
    tempNote->setWordWrap(true); tempNote->setFont(barlow(12));
    tempNote->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    sc->addWidget(tempNote);
    connect(tempBrowse, &QAbstractButton::clicked, &dlg, [=, &dlg] {
        QString d = QFileDialog::getExistingDirectory(&dlg, "Temporary directory", tempEdit->text());
        if (!d.isEmpty()) tempEdit->setText(d);
    });
    sc->addStretch(1);

    // ========== Connections ==========
    QVBoxLayout* cc = nullptr; stack->addWidget(makePage(cc));
    cc->addWidget(sectionHead("CONNECTIONS"));
    auto* fc = makeForm(); cc->addLayout(fc);
    auto* connType = mkCombo();
    connType->addItems({"Fiber / high-speed", "Broadband", "DSL / cable", "Slow / mobile", "Single", "Custom"});
    const int presetConns[] = {16, 8, 4, 2, 1};
    auto* conns = new QSpinBox(); conns->setRange(1, 32); conns->setValue(S.connections);
    auto syncPreset = [connType, presetConns](int n) {
        int idx = 5;
        for (int i = 0; i < 5; ++i) if (presetConns[i] == n) { idx = i; break; }
        QSignalBlocker b(connType); connType->setCurrentIndex(idx);
    };
    syncPreset(S.connections);
    connect(connType, QOverload<int>::of(&QComboBox::activated), &dlg, [conns, presetConns](int i) {
        if (i < 5) { QSignalBlocker b(conns); conns->setValue(presetConns[i]); }
    });
    connect(conns, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [syncPreset](int n) { syncPreset(n); });
    fc->addRow("Connection type", connType);
    fc->addRow("Connections / download", conns);
    auto* dynSeg = new QCheckBox("Work-steal idle connections near the end");
    dynSeg->setChecked(S.dynamic_segmentation);
    fc->addRow("Dynamic segmentation", dynSeg);

    cc->addSpacing(4);
    cc->addWidget(sectionHead("RETRIES"));
    auto* fcr = makeForm(); cc->addLayout(fcr);
    auto* retries = new QSpinBox(); retries->setRange(0, 50); retries->setValue(S.max_retries);
    retries->setSuffix(" times");
    fcr->addRow("Retry attempts on error", retries);
    auto* retryWait = new QSpinBox(); retryWait->setRange(0, 600); retryWait->setValue((int)S.retry_interval);
    retryWait->setSuffix(" sec"); retryWait->setSpecialValueText("Automatic");
    fcr->addRow("Wait between retries", retryWait);
    auto* retryNote = new QLabel("How often a failed connection is retried before the download gives up, "
                                 "and how long to wait between tries (Automatic backs off a bit longer each time).");
    retryNote->setWordWrap(true); retryNote->setFont(barlow(12));
    retryNote->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    cc->addWidget(retryNote);
    cc->addStretch(1);

    // ========== Speed ==========
    QVBoxLayout* spc = nullptr; stack->addWidget(makePage(spc));
    spc->addWidget(sectionHead("SPEED LIMITS"));
    auto* fs = makeForm(); spc->addLayout(fs);
    auto* defLimit = new QLineEdit(S.max_rate_bps ? QString::fromStdString(format_rate((double)S.max_rate_bps)) : "");
    defLimit->setPlaceholderText("unlimited (e.g. 500k, 1.5m)");
    fs->addRow("Per-download limit", defLimit);
    auto* glLimit = new QLineEdit(manager_.global_rate_bps() ? QString::fromStdString(format_rate(manager_.global_rate_bps())) : "");
    glLimit->setPlaceholderText("unlimited");
    fs->addRow("Global limit (all downloads)", glLimit);
    spc->addStretch(1);

    // ========== Scheduler ==========
    QVBoxLayout* schc = nullptr; stack->addWidget(makePage(schc));
    schc->addWidget(sectionHead("SCHEDULER"));
    auto* schNote = new QLabel("Set rules to pause, resume, or throttle downloads at a time of day.");
    schNote->setWordWrap(true); schNote->setFont(barlow(13));
    schNote->setStyleSheet("color:" + hx(P.text2) + ";background:transparent");
    schc->addWidget(schNote);
    auto* schBtn = new IndBtn("Open scheduler…"); schBtn->setPalette(pal_);
    connect(schBtn, &QAbstractButton::clicked, &dlg, [this] { openScheduler(); });
    auto* schBtnRow = new QHBoxLayout(); schBtnRow->addWidget(schBtn); schBtnRow->addStretch(1);
    schc->addLayout(schBtnRow);
    schc->addStretch(1);

    // ========== Proxy ==========
    QVBoxLayout* pc = nullptr; stack->addWidget(makePage(pc));
    pc->addWidget(sectionHead("PROXY"));

    auto* rbNone = new QCheckBox("No proxy — connect directly");
    auto* rbSystem = new QCheckBox("Use the system / browser proxy");
    auto* rbManual = new QCheckBox("Manual proxy configuration");
    auto* pgrp = new QButtonGroup(&dlg);  // exclusive: exactly one stays checked
    pgrp->addButton(rbNone); pgrp->addButton(rbSystem); pgrp->addButton(rbManual);
    pc->addWidget(rbNone); pc->addWidget(rbSystem); pc->addWidget(rbManual);

    auto* fp = makeForm(); fp->setContentsMargins(22, 0, 0, 0); pc->addLayout(fp);
    auto* pType = mkCombo(); pType->addItems({"HTTP", "HTTPS", "SOCKS5"});
    fp->addRow("Type", pType);
    auto* pHost = new QLineEdit(); pHost->setPlaceholderText("proxy server address");
    fp->addRow("Server", pHost);
    auto* pPort = new QLineEdit(); pPort->setPlaceholderText("port"); pPort->setFixedWidth(120);
    fp->addRow("Port", pPort);
    auto* pUser = new QLineEdit(); pUser->setPlaceholderText("optional");
    fp->addRow("Username", pUser);
    auto* pPass = new QLineEdit(); pPass->setEchoMode(QLineEdit::Password); pPass->setPlaceholderText("optional");
    fp->addRow("Password", pPass);

    pc->addSpacing(4);
    pc->addWidget(sectionHead("SECURITY"));
    auto* verifyTls = new QCheckBox("Verify TLS certificates"); verifyTls->setChecked(S.verify_tls);
    pc->addWidget(verifyTls);
    pc->addStretch(1);

    auto syncManual = [=] {
        bool m = rbManual->isChecked();
        for (QWidget* w : {(QWidget*)pType, (QWidget*)pHost, (QWidget*)pPort, (QWidget*)pUser, (QWidget*)pPass})
            w->setEnabled(m);
    };
    auto setProxyUI = [=](const std::string& p) {
        if (p.empty()) { rbNone->setChecked(true); }
        else if (p == "system") { rbSystem->setChecked(true); }
        else {
            rbManual->setChecked(true);
            QUrl u(QString::fromStdString(p));
            QString sc = u.scheme().toLower();
            pType->setCurrentIndex(sc.startsWith("socks") ? 2 : sc == "https" ? 1 : 0);
            pHost->setText(u.host());
            pPort->setText(u.port() > 0 ? QString::number(u.port()) : "");
            pUser->setText(u.userName());
            pPass->setText(u.password());
        }
        syncManual();
    };
    auto buildProxy = [=]() -> std::string {
        if (rbNone->isChecked()) return "";
        if (rbSystem->isChecked()) return system_proxy_url();
        QString host = pHost->text().trimmed();
        if (host.isEmpty()) return "";
        const char* schemes[] = {"http", "https", "socks5h"};
        QString url = QString(schemes[pType->currentIndex()]) + "://";
        QString user = pUser->text().trimmed(), pass = pPass->text();
        if (!user.isEmpty()) { url += user; if (!pass.isEmpty()) url += ":" + pass; url += "@"; }
        url += host;
        QString port = pPort->text().trimmed();
        if (!port.isEmpty()) url += ":" + port;
        return url.toStdString();
    };
    connect(rbNone, &QAbstractButton::toggled, &dlg, [syncManual] { syncManual(); });
    connect(rbSystem, &QAbstractButton::toggled, &dlg, [syncManual] { syncManual(); });
    connect(rbManual, &QAbstractButton::toggled, &dlg, [syncManual] { syncManual(); });
    setProxyUI(S.proxy);

    // ========== Browser & clipboard ==========
    QVBoxLayout* bc = nullptr; stack->addWidget(makePage(bc));
    bc->addWidget(sectionHead("IDENTITY"));
    auto* fb = makeForm(); bc->addLayout(fb);
    auto* ua = mkCombo(); ua->addItems({"Honest (MPDM)", "Browser (Chrome)", "Custom"});
    auto* uaCustom = new QLineEdit();
    if (S.user_agent == DEFAULT_USER_AGENT) ua->setCurrentIndex(0);
    else if (S.user_agent == BROWSER_USER_AGENT) ua->setCurrentIndex(1);
    else { ua->setCurrentIndex(2); uaCustom->setText(QString::fromStdString(S.user_agent)); }
    uaCustom->setPlaceholderText("custom User-Agent string");
    uaCustom->setEnabled(ua->currentIndex() == 2);
    connect(ua, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [uaCustom](int i) { uaCustom->setEnabled(i == 2); });
    fb->addRow("User-Agent", ua);
    fb->addRow("Custom UA", uaCustom);
    bc->addSpacing(4);
    bc->addWidget(sectionHead("CLIPBOARD"));
    auto* fb2 = makeForm(); bc->addLayout(fb2);
    auto* clip = new QCheckBox("Offer to add links I copy to the clipboard");
    clip->setChecked(manager_.clip_monitor_pref);
    fb2->addRow("Monitoring", clip);

    // ---- browser host: register the native-messaging host from inside the app ---
    bc->addSpacing(4);
    bc->addWidget(sectionHead("BROWSER HOST"));
    auto* hostNote = new QLabel(
        "Load the extension at chrome://extensions (Developer mode → Load unpacked → the "
        "\"extension\" folder next to MPDM), paste its ID here, and Register so the browser can "
        "hand its downloads to MPDM.");
    hostNote->setWordWrap(true); hostNote->setFont(barlow(12));
    hostNote->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    bc->addWidget(hostNote);
    auto* hostRow = new QHBoxLayout();
    auto* extIdEdit = new QLineEdit();
    extIdEdit->setPlaceholderText("extension ID from chrome://extensions");
    auto* regBtn = new IndBtn("Register", true); regBtn->setPalette(pal_);
    auto* unregBtn = new IndBtn("Remove"); unregBtn->setPalette(pal_);
    hostRow->addWidget(extIdEdit, 1); hostRow->addWidget(regBtn); hostRow->addWidget(unregBtn);
    bc->addLayout(hostRow);
    auto* hostStatus = new QLabel(); hostStatus->setFont(barlow(12));
    hostStatus->setStyleSheet("background:transparent"); hostStatus->setWordWrap(true);
    bc->addWidget(hostStatus);

    auto hostExe = [] {
        QString e = QCoreApplication::applicationDirPath() + "/mpdm_host";
#ifdef _WIN32
        e += ".exe";
#endif
        return e;
    };
    // Prefill the field with the currently-registered extension ID, if any.
    {
#ifdef _WIN32
        QString mf = qEnvironmentVariable("LOCALAPPDATA") + "/MPDM/com.mypersonaldownloadmanager.host.json";
#else
        QString mf = qEnvironmentVariable("HOME") +
                     "/.config/google-chrome/NativeMessagingHosts/com.mypersonaldownloadmanager.host.json";
#endif
        QFile f(mf);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString s = QString::fromUtf8(f.readAll());
            int i = s.indexOf("chrome-extension://");
            if (i >= 0) { i += 19; int j = s.indexOf('/', i); if (j > i) extIdEdit->setText(s.mid(i, j - i)); }
        }
    }
    auto runHost = [=](const QStringList& args, const QString& okMsg) {
        QString exe = hostExe();
        if (!QFile::exists(exe)) {
            hostStatus->setText("mpdm_host was not found next to the app.");
            hostStatus->setStyleSheet("color:" + hx(P.red) + ";background:transparent");
            return;
        }
        QProcess p;
        p.start(exe, args);
        bool fin = p.waitForFinished(15000);
        if (fin && p.exitCode() == 0) {
            hostStatus->setText(okMsg);
            hostStatus->setStyleSheet("color:" + hx(P.green) + ";background:transparent");
        } else {
            QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            hostStatus->setText("Failed. " + (err.isEmpty() ? QString("Could not run mpdm_host.") : err));
            hostStatus->setStyleSheet("color:" + hx(P.red) + ";background:transparent");
        }
    };
    connect(regBtn, &QAbstractButton::clicked, &dlg, [=] {
        QString id = extIdEdit->text().trimmed();
        if (id.isEmpty()) {
            hostStatus->setText("Enter the extension's ID first (from chrome://extensions).");
            hostStatus->setStyleSheet("color:" + hx(P.amberText) + ";background:transparent");
            return;
        }
        runHost({"--register", "--chrome", "--edge", "--extension-id", id},
                "Registered ✓  Reload the extension in your browser.");
    });
    connect(unregBtn, &QAbstractButton::clicked, &dlg, [=] {
        runHost({"--unregister", "--chrome", "--edge"}, "Browser host removed.");
    });

    bc->addStretch(1);

    // Force the popup colors straight onto every combo's item view — the surest
    // way to override the native (dark) popup the Windows 11 style would draw.
    QString popQss = QString(
        "QAbstractItemView{background:%1;color:%2;border:1px solid %3;outline:0;"
        "selection-background-color:%4;selection-color:%5}"
        "QAbstractItemView::item{min-height:26px;padding:4px 10px;color:%2}")
        .arg(hx(P.panel), hx(P.text), brd, hx(P.amber), hx(P.amberInk));
    for (auto* cb : dlg.findChildren<QComboBox*>())
        if (cb->view()) cb->view()->setStyleSheet(popQss);

    nav->setCurrentRow(0);
    // Dev: MPDM_SETTINGS_PAGE=<row> opens a given section (for screenshots).
    if (const char* pg = std::getenv("MPDM_SETTINGS_PAGE")) {
        int r = std::atoi(pg);
        if (r >= 0 && r < nav->count()) nav->setCurrentRow(r);
    }

    // --- footer ---
    auto* footSep = new QFrame; footSep->setFrameShape(QFrame::HLine);
    footSep->setStyleSheet("color:" + brd + ";background:" + brd + ";max-height:1px");
    root->addWidget(footSep);
    auto* foot = new QWidget; foot->setStyleSheet("background:transparent");
    auto* footL = new QHBoxLayout(foot);
    footL->setContentsMargins(16, 12, 16, 12); footL->setSpacing(10);
    auto* reset = new IndBtn("Reset to defaults"); reset->setPalette(pal_);
    footL->addWidget(reset); footL->addStretch(1);
    auto* cancel = new IndBtn("Cancel"); cancel->setPalette(pal_);
    auto* save = new IndBtn("Save changes", true); save->setPalette(pal_);
    footL->addWidget(cancel); footL->addWidget(save);
    root->addWidget(foot);
    connect(cancel, &QAbstractButton::clicked, &dlg, &QDialog::reject);

    connect(reset, &QAbstractButton::clicked, &dlg, [=, &S]() mutable {
        Settings d{};
        appearance->setCurrentText("Dark");
        maxConc->setValue(3);
        askRemove->setChecked(true); finishPopup->setChecked(true);
        showPopups->setChecked(true); autostart->setChecked(true); ignoreMtime->setChecked(true);
        runStartup->setChecked(false);
        onDone->setCurrentIndex(0); onCmd->clear();
        outEdit->setText(QString::fromStdString(d.output_dir));
        categorize->setChecked(d.categorize);
        dupPolicy->setCurrentIndex(0);
        catDirs->clear(); catExts->clear(); remember->clear();
        tempEdit->clear(); fileDateChk->setChecked(false);
        conns->setValue(d.connections); dynSeg->setChecked(d.dynamic_segmentation);
        defLimit->clear(); glLimit->clear();
        setProxyUI(""); verifyTls->setChecked(d.verify_tls);
        ua->setCurrentIndex(0); uaCustom->clear();
        clip->setChecked(false);
        retries->setValue(d.max_retries); retryWait->setValue((int)d.retry_interval);
        (*rebuildCombo)(); catCombo->setCurrentIndex(0); (*showCat)();
    });

    connect(save, &QAbstractButton::clicked, &dlg, [=, &S, &dlg]() mutable {
        auto rate = [](const QString& t) -> long long { try { return parse_rate(t.toStdString()); } catch (...) { return 0; } };
        (*applyFolder)();  // flush any in-progress category folder edit
        S.output_dir = outEdit->text().toStdString();
        S.categorize = categorize->isChecked();
        S.category_dirs = *catDirs;
        S.category_exts = *catExts;
        S.remember_category_folder = *remember;
        S.temp_dir = tempEdit->text().toStdString();
        S.set_file_times = fileDateChk->isChecked();
        S.duplicate_policy = dupVals[dupPolicy->currentIndex()];
        manager_.max_concurrent = maxConc->value();
        S.connections = conns->value();
        S.max_rate_bps = rate(defLimit->text());
        S.dynamic_segmentation = dynSeg->isChecked();
        S.user_agent = ua->currentIndex() == 0 ? DEFAULT_USER_AGENT
                       : ua->currentIndex() == 1 ? BROWSER_USER_AGENT
                       : uaCustom->text().toStdString();
        S.proxy = buildProxy();
        S.verify_tls = verifyTls->isChecked();
        S.max_retries = retries->value();
        S.retry_interval = retryWait->value();
        manager_.clip_monitor_pref = clip->isChecked();
        manager_.set_global_rate((double)rate(glLimit->text()));
        manager_.set_on_complete(acts[onDone->currentIndex()], onCmd->text().toStdString());
        S.resume_ignore_mtime = ignoreMtime->isChecked();
        manager_.save_settings();
        QSettings().setValue("ask_on_remove", askRemove->isChecked());
        QSettings().setValue("finish_popup", finishPopup->isChecked());
        QSettings().setValue("show_popups", showPopups->isChecked());
        QSettings().setValue("autostart", autostart->isChecked());
        set_autostart(runStartup->isChecked());
        QString mode = appearance->currentText().toLower();
        dlg.accept();
        if (mode != QSettings().value("appearance", "dark").toString()) applyAppearance(mode);
    });

    dlg.exec();
}

void MainWindow::openScheduler() {
    const Palette& P = *pal_;
    QDialog dlg(this);
    dlg.setWindowTitle("Scheduler");
    dlg.setMinimumWidth(520);
    QPalette dp = dlg.palette(); dp.setColor(QPalette::Window, P.frame); dlg.setPalette(dp); dlg.setAutoFillBackground(true);
    dlg.setStyleSheet(fieldQss(P));

    auto* outer = new QVBoxLayout(&dlg); outer->setContentsMargins(6, 6, 6, 6);
    auto* panel = new Surface(Surface::Panel); panel->setPalette(pal_);
    outer->addWidget(panel);
    auto* v = new QVBoxLayout(panel); v->setContentsMargins(16, 16, 16, 16); v->setSpacing(12);
    auto* titleL = new QLabel("Scheduler"); titleL->setFont(chakra(15, 600, 0.0));
    titleL->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    v->addWidget(titleL);

    auto* listBox = new QVBoxLayout(); listBox->setSpacing(6);
    v->addLayout(listBox);

    std::function<void()> rebuild;
    auto actionLabel = [](const std::string& a, long long rate) -> QString {
        if (a == "pause_all") return "Pause all";
        if (a == "resume_all") return "Resume all";
        return "Limit " + QString::fromStdString(format_rate((double)rate));
    };
    rebuild = [&] {
        while (QLayoutItem* it = listBox->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
        auto rules = manager_.list_schedules();
        if (rules.empty()) {
            auto* none = new QLabel("No rules yet."); none->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
            listBox->addWidget(none);
        }
        for (const auto& r : rules) {
            auto* well = new Surface(Surface::Well); well->setPalette(pal_);
            auto* h = new QHBoxLayout(well); h->setContentsMargins(10, 8, 10, 8); h->setSpacing(10);
            auto* en = new QCheckBox(); en->setChecked(r.enabled);
            std::string id = r.id;
            connect(en, &QCheckBox::toggled, &dlg, [this, id](bool on) { manager_.set_schedule_enabled(id, on); });
            h->addWidget(en);
            auto* txt = new QLabel(QString("%1  ·  %2  ·  %3").arg(actionLabel(r.action, r.rate_bps),
                                    QString::fromStdString(r.at), QString::fromStdString(r.repeat)));
            txt->setFont(mono(12)); txt->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
            h->addWidget(txt, 1);
            auto* rm = new IndBtn("Remove"); rm->setPalette(pal_);
            connect(rm, &QAbstractButton::clicked, &dlg, [this, id, &rebuild] { manager_.remove_schedule(id); rebuild(); });
            h->addWidget(rm);
            listBox->addWidget(well);
        }
    };
    rebuild();

    // add-rule row
    auto* addHead = new QLabel("ADD RULE"); addHead->setFont(chakra(11, 600, 0.10));
    addHead->setStyleSheet("color:" + hx(P.text3) + ";background:transparent");
    v->addWidget(addHead);
    auto* addRow = new QHBoxLayout(); addRow->setSpacing(6);
    auto* action = mkCombo(); action->addItems({"Pause all", "Resume all", "Set global limit"});
    auto* at = new QLineEdit(); at->setPlaceholderText("HH:MM  or  2026-01-01T09:00");
    auto* repeat = mkCombo(); repeat->addItems({"daily", "once"});
    auto* rate = new QLineEdit(); rate->setPlaceholderText("limit"); rate->setFixedWidth(90);
    addRow->addWidget(action); addRow->addWidget(at, 1); addRow->addWidget(repeat); addRow->addWidget(rate);
    auto* add = new IndBtn("Add", true); add->setPalette(pal_);
    addRow->addWidget(add);
    v->addLayout(addRow);
    auto* err = new QLabel(); err->setStyleSheet("color:" + hx(P.red) + ";background:transparent"); err->hide();
    v->addWidget(err);

    connect(add, &QAbstractButton::clicked, &dlg, [&] {
        const char* acts[] = {"pause_all", "resume_all", "set_global_rate"};
        long long rbps = 0; try { rbps = parse_rate(rate->text().toStdString()); } catch (...) {}
        try {
            manager_.add_schedule(acts[action->currentIndex()], at->text().toStdString(),
                                  repeat->currentText().toStdString(), {}, rbps);
            at->clear(); rate->clear(); err->hide();
            rebuild();
        } catch (const std::exception& e) {
            err->setText(e.what()); err->show();
        }
    });

    auto* btns = new QHBoxLayout(); btns->addStretch(1);
    auto* close = new IndBtn("Close"); close->setPalette(pal_);
    connect(close, &QAbstractButton::clicked, &dlg, &QDialog::accept);
    btns->addWidget(close);
    v->addLayout(btns);

    dlg.exec();
}

} // namespace mpdm::gui
