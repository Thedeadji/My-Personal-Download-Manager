// Settings and Scheduler dialogs for the MPDM GUI, styled to the Industrial theme.
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mainwindow.hpp"
#include "mpdm/config.hpp"
#include "mpdm/ratelimit.hpp"

namespace mpdm::gui {

const Palette* paletteForMode(const QString& mode);  // defined in mainwindow.cpp

static QString hx(const QColor& c) { return c.name(QColor::HexRgb); }

static QString fieldQss(const Palette& P) {
    return QString(
               "QLabel{color:%2;background:transparent}"
               "QLineEdit,QComboBox,QSpinBox{background:%1;color:%2;border:0;padding:7px 10px;min-height:34px}"
               "QComboBox::drop-down{border:0;width:18px}"
               "QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%4}"
               "QCheckBox{color:%2;spacing:8px;background:transparent}"
               "QCheckBox::indicator{width:16px;height:16px;background:%1;border:1px solid %3}"
               "QCheckBox::indicator:checked{background:%4;border:1px solid %4}")
        .arg(hx(P.well), hx(P.text), hx(P.bevelLight), hx(P.amber));
}

void MainWindow::applyAppearance(const QString& mode) {
    QSettings().setValue("appearance", mode);
    pal_ = paletteForMode(mode);
    buildUi();
    applyTheme();
    refresh();
}

void MainWindow::openSettings() {
    const Palette& P = *pal_;
    QDialog dlg(this);
    dlg.setWindowTitle("Settings");
    dlg.setMinimumWidth(520);
    QPalette dp = dlg.palette(); dp.setColor(QPalette::Window, P.frame); dlg.setPalette(dp); dlg.setAutoFillBackground(true);
    dlg.setStyleSheet(fieldQss(P));

    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* panel = new Surface(Surface::Panel); panel->setPalette(pal_);
    outer->addWidget(panel);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(16, 16, 16, 16); v->setSpacing(12);
    auto head = [&](const char* t) {
        auto* l = new QLabel(QString(t).toUpper()); l->setFont(chakra(11, 600, 0.10));
        l->setStyleSheet("color:" + hx(P.text3) + ";background:transparent"); return l;
    };
    auto* titleL = new QLabel("Settings"); titleL->setFont(chakra(15, 600, 0.0));
    titleL->setStyleSheet("color:" + hx(P.text) + ";background:transparent");
    v->addWidget(titleL);

    auto& S = manager_.settings;
    auto* form = new QFormLayout(); form->setSpacing(8); form->setLabelAlignment(Qt::AlignLeft);

    v->addWidget(head("Appearance"));
    auto* appearance = new QComboBox(); appearance->addItems({"System", "Light", "Dark"});
    QString curMode = QSettings().value("appearance", "dark").toString();
    appearance->setCurrentText(curMode.left(1).toUpper() + curMode.mid(1));
    form->addRow("Theme", appearance);
    v->addLayout(form);

    v->addWidget(head("Downloads"));
    auto* form2 = new QFormLayout(); form2->setSpacing(8);
    auto* outRow = new QHBoxLayout();
    auto* outEdit = new QLineEdit(QString::fromStdString(S.output_dir));
    auto* browse = new IndBtn("Browse"); browse->setPalette(pal_);
    connect(browse, &QAbstractButton::clicked, &dlg, [&] {
        QString d = QFileDialog::getExistingDirectory(&dlg, "Output folder", outEdit->text());
        if (!d.isEmpty()) outEdit->setText(d);
    });
    outRow->addWidget(outEdit, 1); outRow->addWidget(browse);
    auto* outW = new QWidget(); outW->setLayout(outRow);
    form2->addRow("Output folder", outW);
    auto* categorize = new QCheckBox("Sort into type folders (Video/Music/…)"); categorize->setChecked(S.categorize);
    form2->addRow("Categorize", categorize);
    auto* maxConc = new QSpinBox(); maxConc->setRange(1, 10); maxConc->setValue(manager_.max_concurrent);
    form2->addRow("Max concurrent", maxConc);
    auto* conns = new QSpinBox(); conns->setRange(1, 32); conns->setValue(S.connections);
    form2->addRow("Connections / download", conns);
    auto* defLimit = new QLineEdit(S.max_rate_bps ? QString::fromStdString(format_rate((double)S.max_rate_bps)) : "");
    defLimit->setPlaceholderText("unlimited (e.g. 500k, 1.5m)");
    form2->addRow("Default speed limit", defLimit);
    auto* glLimit = new QLineEdit(manager_.global_rate_bps() ? QString::fromStdString(format_rate(manager_.global_rate_bps())) : "");
    glLimit->setPlaceholderText("unlimited");
    form2->addRow("Global speed limit", glLimit);
    auto* dynSeg = new QCheckBox("Work-steal idle connections near the end"); dynSeg->setChecked(S.dynamic_segmentation);
    form2->addRow("Dynamic segmentation", dynSeg);
    v->addLayout(form2);

    v->addWidget(head("Network"));
    auto* form3 = new QFormLayout(); form3->setSpacing(8);
    auto* ua = new QComboBox(); ua->addItems({"Honest (MPDM)", "Browser (Chrome)", "Custom"});
    auto* uaCustom = new QLineEdit();
    if (S.user_agent == DEFAULT_USER_AGENT) ua->setCurrentIndex(0);
    else if (S.user_agent == BROWSER_USER_AGENT) ua->setCurrentIndex(1);
    else { ua->setCurrentIndex(2); uaCustom->setText(QString::fromStdString(S.user_agent)); }
    uaCustom->setPlaceholderText("custom User-Agent string");
    uaCustom->setEnabled(ua->currentIndex() == 2);
    connect(ua, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [uaCustom](int i) { uaCustom->setEnabled(i == 2); });
    form3->addRow("User-Agent", ua);
    form3->addRow("Custom UA", uaCustom);
    auto* proxy = new QLineEdit(QString::fromStdString(S.proxy)); proxy->setPlaceholderText("http://host:8080 (blank = none)");
    form3->addRow("Proxy", proxy);
    auto* verifyTls = new QCheckBox("Verify TLS certificates"); verifyTls->setChecked(S.verify_tls);
    form3->addRow("Security", verifyTls);
    v->addLayout(form3);

    v->addWidget(head("When all downloads finish"));
    auto* form4 = new QFormLayout(); form4->setSpacing(8);
    auto* onDone = new QComboBox(); onDone->addItems({"Do nothing", "Run a command", "Sleep", "Shut down"});
    const char* acts[] = {"none", "run", "sleep", "shutdown"};
    for (int i = 0; i < 4; ++i) if (manager_.on_done_action == acts[i]) onDone->setCurrentIndex(i);
    auto* onCmd = new QLineEdit(QString::fromStdString(manager_.on_done_command)); onCmd->setPlaceholderText("command to run");
    onCmd->setEnabled(onDone->currentIndex() == 1);
    connect(onDone, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [onCmd](int i) { onCmd->setEnabled(i == 1); });
    form4->addRow("Action", onDone);
    form4->addRow("Command", onCmd);
    v->addLayout(form4);

    auto* btns = new QHBoxLayout(); btns->addStretch(1);
    auto* cancel = new IndBtn("Cancel"); cancel->setPalette(pal_);
    auto* save = new IndBtn("Save", true); save->setPalette(pal_);
    connect(cancel, &QAbstractButton::clicked, &dlg, &QDialog::reject);
    btns->addWidget(cancel); btns->addWidget(save);
    v->addLayout(btns);

    connect(save, &QAbstractButton::clicked, &dlg, [&] {
        auto rate = [](const QString& t) -> long long { try { return parse_rate(t.toStdString()); } catch (...) { return 0; } };
        S.output_dir = outEdit->text().toStdString();
        S.categorize = categorize->isChecked();
        manager_.max_concurrent = maxConc->value();
        S.connections = conns->value();
        S.max_rate_bps = rate(defLimit->text());
        S.dynamic_segmentation = dynSeg->isChecked();
        S.user_agent = ua->currentIndex() == 0 ? DEFAULT_USER_AGENT
                       : ua->currentIndex() == 1 ? BROWSER_USER_AGENT
                       : uaCustom->text().toStdString();
        S.proxy = proxy->text().toStdString();
        S.verify_tls = verifyTls->isChecked();
        manager_.set_global_rate((double)rate(glLimit->text()));
        manager_.set_on_complete(acts[onDone->currentIndex()], onCmd->text().toStdString());
        manager_.save_settings();
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
    auto* action = new QComboBox(); action->addItems({"Pause all", "Resume all", "Set global limit"});
    auto* at = new QLineEdit(); at->setPlaceholderText("HH:MM  or  2026-01-01T09:00");
    auto* repeat = new QComboBox(); repeat->addItems({"daily", "once"});
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
