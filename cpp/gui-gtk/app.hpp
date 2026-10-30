// MPDM GTK4 / libadwaita front-end — application object. Owns the download
// manager, the main window, and one progress window per watched download, and
// routes every user flow (add, remove, preferences, notifications) through here.
#pragma once
#include <adwaita.h>
#include <gtkmm.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "mpdm/manager.hpp"
#include "uiprefs.hpp"

namespace mpdm::gtk {

class MainWindow;
class DownloadWindow;

inline constexpr const char* APP_ID = "com.mypersonaldownloadmanager.MPDM";

// Dev-only screenshot mode (--shot*): render a surface to PNG, then quit.
struct ShotRequest {
    std::string kind;  // "" (off) | main | popup | stream | prefs | add | remove
    std::string out;
    std::string page;  // preferences page name for kind == prefs
    bool demo = false;
};

class App {
public:
    explicit App(ShotRequest shot);
    ~App();
    int run(int argc, char** argv);

    GtkApplication* gtk_app() const { return app_->gobj(); }
    DownloadManager& manager() { return *manager_; }
    UiPrefs& prefs() { return prefs_; }
    GtkWindow* main_window() const;
    GtkWidget* dialog_parent() const;  // widget AdwDialogs are presented on
    std::shared_ptr<DownloadItem> item(const std::string& id);
    std::string exe_path() const;

    // user flows
    void present_main();
    void open_download_window(const std::string& id);
    void close_download_window(const std::string& id);
    void show_add_dialog();
    void show_preferences(const char* page = nullptr);
    void confirm_remove(const std::string& id);
    void ask_item_limit(const std::string& id);
    void show_about();
    void toast(const std::string& title, const char* button = nullptr, std::function<void()> on_button = {});

    void apply_appearance();
    void refresh();          // main-window tick (also checks completions)
    void sync_speed_limit_action();

private:
    void on_startup();
    int on_command_line(const Glib::RefPtr<Gio::ApplicationCommandLine>& cl);
    void setup_actions();
    void poll_jobs();
    void announce_finished();
    void notify(const std::string& id, const std::string& title, const std::string& body,
                const std::string& path);
    std::string ask_duplicate_blocking(const std::string& path);
    void ask_duplicate(const std::string& path, std::function<void(std::string)> done);
    void quit();
    void run_shot();
    void seed_demo();

    Glib::RefPtr<Gtk::Application> app_;
    Glib::RefPtr<Gio::SimpleAction> limit_action_;
    std::unique_ptr<DownloadManager> manager_;
    UiPrefs prefs_;
    std::unique_ptr<MainWindow> main_;
    std::map<std::string, std::unique_ptr<DownloadWindow>> popups_;
    std::set<std::string> finish_announced_;
    std::atomic<bool> quitting_{false};
    std::thread::id main_thread_;
    ShotRequest shot_;
    bool started_ = false;
};

} // namespace mpdm::gtk
