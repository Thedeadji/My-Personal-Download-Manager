#include "charts.hpp"

#include <adwaita.h>

#include <algorithm>
#include <cmath>

#include "util.hpp"

namespace mpdm::gtk {

static constexpr size_t kSamples = 60;  // one per refresh tick (~1 min of history)

Gdk::RGBA accent_rgba() {
    GdkRGBA* c = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    Gdk::RGBA out("#3584e4");
    if (c) { out.set_rgba(c->red, c->green, c->blue, c->alpha); gdk_rgba_free(c); }
    return out;
}

static void rounded_rect(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double w, double h, double r) {
    r = std::min({r, w / 2, h / 2});
    cr->begin_new_sub_path();
    cr->arc(x + w - r, y + r, r, -M_PI / 2, 0);
    cr->arc(x + w - r, y + h - r, r, 0, M_PI / 2);
    cr->arc(x + r, y + h - r, r, M_PI / 2, M_PI);
    cr->arc(x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cr->close_path();
}

// ---- throughput sparkline ----------------------------------------------------

ThroughputGraph::ThroughputGraph() {
    set_content_height(72);
    set_hexpand(true);
    set_draw_func(sigc::mem_fun(*this, &ThroughputGraph::draw));
}

void ThroughputGraph::push(double net, double disk) {
    samples_.emplace_back(net, disk);
    while (samples_.size() > kSamples) samples_.pop_front();
    queue_draw();
}

double ThroughputGraph::peak() const {
    double p = 0;
    for (auto& s : samples_) p = std::max({p, s.first, s.second});
    return p;
}

void ThroughputGraph::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const Gdk::RGBA fg = get_color();
    const Gdk::RGBA accent = accent_rgba();
    const double top = 4, bottom = h - 1.0;

    // faint baseline
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.08);
    cr->set_line_width(1);
    cr->move_to(0, bottom - 0.5);
    cr->line_to(w, bottom - 0.5);
    cr->stroke();

    if (samples_.size() < 2) return;
    double pk = std::max(peak(), 1.0) * 1.15;
    double step = (double)w / (double)(kSamples - 1);
    double x0 = w - step * (samples_.size() - 1);
    auto y_of = [&](double v) { return bottom - (bottom - top) * (v / pk); };

    // network: smooth line + gradient fill in the accent color
    auto trace = [&](bool net) {
        size_t i = 0;
        double px = 0, py = 0;
        for (auto& s : samples_) {
            double x = x0 + step * i, y = y_of(net ? s.first : s.second);
            if (i == 0) cr->move_to(x, y);
            else {
                double cx = (px + x) / 2;
                cr->curve_to(cx, py, cx, y, x, y);
            }
            px = x; py = y; ++i;
        }
    };
    trace(true);
    cr->line_to(w, bottom);
    cr->line_to(x0, bottom);
    cr->close_path();
    auto grad = Cairo::LinearGradient::create(0, top, 0, bottom);
    grad->add_color_stop_rgba(0, accent.get_red(), accent.get_green(), accent.get_blue(), 0.32);
    grad->add_color_stop_rgba(1, accent.get_red(), accent.get_green(), accent.get_blue(), 0.0);
    cr->set_source(grad);
    cr->fill();

    trace(true);
    cr->set_source_rgba(accent.get_red(), accent.get_green(), accent.get_blue(), 1.0);
    cr->set_line_width(2);
    cr->set_line_join(Cairo::Context::LineJoin::ROUND);
    cr->stroke();

    // disk writes: a quiet dashed line in the foreground color
    trace(false);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.35);
    cr->set_line_width(1.25);
    cr->set_dash(std::vector<double>{3.0, 3.0}, 0);
    cr->stroke();
}

// ---- per-connection lanes ----------------------------------------------------

static constexpr int kLaneH = 6, kLaneGap = 14, kLabelW = 34, kSpeedW = 76;

ConnectionLanes::ConnectionLanes() {
    set_hexpand(true);
    set_content_height(kLaneGap);
    set_draw_func(sigc::mem_fun(*this, &ConnectionLanes::draw));
}

void ConnectionLanes::set_data(const std::vector<SegmentProgress>& conns) {
    conns_ = conns;
    std::sort(conns_.begin(), conns_.end(), [](auto& a, auto& b) { return a.index < b.index; });
    int rows = std::max<int>(1, (int)conns_.size());
    set_content_height(rows * (kLaneH + kLaneGap));
    queue_draw();
}

void ConnectionLanes::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
    const Gdk::RGBA fg = get_color();
    const Gdk::RGBA accent = accent_rgba();
    auto layout = create_pango_layout("");
    Pango::FontDescription fd = layout->get_context()->get_font_description();
    fd.set_size(int(fd.get_size() * 0.82));
    layout->set_font_description(fd);
    auto text = [&](const std::string& s, double x, double y, double alpha, bool right = false, double width = 0) {
        layout->set_text(s);
        int tw, th;
        layout->get_pixel_size(tw, th);
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), alpha);
        cr->move_to(right ? x + width - tw : x, y - th / 2.0);
        layout->show_in_cairo_context(cr);
    };

    if (conns_.empty()) {
        text("No active connections", 0, (kLaneH + kLaneGap) / 2.0, 0.55);
        return;
    }
    double barX = kLabelW, barW = std::max(20.0, (double)w - kLabelW - kSpeedW - 8);
    for (size_t i = 0; i < conns_.size(); ++i) {
        const auto& c = conns_[i];
        double cy = i * (kLaneH + kLaneGap) + (kLaneH + kLaneGap) / 2.0;
        text("C" + std::to_string(c.index + 1), 0, cy, 0.55);
        rounded_rect(cr, barX, cy - kLaneH / 2.0, barW, kLaneH, kLaneH / 2.0);
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.10);
        cr->fill();
        double frac = std::clamp(c.fraction(), 0.0, 1.0);
        if (frac > 0) {
            rounded_rect(cr, barX, cy - kLaneH / 2.0, std::max((double)kLaneH, barW * frac), kLaneH, kLaneH / 2.0);
            if (c.active) cr->set_source_rgba(accent.get_red(), accent.get_green(), accent.get_blue(), 1.0);
            else cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.35);
            cr->fill();
        }
        std::string sp = c.active && c.speed_bps > 0 ? human_speed(c.speed_bps) : (frac >= 1.0 ? "done" : "idle");
        text(sp, barX + barW + 8, cy, c.active ? 0.85 : 0.5, true, kSpeedW);
    }
}

} // namespace mpdm::gtk
