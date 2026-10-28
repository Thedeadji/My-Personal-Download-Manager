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

inline const Palette kLight{
    QColor("#C9D1DA"), QColor("#E4E9EF"), QColor("#D5DCE4"), QColor("#EEF2F6"), QColor("#F5F7FA"),
    QColor("#EEF2F6"), QColor("#E8EDF2"), QColor("#F8E9CF"), QColor("#CCD4DD"), QColor("#CCD4DD"), QColor("#DCE2E9"),
    QColor("#FFFFFF"), QColor(40, 55, 70, 89), QColor(30, 40, 50, 71), QColor("#FFFFFF"),
    QColor("#18202A"), QColor("#3E4A57"), QColor("#56626F"),
    QColor("#B86E00"), QColor("#8F5200"), QColor("#1C1203"), QColor("#F3DFC0"),
    QColor("#0B8FAD"), QColor("#00708A"), QColor("#CDE9F0"),
    QColor("#2E9E43"), QColor("#1F7A33"), QColor("#C9372C"), QColor("#B3261E"), QColor("#6A4FD6"), QColor("#8A95A3"),
    false, false};

// Base stylesheet for the plain chrome; custom widgets paint themselves.
inline QString baseStylesheet(const Palette& p) {
    auto c = [](const QColor& q) { return q.name(QColor::HexArgb); };
    return QString(R"(
        QWidget{background:%FRAME%;color:%TEXT%;font-family:'Barlow';font-size:13px}
        QToolTip{background:%PANEL%;color:%TEXT%;border:1px solid %BEVEL%}
        QLineEdit{background:transparent;border:0;color:%TEXT%;selection-background-color:%AMBER%;selection-color:%INK%}
        QScrollBar:vertical{background:transparent;width:10px;margin:0}
        QScrollBar::handle:vertical{background:%LINE%;border-radius:5px;min-height:24px}
        QScrollBar::add-line,QScrollBar::sub-line{height:0}
        QScrollBar:horizontal{background:transparent;height:10px;margin:0}
        QScrollBar::handle:horizontal{background:%LINE%;border-radius:5px;min-width:24px}
        QMenu{background:%PANEL%;color:%TEXT%;border:1px solid %BEVEL%}
        QMenu::item:selected{background:%PANELHI%}
    )")
        .replace("%FRAME%", c(p.frame))
        .replace("%PANELHI%", c(p.panelHi))
        .replace("%PANEL%", c(p.panel))
        .replace("%TEXT%", c(p.text))
        .replace("%AMBER%", c(p.amber))
        .replace("%INK%", c(p.amberInk))
        .replace("%LINE%", c(p.bevelLight))
        .replace("%BEVEL%", c(p.bevelLight));
}

} // namespace mpdm::gui
