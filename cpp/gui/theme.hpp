// Theme tokens + Qt stylesheet for the MPDM GUI. Dark and light palettes mirror
// the mockups (Industrial@1x / Industrial light@2x). Amber is constant; the dark
// cyan deepens to a teal in light mode for contrast on white.
#pragma once
#include <QColor>
#include <QString>

namespace mpdm::gui {

struct Palette {
    bool dark;
    QColor bg, surface, surface2, line, text, muted, faint;
    QColor amber, accent, green, red, purple;  // accent = cyan (dark) / teal (light)
    QColor selRow;                              // selected queue row tint
};

inline Palette darkPalette() {
    Palette p;
    p.dark = true;
    p.bg = "#0b0e13"; p.surface = "#121821"; p.surface2 = "#0e141b"; p.line = "#212a35";
    p.text = "#e6e8ee"; p.muted = "#8a93a3"; p.faint = "#5b6675";
    p.amber = "#f6a01a"; p.accent = "#34cde6"; p.green = "#3fb950"; p.red = "#e5534b";
    p.purple = "#a371f7"; p.selRow = "#1a1608";
    return p;
}

inline Palette lightPalette() {
    Palette p;
    p.dark = false;
    p.bg = "#eef1f5"; p.surface = "#ffffff"; p.surface2 = "#f3f5f8"; p.line = "#d6dbe2";
    p.text = "#1f2630"; p.muted = "#5b6675"; p.faint = "#8a93a3";
    p.amber = "#f6a01a"; p.accent = "#1b8fae"; p.green = "#2e9e44"; p.red = "#d83a31";
    p.purple = "#7c53d6"; p.selRow = "#fdf0d8";
    return p;
}

// Global application stylesheet derived from a palette. Widgets that need custom
// painting (segmented bars, graph, connection bars) read the Palette directly.
inline QString stylesheet(const Palette& p) {
    auto c = [](const QColor& q) { return q.name(QColor::HexRgb); };
    return QString(R"(
        QWidget { background: %BG%; color: %TEXT%; font-family: "Segoe UI","Inter",sans-serif; font-size: 13px; }
        QFrame#card { background: %SURF%; border: 1px solid %LINE%; border-radius: 8px; }
        QFrame#titlebar { background: %SURF2%; border-bottom: 1px solid %LINE%; }
        QLabel#h { color: %FAINT%; font-size: 10px; letter-spacing: 1px; }
        QLabel#muted { color: %MUTED%; }
        QLabel#faint { color: %FAINT%; }
        QLabel#mono { font-family: "JetBrains Mono","Consolas",monospace; }
        QLineEdit { background: %SURF2%; border: 1px solid %LINE%; border-radius: 7px; padding: 7px 10px; color: %TEXT%; }
        QComboBox { background: %SURF%; border: 1px solid %LINE%; border-radius: 7px; padding: 6px 10px; color: %TEXT%; }
        QPushButton { background: %SURF%; border: 1px solid %LINE%; border-radius: 7px; padding: 8px 13px; color: %TEXT%; }
        QPushButton:hover { border-color: %AMBER%; }
        QPushButton#primary { background: %AMBER%; color: #0b0e13; border: none; font-weight: 700; }
        QPushButton:disabled { color: %FAINT%; }
        QHeaderView::section { background: %SURF%; color: %FAINT%; border: none; border-bottom: 1px solid %LINE%;
                              padding: 6px 8px; font-size: 10px; letter-spacing: 1px; }
        QTableWidget { background: %SURF%; gridline-color: transparent; border: none; }
        QTableWidget::item { border-bottom: 1px solid %LINE%; }
        QTableWidget::item:selected { background: %SEL%; color: %TEXT%; }
        QScrollBar:vertical { background: transparent; width: 10px; }
        QScrollBar::handle:vertical { background: %LINE%; border-radius: 5px; }
    )")
        .replace("%BG%", c(p.bg))
        .replace("%SURF2%", c(p.surface2))
        .replace("%SURF%", c(p.surface))
        .replace("%LINE%", c(p.line))
        .replace("%TEXT%", c(p.text))
        .replace("%MUTED%", c(p.muted))
        .replace("%FAINT%", c(p.faint))
        .replace("%AMBER%", c(p.amber))
        .replace("%SEL%", c(p.selRow));
}

} // namespace mpdm::gui
