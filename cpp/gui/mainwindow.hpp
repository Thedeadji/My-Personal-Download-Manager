// MPDM desktop GUI — Industrial main window (Qt Widgets), per DESIGN.md.
#pragma once
#include <QGridLayout>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <vector>

#include "mpdm/manager.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace mpdm::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void applyTheme();
    void refresh();
    void onAdd();

    // register a widget that has setPalette(const Palette*)
    template <class W>
    W* themed(W* w) {
        themed_.push_back([w](const Palette* p) { w->setPalette(p); });
        return w;
    }

    mpdm::DownloadManager manager_;
    const Palette* pal_ = &kDark;
    std::vector<std::function<void(const Palette*)>> themed_;

    Graph* graph_ = nullptr;
    LedMeter* speedMeter_ = nullptr;
    LedMeter* selMeter_ = nullptr;
    Lanes* lanes_ = nullptr;
    QWidget* slotLamps_ = nullptr;
    QGridLayout* catsLay_ = nullptr;
    QVBoxLayout* statusLay_ = nullptr;
    QVBoxLayout* rowsLay_ = nullptr;

    QString selId_;
    QTimer timer_;
};

} // namespace mpdm::gui
