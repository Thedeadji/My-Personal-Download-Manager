// MPDM desktop GUI — home window (Qt Widgets), built to the Industrial mockups.
#pragma once
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

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

private:
    void buildUi();
    void applyTheme();
    void refresh();
    void onAdd();
    std::string selectedId() const;

    QFrame* card(const QString& objName = "card");
    QWidget* statCard(const QString& title, QLabel*& valueOut);
    QWidget* categorySidebar();

    mpdm::DownloadManager manager_;
    Palette pal_ = darkPalette();

    QLabel* statSpeed_ = nullptr;
    QLabel* statSlots_ = nullptr;
    QLabel* statToday_ = nullptr;
    QLabel* statSched_ = nullptr;
    ThroughputChart* chart_ = nullptr;
    QTableWidget* table_ = nullptr;

    QLabel* selName_ = nullptr;
    QLabel* selHost_ = nullptr;
    QLabel* selPct_ = nullptr;
    QLabel* selSize_ = nullptr;
    QLabel* selState_ = nullptr;
    SegmentBar* selBar_ = nullptr;
    ConnectionsPanel* conns_ = nullptr;
    QLabel* selSpeed_ = nullptr;
    QLabel* selEta_ = nullptr;
    QLabel* selSaveTo_ = nullptr;

    QPushButton* themeBtn_ = nullptr;
    QTimer timer_;
    QString selectedId_;
    std::vector<SegmentBar*> rowBars_;
};

} // namespace mpdm::gui
