// Custom-painted Industrial widgets for the MPDM GUI (DESIGN.md sections 4-5):
// chamfered panels/wells with bevels, LED meters, lamps, connection lanes, the
// throughput graph, chamfered buttons, and the logo. Plain QWidget subclasses.
#pragma once
#include <QAbstractButton>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QWidget>

#include <algorithm>
#include <deque>
#include <vector>

#include "mpdm/models.hpp"
#include "theme.hpp"

namespace mpdm::gui {

// ---- fonts -----------------------------------------------------------------
inline QFont chakra(int px, int weight = 600, double tracking = 0.10) {
    QFont f("Chakra Petch");
    f.setPixelSize(px);
    f.setWeight(weight >= 600 ? QFont::DemiBold : QFont::Medium);
    f.setLetterSpacing(QFont::PercentageSpacing, 100 + tracking * 100);
    return f;
}
inline QFont barlow(int px, int weight = 400) {
    QFont f("Barlow");
    f.setPixelSize(px);
    f.setWeight(weight >= 500 ? QFont::Medium : QFont::Normal);
    return f;
}
inline QFont mono(int px, int weight = 400) {
    QFont f("JetBrains Mono");
    f.setPixelSize(px);
    f.setWeight(weight >= 500 ? QFont::Medium : QFont::Normal);
    return f;
}

// Chamfered outline. cut corners: top-left and/or bottom-right.
inline QPainterPath chamfer(const QRectF& r, qreal cut, bool tl, bool br) {
    QPainterPath p;
    qreal x = r.left(), y = r.top(), X = r.right(), Y = r.bottom();
    p.moveTo(tl ? x + cut : x, y);
    p.lineTo(X, y);
    if (br) { p.lineTo(X, Y - cut); p.lineTo(X - cut, Y); } else { p.lineTo(X, Y); }
    p.lineTo(x, Y);
    if (tl) p.lineTo(x, y + cut);
    p.closeSubpath();
    return p;
}

// Paint a raised panel or inset well into `w`'s rect, with 1px bevel lines.
inline void paintSurface(QPainter& g, const QRectF& r, const Palette& p, qreal cut, bool tl, bool br,
                         const QColor& fill, bool inset) {
    g.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path = chamfer(r.adjusted(0.5, 0.5, -0.5, -0.5), cut, tl, br);
    g.fillPath(path, fill);
    QColor hi = inset ? p.insetDark : p.bevelLight;    // top/left
    QColor lo = inset ? p.insetLight : p.bevelDark;    // bottom/right
    qreal x = r.left() + 0.5, y = r.top() + 0.5, X = r.right() - 0.5, Y = r.bottom() - 0.5;
    QPen pen(hi, 1);
    g.setPen(pen);
    g.drawLine(QPointF(tl ? x + cut : x, y), QPointF(X, y));      // top
    g.drawLine(QPointF(x, tl ? y + cut : y), QPointF(x, Y));      // left
    pen.setColor(lo);
    g.setPen(pen);
    g.drawLine(QPointF(x, Y), QPointF(br ? X - cut : X, Y));      // bottom
    g.drawLine(QPointF(X, y), QPointF(X, br ? Y - cut : Y));      // right
}

// ---- chamfered container ---------------------------------------------------
class Surface : public QWidget {
public:
    enum Kind { Panel, Well, TitleBar };
    explicit Surface(Kind kind = Panel, QWidget* parent = nullptr) : QWidget(parent), kind_(kind) {}
    void setPalette(const Palette* p) { pal_ = p; update(); }
    void setKind(Kind k) { kind_ = k; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        bool well = kind_ == Well;
        qreal cut = 8;
        bool tl = true, br = (kind_ == Panel || kind_ == Well);
        QColor fill = well ? pal_->well : (kind_ == TitleBar ? pal_->panelHi : pal_->panel);
        paintSurface(g, QRectF(rect()), *pal_, cut, tl, br, fill, well);
    }
    const Palette* pal_ = nullptr;
    Kind kind_;
};

// ---- lamp ------------------------------------------------------------------
inline void drawLamp(QPainter& g, QPointF c, const QColor& col, bool glow) {
    g.setRenderHint(QPainter::Antialiasing, true);
    if (glow) {
        QRadialGradient rg(c, 7);
        QColor gc = col; gc.setAlpha(150); rg.setColorAt(0, gc); gc.setAlpha(0); rg.setColorAt(1, gc);
        g.setBrush(rg); g.setPen(Qt::NoPen);
        g.drawEllipse(c, 7, 7);
    }
    g.setBrush(col); g.setPen(Qt::NoPen);
    g.drawEllipse(c, 3.5, 3.5);
}

// ---- LED meter -------------------------------------------------------------
class LedMeter : public QWidget {
public:
    LedMeter(int cells, int cw, int ch, int gap, QWidget* parent = nullptr)
        : QWidget(parent), cells_(cells), cw_(cw), ch_(ch), gap_(gap) {
        setFixedHeight(ch);
        if (cw > 0) setFixedWidth(cells * cw + (cells - 1) * gap);
    }
    void setPalette(const Palette* p) { pal_ = p; update(); }
    void set(double frac, QColor on) { frac_ = std::clamp(frac, 0.0, 1.0); on_ = on; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        int n = cells_;
        int cw = cw_ > 0 ? cw_ : std::max(2, (width() - (n - 1) * gap_) / n);
        int lit = int(frac_ * n + 1e-6);
        for (int i = 0; i < n; ++i) {
            g.fillRect(QRect(i * (cw + gap_), (height() - ch_) / 2, cw, ch_),
                       i < lit ? on_ : pal_->cellOff);
        }
    }
    const Palette* pal_ = nullptr;
    int cells_, cw_, ch_, gap_;
    double frac_ = 0;
    QColor on_;
};

// ---- connection lanes ------------------------------------------------------
class Lanes : public QWidget {
public:
    explicit Lanes(QWidget* parent = nullptr) : QWidget(parent) {}
    void setPalette(const Palette* p) { pal_ = p; update(); }
    void setData(std::vector<SegmentProgress> c) {
        conns_ = std::move(c);
        if ((int)conns_.size() > 8) conns_.resize(8);
        setMinimumHeight((int)conns_.size() * 18);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setFont(mono(11));
        int rh = 18, barH = 8;
        for (int i = 0; i < (int)conns_.size(); ++i) {
            const auto& c = conns_[i];
            int y = i * rh;
            bool stolen = c.stole_from >= 0;
            QColor col = stolen ? pal_->amber : pal_->cyan;
            QColor tr = stolen ? pal_->amberTrack : pal_->cyanTrack;
            g.setPen(stolen ? pal_->amber : pal_->text3);
            g.drawText(QRect(0, y, 24, rh), Qt::AlignVCenter | Qt::AlignLeft, QString("C%1").arg(c.index + 1));
            int bx = 28, bw = width() - bx - 44;
            int cy = y + (rh - barH) / 2;
            g.fillRect(QRect(bx, cy, bw, barH), tr);
            g.fillRect(QRect(bx, cy, int(bw * std::clamp(c.fraction(), 0.0, 1.0)), barH), col);
            g.setPen(pal_->text2);
            g.drawText(QRect(width() - 42, y, 42, rh), Qt::AlignVCenter | Qt::AlignRight,
                       QString::number(c.speed_bps / 1048576.0, 'f', 1));
        }
    }
    const Palette* pal_ = nullptr;
    std::vector<SegmentProgress> conns_;
};

// ---- throughput graph ------------------------------------------------------
class Graph : public QWidget {
public:
    explicit Graph(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(118); }
    void setPalette(const Palette* p) { pal_ = p; update(); }
    void push(double bps) { s_.push_back(bps); while (s_.size() > cap_) s_.pop_front(); peak_ = std::max(peak_, bps); update(); }
    double peak() const { return peak_; }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        QRectF a = rect();
        g.fillRect(a, pal_->well);
        g.setPen(QPen(pal_->grid, 1));
        for (int i = 1; i < 4; ++i) { qreal y = a.top() + a.height() * i / 4.0; g.drawLine(QPointF(a.left(), y), QPointF(a.right(), y)); }
        for (qreal x = 60; x < a.width(); x += 60) g.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        if (s_.size() < 2) return;
        g.setRenderHint(QPainter::Antialiasing, true);
        double sc = std::max(peak_ * 1.15, 1.0);
        auto X = [&](int i) { return a.left() + a.width() * i / (cap_ - 1.0); };
        auto Y = [&](double v) { return a.bottom() - (v / sc) * a.height() * 0.9; };
        QPainterPath line;
        int off = int(cap_ - s_.size());
        line.moveTo(X(off), Y(s_[0]));
        for (int i = 1; i < (int)s_.size(); ++i) line.lineTo(X(off + i), Y(s_[i]));
        QPainterPath fill = line;
        fill.lineTo(X(cap_ - 1), a.bottom()); fill.lineTo(X(off), a.bottom()); fill.closeSubpath();
        QColor fc = pal_->cyan; fc.setAlphaF(0.12);
        g.fillPath(fill, fc);
        g.setPen(QPen(pal_->cyan, 1.6));
        g.drawPath(line);
    }
    const Palette* pal_ = nullptr;
    std::deque<double> s_;
    size_t cap_ = 80;
    double peak_ = 1;
};

// ---- chamfered button ------------------------------------------------------
class IndBtn : public QAbstractButton {
public:
    explicit IndBtn(const QString& label, bool primary = false, QWidget* parent = nullptr)
        : QAbstractButton(parent), primary_(primary) {
        setText(label);
        setMinimumHeight(44);
        setCursor(Qt::PointingHandCursor);
        QFontMetrics fm(chakra(12, 600, 0.06));
        setMinimumWidth(fm.horizontalAdvance(label.toUpper()) + 28);
    }
    void setPalette(const Palette* p) { pal_ = p; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        qreal cut = 5;
        QColor fill = primary_ ? pal_->amber : pal_->panelHi;
        if (!isEnabled()) g.setOpacity(0.38);
        else if (underMouse() && !primary_) fill = fill.lighter(112);
        paintSurface(g, QRectF(rect()), *pal_, cut, true, true, fill, false);
        g.setFont(chakra(12, 600, 0.06));
        g.setPen(primary_ ? pal_->amberInk : pal_->text);
        g.drawText(rect(), Qt::AlignCenter, text().toUpper());
    }
    bool primary_;
    const Palette* pal_ = nullptr;
};

// ---- logo ------------------------------------------------------------------
class Logo : public QWidget {
public:
    explicit Logo(int sz = 22, QWidget* parent = nullptr) : QWidget(parent) { setFixedSize(sz, sz); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        g.scale(width() / 64.0, height() / 64.0);
        QPainterPath shield; shield.moveTo(6, 10); shield.lineTo(14, 2); shield.lineTo(58, 2);
        shield.lineTo(58, 44); shield.lineTo(32, 62); shield.lineTo(6, 44); shield.closeSubpath();
        g.fillPath(shield, QColor("#222B35"));
        g.setPen(QPen(QColor("#FFA62B"), 3)); g.drawPath(shield);
        QPainterPath up; up.moveTo(28.5, 13); up.lineTo(35.5, 13); up.lineTo(35.5, 22.6); up.lineTo(28.5, 25.4); up.closeSubpath();
        g.fillPath(up, QColor("#FFA62B"));
        QPainterPath lo; lo.moveTo(28.5, 28.9); lo.lineTo(35.5, 26.1); lo.lineTo(35.5, 39); lo.lineTo(32, 41.7); lo.lineTo(28.5, 39); lo.closeSubpath();
        g.fillPath(lo, QColor("#3BE0FF"));
        g.setBrush(QColor("#FFA62B")); g.setPen(Qt::NoPen); g.drawEllipse(QPointF(14.5, 11.5), 2.5, 2.5);
        QPainterPath ch; ch.moveTo(19, 41); ch.lineTo(32, 51); ch.lineTo(45, 41);
        g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor("#E6EBF0"), 4.5)); g.drawPath(ch);
    }
};

} // namespace mpdm::gui
