// Per-download progress window — a live view of a queue item the app owns.
// Closing it leaves the download running; when the item finishes the same
// window turns into the "download complete" view (Open / Show in Folder).
#pragma once
#include <adwaita.h>
#include <gtkmm.h>

#include <memory>
#include <string>

#include "mpdm/manager.hpp"

namespace mpdm::gtk {

class App;
class ConnectionLanes;

class DownloadWindow {
public:
    DownloadWindow(App& app, const std::string& id);
    ~DownloadWindow();
    void present();
    bool visible() const;
    GtkWindow* window() const { return GTK_WINDOW(win_); }

private:
    void build(const DownloadItem& it);
    GtkWidget* property_row(GtkWidget* list, const char* title, Gtk::Label*& value);
    GtkWidget* step_row(GtkWidget* list, const char* title, const char* subtitle);
    void set_step(GtkWidget* row, bool active, bool complete);
    bool tick();
    void set_state_class(const std::string& cls);

    App& app_;
    std::string id_;
    bool stream_ = false;
    GtkWidget* win_ = nullptr;
    AdwWindowTitle* title_ = nullptr;
    sigc::connection timer_;
    bool opened_folder_ = false;
    std::string state_class_;

    Gtk::Image* icon_ = nullptr;
    Gtk::Label* name_ = nullptr;
    Gtk::Label* source_ = nullptr;
    Gtk::ProgressBar* bar_ = nullptr;
    Gtk::Label* amount_ = nullptr;
    Gtk::Label* percent_ = nullptr;
    Gtk::Label* error_ = nullptr;

    // file downloads
    Gtk::Label* v_speed_ = nullptr;
    Gtk::Label* v_eta_ = nullptr;
    Gtk::Label* v_folder_ = nullptr;
    Gtk::Label* v_limit_ = nullptr;
    Gtk::Label* v_resume_ = nullptr;
    GtkWidget* speed_row_ = nullptr;
    GtkWidget* limit_row_ = nullptr;
    GtkWidget* folder_row_ = nullptr;
    Gtk::Widget* lanes_group_ = nullptr;
    ConnectionLanes* lanes_ = nullptr;
    Gtk::Label* lanes_note_ = nullptr;
    GtkWidget* open_when_done_ = nullptr;  // AdwSwitchRow

    // stream grabs
    GtkWidget* step1_ = nullptr;
    GtkWidget* step2_ = nullptr;
    GtkWidget* step3_ = nullptr;

    // footer buttons
    Gtk::Button* pause_ = nullptr;
    Gtk::Button* cancel_ = nullptr;
    Gtk::Button* retry_ = nullptr;
    Gtk::Button* show_ = nullptr;
    Gtk::Button* open_ = nullptr;
};

} // namespace mpdm::gtk
