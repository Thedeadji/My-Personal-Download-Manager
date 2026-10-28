// Custom-painted widgets for the MPDM GUI: segmented progress bar, per-connection
// panel, and the throughput chart. Plain QWidget subclasses (no signals/slots), so
// no moc is required.
#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QWidget>

#include <algorithm>
#include <deque>
#include <vector>

#include "mpdm/models.hpp"
#include "theme.hpp"

namespace mpdm::gui {

// IDM-style segmented (ticked) progress bar.
class SegmentBar : public QWidget {
public:
    explicit SegmentBar(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(14); }
    void setPalette(const Palette* p) { pal_ = p; }
    void set(double fraction, QColor color) {
        frac_ = std::clamp(fraction, 0.0, 1.0);
        color_ = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        const int h = 8, y = (height() - h) / 2;
        const int segW = 6, gap = 3, step = segW + gap;
        const int n = std::max(1, (width() + gap) / step);
        const int filled = static_cast<int>(frac_ * n + 0.0001);
        for (int i = 0; i < n; ++i) {
            QRect r(i * step, y, segW, h);
            g.setBrush(i < filled ? color_ : pal_->line);
            g.setPen(Qt::NoPen);
            g.drawRoundedRect(r, 2, 2);
        }
    }

private:
    const Palette* pal_ = nullptr;
    double frac_ = 0;
    QColor color_;
};

// C1..CN connection bars with per-connection speed, from the engine telemetry.
class ConnectionsPanel : public QWidget {
public:
    explicit ConnectionsPanel(QWidget* parent = nullptr) : QWidget(parent) {}
    void setPalette(const Palette* p) { pal_ = p; }
    void setData(std::vector<SegmentProgress> conns, int maxRows = 8) {
        conns_ = std::move(conns);
        if (static_cast<int>(conns_.size()) > maxRows) conns_.resize(maxRows);
        setMinimumHeight(static_cast<int>(conns_.size()) * rowH_);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        QFont mono("Consolas"); mono.setPointSize(9);
        g.setFont(mono);
        const int segW = 6, gap = 3, step = segW + gap, barH = 6;
        for (int i = 0; i < static_cast<int>(conns_.size()); ++i) {
            const auto& c = conns_[i];
            int y = i * rowH_;
            int cy = y + (rowH_ - barH) / 2;
            g.setPen(pal_->faint);
            g.drawText(QRect(0, y, 24, rowH_), Qt::AlignVCenter | Qt::AlignLeft,
                       QString("C%1").arg(c.index + 1));
            int trackX = 28, trackW = width() - trackX - 46;
            int n = std::max(1, (trackW + gap) / step);
            int filled = static_cast<int>(c.fraction() * n + 0.0001);
            QColor col = (i == 0) ? pal_->amber : pal_->accent;  // C1 amber, like the mockup
            for (int s = 0; s < n; ++s) {
                QRect r(trackX + s * step, cy, segW, barH);
                g.setBrush(s < filled ? col : pal_->line);
                g.setPen(Qt::NoPen);
                g.drawRoundedRect(r, 2, 2);
            }
            g.setPen(pal_->muted);
            double mbps = c.speed_bps / (1024.0 * 1024.0);
            g.drawText(QRect(width() - 44, y, 44, rowH_), Qt::AlignVCenter | Qt::AlignRight,
                       QString::number(mbps, 'f', 1));
        }
    }

private:
    const Palette* pal_ = nullptr;
    std::vector<SegmentProgress> conns_;
    int rowH_ = 22;
};

// Rolling throughput line chart fed one global-speed sample per refresh.
class ThroughputChart : public QWidget {
public:
    explicit ThroughputChart(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(120); }
    void setPalette(const Palette* p) { pal_ = p; }
    void push(double bps) {
        samples_.push_back(bps);
        while (samples_.size() > cap_) samples_.pop_front();
        peak_ = std::max(peak_, bps);
        update();
    }
    double peak() const { return peak_; }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!pal_) return;
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing, true);
        QRectF area = rect().adjusted(1, 1, -1, -1);
        g.fillRect(area, pal_->surface2);
        // grid
        g.setPen(QPen(pal_->line, 1));
        for (int i = 1; i < 4; ++i) {
            double y = area.top() + area.height() * i / 4.0;
            g.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }
        if (samples_.size() < 2) return;
        double scale = std::max(peak_ * 1.15, 1.0);
        auto X = [&](int i) { return area.left() + area.width() * i / (cap_ - 1.0); };
        auto Y = [&](double v) { return area.bottom() - (v / scale) * area.height() * 0.92; };
        QPainterPath line;
        int off = static_cast<int>(cap_ - samples_.size());
        line.moveTo(X(off), Y(samples_.front()));
        for (int i = 1; i < static_cast<int>(samples_.size()); ++i) line.lineTo(X(off + i), Y(samples_[i]));
        QPainterPath fill = line;
        fill.lineTo(X(cap_ - 1), area.bottom());
        fill.lineTo(X(off), area.bottom());
        fill.closeSubpath();
        QColor a = pal_->accent; a.setAlpha(60);
        g.fillPath(fill, a);
        g.setPen(QPen(pal_->accent, 2));
        g.drawPath(line);
    }

private:
    const Palette* pal_ = nullptr;
    std::deque<double> samples_;
    size_t cap_ = 80;
    double peak_ = 1.0;
};

} // namespace mpdm::gui
