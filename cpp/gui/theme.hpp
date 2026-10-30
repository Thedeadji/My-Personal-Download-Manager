// MPDM "Industrial" theme — palette tokens from DESIGN.md section 8.
// Custom-painted widgets read the active Palette; a light base stylesheet handles
// the plain chrome (inputs, scrollbars, tooltips).
#pragma once
#include <QColor>
#include <QString>

namespace mpdm::gui {

struct Palette {
    QColor frame, panel, panelHi, button, well;
    QColor row, rowAlt, rowSelected, cellOff, lampOff, grid;
    QColor bevelLight, bevelDark, insetDark, insetLight;
    QColor text, text2, text3;
    QColor amber, amberText, amberInk, amberTrack;
    QColor cyan, cyanText, cyanTrack;
    QColor green, greenText, red, redText, violet, queued;
    bool glows;
    bool dark;
};

inline const Palette kDark{
    QColor("#1A2129"), QColor("#222B35"), QColor("#2B3643"), QColor("#2B3643"), QColor("#0C1116"),
    QColor("#131A21"), QColor("#10161C"), QColor("#2B2111"), QColor("#1B242E"), QColor("#2A3542"), QColor("#18212B"),
    QColor("#3B4756"), QColor(0, 0, 0, 204), QColor(0, 0, 0, 230), QColor("#2E3946"),
    QColor("#E6EBF0"), QColor("#A9B4C1"), QColor("#8B97A5"),
    QColor("#FFA62B"), QColor("#FFA62B"), QColor("#1C1203"), QColor("#3A2A10"),
    QColor("#3BE0FF"), QColor("#3BE0FF"), QColor("#0E2A33"),
    QColor("#6BD66F"), QColor("#6BD66F"), QColor("#FF6B60"), QColor("#FF6B60"), QColor("#A68CFF"), QColor("#8B97A5"),
    true, true};

// Light ("Industrial light" mockup): near-white frame, white card panels with
// crisp light-grey borders, teal/amber accents, soft amber selected row.
inline const Palette kLight{
    QColor("#E7EBF1"), QColor("#FFFFFF"), QColor("#EEF2F7"), QColor("#EEF2F7"), QColor("#F4F7FA"),
    QColor("#FFFFFF"), QColor("#F5F8FB"), QColor("#FBEED6"), QColor("#DBE2EA"), QColor("#CDD5DE"), QColor("#E6EBF1"),
    QColor("#FFFFFF"), QColor("#D3DAE2"), QColor("#DBE2EA"), QColor("#FFFFFF"),
    QColor("#1C2630"), QColor("#4A5563"), QColor("#7A8695"),
    QColor("#EF9A1E"), QColor("#B5700A"), QColor("#3A2708"), QColor("#F7E2C0"),
    QColor("#1685A6"), QColor("#0F6E8C"), QColor("#D4EAF1"),
    QColor("#2E9E43"), QColor("#1F7A33"), QColor("#D0452F"), QColor("#B3261E"), QColor("#6A4FD6"), QColor("#8A95A3"),
    false, false};

// Base stylesheet for the plain chrome; custom widgets paint themselves.
inline QString baseStylesheet(const Palette& p) {
    auto c = [](const QColor& q) { return q.name(QColor::HexArgb); };
    return QString(R"(
        QWidget{background:%FRAME%;color:%TEXT%;font-family:'Barlow';font-size:13px}
        QToolTip{background:%PANEL%;color:%TEXT%;border:1px solid %BEVEL%}
        QLineEdit{background:transparent;border:0;color:%TEXT%;selection-background-color:%AMBER%;selection-color:%INK%}
        QScrollBar:vertical{background:transparent;width:10px;margin:0}
        QScrollBar::handle:vertical{background:%HANDLE%;border-radius:5px;min-height:24px}
        QScrollBar::add-line,QScrollBar::sub-line{height:0}
        QScrollBar:horizontal{background:transparent;height:10px;margin:0}
        QScrollBar::handle:horizontal{background:%HANDLE%;border-radius:5px;min-width:24px}
        QMenu{background:%PANEL%;color:%TEXT%;border:1px solid %BEVEL%}
        QMenu::item:selected{background:%PANELHI%}
        QComboBox QAbstractItemView{background:%PANEL%;color:%TEXT%;border:1px solid %BEVEL%;outline:0;
            selection-background-color:%AMBER%;selection-color:%INK%}
        QComboBox QAbstractItemView::item{min-height:24px;padding:2px 8px;color:%TEXT%}
        QCheckBox::indicator{width:16px;height:16px;background:%WELL%;border:1px solid %BEVEL%}
        QCheckBox::indicator:checked{background:%AMBER%;border:1px solid %AMBER%;image:url(:/icons/check.png)}
    )")
        .replace("%FRAME%", c(p.frame))
        .replace("%PANELHI%", c(p.panelHi))
        .replace("%PANEL%", c(p.panel))
        .replace("%WELL%", c(p.well))
        .replace("%TEXT%", c(p.text))
        .replace("%AMBER%", c(p.amber))
        .replace("%INK%", c(p.amberInk))
        .replace("%HANDLE%", c(p.text3))
        .replace("%LINE%", c(p.bevelLight))
        .replace("%BEVEL%", c(p.bevelLight));
}

} // namespace mpdm::gui
