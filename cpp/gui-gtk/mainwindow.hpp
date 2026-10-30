// Main window: an adaptive sidebar (status + category filters, storage) and a
// content pane with a live throughput overview above the download list.
#pragma once
#include <adwaita.h>
#include <gtkmm.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mpdm/manager.hpp"

namespace mpdm::gtk {

class App;
class ThroughputGraph;

class MainWindow {
public:
    explicit MainWindow(App& app);
    GtkWindow* window() const { return GTK_WINDOW(win_); }
    void refresh();
    void toggle_search();
    void add_toast(AdwToast* toast);
    void seed_graph();  // dev: fill the graph with a plausible history

private:
    struct Filter {
        std::string key, title, icon;
        bool category = false;
        Gtk::ListBoxRow* row = nullptr;
        Gtk::Label* count = nullptr;
    };
    struct Row {
        std::string id;
        Gtk::ListBoxRow* row = nullptr;
        Gtk::Image* icon = nullptr;
        Gtk::Label* name = nullptr;
        Gtk::Label* pill = nullptr;
        Gtk::ProgressBar* bar = nullptr;
        Gtk::Label* detail = nullptr;
        Gtk::Button* primary = nullptr;
        Glib::RefPtr<Gio::SimpleActionGroup> actions;
        std::string state_class;  // css class currently applied (status)
    };

    void build_sidebar(GtkWidget*& page);
    void build_content(GtkWidget*& page);
    Gtk::Widget* build_overview();
    void rebuild_filters();
    std::unique_ptr<Row> make_row(const std::shared_ptr<DownloadItem>& it);
    void update_row(Row& r, const DownloadItem& it);
    bool matches(const DownloadItem& it, const std::string& filter) const;
    void on_primary(const std::string& id);
    void update_overview(const std::vector<std::shared_ptr<DownloadItem>>& items);

    App& app_;
    GtkWidget* win_ = nullptr;
    AdwToastOverlay* toasts_ = nullptr;
    AdwNavigationSplitView* split_ = nullptr;
    AdwWindowTitle* content_title_ = nullptr;

    Gtk::ListBox* sidebar_ = nullptr;
    std::vector<Filter> filters_;
    std::vector<std::string> custom_cats_;
    Gtk::LevelBar* storage_bar_ = nullptr;
    Gtk::Label* storage_label_ = nullptr;

    Gtk::Stack* stack_ = nullptr;         // "empty" | "list"
    Gtk::SearchBar* search_bar_ = nullptr;
    Gtk::SearchEntry* search_entry_ = nullptr;
    Gtk::MenuButton* limit_btn_ = nullptr;
    Gtk::ListBox* list_ = nullptr;
    Gtk::Label* list_title_ = nullptr;
    Gtk::Label* list_count_ = nullptr;
    Gtk::Label* no_match_ = nullptr;

    ThroughputGraph* graph_ = nullptr;
    Gtk::Label* speed_value_ = nullptr;
    Gtk::Label* speed_unit_ = nullptr;
    Gtk::Label* speed_sub_ = nullptr;
    Gtk::Label* peak_label_ = nullptr;

    std::map<std::string, std::unique_ptr<Row>> rows_;
    std::vector<std::string> order_;
    std::map<const Gtk::ListBoxRow*, int> pos_;  // row -> position in the manager's order
    std::string filter_ = "all";
    std::string query_;
    long long last_disk_bytes_ = -1;
    double last_disk_t_ = 0;
};

} // namespace mpdm::gtk
