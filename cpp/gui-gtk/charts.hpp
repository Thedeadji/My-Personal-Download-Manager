// Custom-drawn widgets: the throughput sparkline and the per-connection lanes.
// Both paint with the system accent + foreground colors so they follow the
// user's Adwaita light/dark style and accent choice.
#pragma once
#include <gtkmm.h>

#include <deque>
#include <vector>

#include "mpdm/models.hpp"

namespace mpdm::gtk {

class ThroughputGraph : public Gtk::DrawingArea {
public:
    ThroughputGraph();
    void push(double net_bps, double disk_bps);
    double peak() const;

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    std::deque<std::pair<double, double>> samples_;  // (network, disk) bytes/s
};

class ConnectionLanes : public Gtk::DrawingArea {
public:
    ConnectionLanes();
    void set_data(const std::vector<SegmentProgress>& conns);

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    std::vector<SegmentProgress> conns_;
};

// The current accent color (falls back to Adwaita blue).
Gdk::RGBA accent_rgba();

} // namespace mpdm::gtk
