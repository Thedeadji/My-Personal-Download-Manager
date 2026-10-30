// Update notifier with one-click update.
//
// Once a day (and on demand from Settings) the app asks GitHub for the latest
// release. If it's newer, a banner offers What's new / Update now / Later /
// Skip this version. "Update now" on Windows downloads the installer, checks it
// against the release's SHA256SUMS.txt, waits until no download is running, then
// runs it (normal UAC prompt) and quits; the installer restarts MPDM. On Linux it
// opens the release page (packages there are updated by the package manager or
// by downloading the new AppImage).
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "mainwindow.hpp"

#ifndef MPDM_VERSION
#define MPDM_VERSION "0.0.0"
#endif

namespace mpdm::gui {

static const char* kLatestApi =
    "https://api.github.com/repos/Thedeadji/My-Personal-Download-Manager/releases/latest";

QString MainWindow::appVersion() {
    // Dev/testing: pretend to be an older build to exercise the updater.
    QString fake = qEnvironmentVariable("MPDM_FAKE_VERSION");
    return fake.isEmpty() ? QString(MPDM_VERSION) : fake;
}

// a > b for dotted versions ("1.0.10" > "1.0.9"); a leading "v" is ignored.
static bool versionNewer(QString a, QString b) {
    if (a.startsWith('v')) a.remove(0, 1);
    if (b.startsWith('v')) b.remove(0, 1);
    const QStringList pa = a.split('.'), pb = b.split('.');
    for (int i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        int x = i < pa.size() ? pa[i].toInt() : 0, y = i < pb.size() ? pb[i].toInt() : 0;
        if (x != y) return x > y;
    }
    return false;
}

static void updLog(const QString& line) {  // dev: MPDM_UPDATE_LOG=<file> traces the flow
    QString path = qEnvironmentVariable("MPDM_UPDATE_LOG");
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) QTextStream(&f) << line << "\n";
}

static QNetworkRequest githubRequest(const QUrl& url) {
    QNetworkRequest rq(url);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString("MPDM/") + MainWindow::appVersion());
    rq.setRawHeader("Accept", "application/vnd.github+json");
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return rq;
}

void MainWindow::checkForUpdates(bool manual, std::function<void(const QString&)> report) {
    if (!net_) net_ = new QNetworkAccessManager(this);
    QSettings().setValue("update_last_check", QDateTime::currentSecsSinceEpoch());
    auto* reply = net_->get(githubRequest(QUrl(kLatestApi)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual, report] {
        reply->deleteLater();
        auto say = [&](const QString& t) { updLog(t); if (report) report(t); };
        if (reply->error() != QNetworkReply::NoError) {
            say("Couldn't check for updates: " + reply->errorString());
            return;
        }
        QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        QString tag = o.value("tag_name").toString();
        QString ver = tag.startsWith('v') ? tag.mid(1) : tag;
        if (ver.isEmpty()) { say("Couldn't read the latest release."); return; }
        if (!versionNewer(ver, appVersion())) {
            say(QString("MPDM %1 is the latest version.").arg(appVersion()));
            return;
        }
        upd_ = {};
        upd_.version = ver;
        upd_.page = o.value("html_url").toString();
        upd_.notes = o.value("body").toString();
        for (const auto& a : o.value("assets").toArray()) {
            QJsonObject ao = a.toObject();
            QString name = ao.value("name").toString(), url = ao.value("browser_download_url").toString();
            if (name.startsWith("MPDM-Setup-") && name.endsWith(".exe")) { upd_.installerName = name; upd_.installer = url; }
            if (name == "SHA256SUMS.txt") upd_.sums = url;
        }
        say(QString("MPDM %1 is available (you have %2).").arg(ver, appVersion()));
        // A skipped version stays quiet unless the user asked to check.
        if (!manual && QSettings().value("update_skip").toString() == ver) return;
        updateDismissed_ = false;
        showUpdateBar();
        if (tray_ && !manual && isHidden())
            tray_->showMessage("Update available",
                               QString("MPDM %1 is available. Open MPDM to update.").arg(ver), Logo::icon(), 6000);
    });
}

// Automatic check: once at startup, then (while running) at most every ~20 hours.
void MainWindow::maybeAutoCheckUpdates(bool startup) {
    if (!QSettings().value("update_check", true).toBool()) return;
    qint64 last = QSettings().value("update_last_check", 0).toLongLong();
    if (!startup && QDateTime::currentSecsSinceEpoch() - last < 20 * 3600) return;
    checkForUpdates(false);
}

void MainWindow::showUpdateBar() {
    if (!updateBar_ || upd_.version.isEmpty() || updateDismissed_) {
        if (updateBar_) updateBar_->hide();
        return;
    }
    QString state;
    if (!updateFile_.isEmpty())
        state = "  —  downloaded; it will install when the current downloads finish.";
    else if (updateBusy_)
        state = "  —  " + updateProgress_;
    updateText_->setText(QString("<b>MPDM %1 is available</b> (you have %2).%3")
                             .arg(upd_.version.toHtmlEscaped(), appVersion().toHtmlEscaped(), state.toHtmlEscaped()));
    if (updateNowBtn_) {
        updateNowBtn_->setEnabled(!updateBusy_);
        updateNowBtn_->setText(!updateFile_.isEmpty() ? "Install now" : "Update now");
    }
    updateBar_->show();
}

void MainWindow::startUpdate() {
#ifdef _WIN32
    if (!updateFile_.isEmpty()) {  // already downloaded: install right away
        manager_.pause_all();
        launchInstaller();
        return;
    }
    if (upd_.installer.isEmpty()) {  // no Windows installer in this release
        QDesktopServices::openUrl(QUrl(upd_.page));
        return;
    }
    if (!net_) net_ = new QNetworkAccessManager(this);
    updateBusy_ = true;
    updateProgress_ = "downloading…";
    showUpdateBar();

    // 1) the checksums, 2) the installer, 3) verify, 4) install when idle.
    auto fail = [this](const QString& why) {
        updLog("update failed: " + why);
        updateBusy_ = false;
        updateProgress_.clear();
        showUpdateBar();
        if (updateText_) updateText_->setText(QString("<b>Update failed:</b> %1").arg(why.toHtmlEscaped()));
    };
    auto* sumsReply = net_->get(githubRequest(QUrl(upd_.sums)));
    connect(sumsReply, &QNetworkReply::finished, this, [this, sumsReply, fail] {
        sumsReply->deleteLater();
        QString expected;
        if (sumsReply->error() == QNetworkReply::NoError)
            for (const QString& line : QString::fromUtf8(sumsReply->readAll()).split('\n')) {
                QStringList parts = line.simplified().split(' ');
                if (parts.size() == 2 && parts[1].remove('*') == upd_.installerName) expected = parts[0].toLower();
            }
        if (expected.isEmpty()) { fail("the release has no checksum for the installer"); return; }

        auto* reply = net_->get(githubRequest(QUrl(upd_.installer)));
        connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
            updateProgress_ = total > 0 ? QString("downloading… %1%").arg(got * 100 / total)
                                        : QString("downloading…");
            showUpdateBar();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, expected, fail] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) { fail(reply->errorString()); return; }
            QByteArray data = reply->readAll();
            QString got = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            if (got != expected) { fail("the download didn't match the published checksum"); return; }
            QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/MPDM-update";
            QDir().mkpath(dir);
            QString path = dir + "/" + upd_.installerName;
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
                fail("couldn't save the installer");
                return;
            }
            f.close();
            updLog("update verified: " + path);
            updateBusy_ = false;
            updateFile_ = path;
            showUpdateBar();
            if (manager_.active_count() == 0) launchInstaller();  // otherwise refresh() waits for idle
        });
    });
#else
    QDesktopServices::openUrl(QUrl(upd_.page));
#endif
}

// Run the verified installer (it shows its own progress and restarts MPDM), then quit.
void MainWindow::launchInstaller() {
    if (updateFile_.isEmpty()) return;
    const QString file = updateFile_;
    updateFile_.clear();
    if (qEnvironmentVariableIsSet("MPDM_UPDATE_DRYRUN")) {  // dev: stop before installing
        updLog("dry run: would launch " + file);
        return;
    }
    updLog("launching " + file);
    if (!QProcess::startDetached(file, {"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/RELAUNCH=1"})) {
        if (updateText_) updateText_->setText("<b>Couldn't start the installer.</b> Run it from " +
                                              QDir::toNativeSeparators(file).toHtmlEscaped());
        return;
    }
    quitApp();
}

// Dev: check, press "Update now", and capture the banner before/after the
// download (pair with MPDM_FAKE_VERSION, MPDM_UPDATE_DRYRUN, MPDM_UPDATE_LOG).
void MainWindow::devTestUpdate(const QString& out) {
    timer_.stop(); jobTimer_.stop();
    checkForUpdates(true, [this, out](const QString&) {
        QTimer::singleShot(300, this, [this, out] {
            grab().save(out + "_banner.png");
            if (upd_.version.isEmpty()) { qApp->quit(); return; }
            startUpdate();
            auto* poll = new QTimer(this);
            connect(poll, &QTimer::timeout, this, [this, out, poll] {
                QFile f(qEnvironmentVariable("MPDM_UPDATE_LOG"));
                QString log = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
                if (log.contains("dry run") || log.contains("update failed")) {
                    poll->stop();
                    grab().save(out + "_after.png");
                    qApp->quit();
                }
            });
            poll->start(500);
        });
    });
}

// The banner shown at the top of the main window when an update is available.
QWidget* MainWindow::buildUpdateBar() {
    const Palette& P = *pal_;
    auto hx = [](const QColor& c) { return c.name(QColor::HexArgb); };
    auto* bar = new QWidget;
    bar->setObjectName("updateBar");
    bar->setAutoFillBackground(true);
    bar->setStyleSheet("QWidget#updateBar{background:" + hx(P.rowSelected) + ";border-left:3px solid " +
                       hx(P.amber) + "}");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(14, 6, 8, 6);
    h->setSpacing(8);
    updateText_ = new QLabel;
    updateText_->setFont(barlow(13));
    updateText_->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    updateText_->setMinimumWidth(0);
    h->addWidget(updateText_, 1);
    auto mk = [&](const QString& t, bool primary) {
        auto* b = themed(new IndBtn(t, primary));
        b->setMinimumHeight(34);
        h->addWidget(b);
        return b;
    };
    auto* notes = mk("What's new", false);
    updateNowBtn_ = mk("Update now", true);
    auto* later = mk("Later", false);
    auto* skip = mk("Skip this version", false);
    connect(notes, &QAbstractButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(upd_.page)); });
    connect(updateNowBtn_, &QAbstractButton::clicked, this, [this] { startUpdate(); });
    connect(later, &QAbstractButton::clicked, this, [this] { updateDismissed_ = true; showUpdateBar(); });
    connect(skip, &QAbstractButton::clicked, this, [this] {
        QSettings().setValue("update_skip", upd_.version);
        updateDismissed_ = true;
        showUpdateBar();
    });
    bar->hide();
    updateBar_ = bar;
    return bar;
}

}  // namespace mpdm::gui
