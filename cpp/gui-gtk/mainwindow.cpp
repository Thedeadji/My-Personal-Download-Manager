#include "mainwindow.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

#include "app.hpp"
#include "charts.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace mpdm::gtk {

static double mono_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static const char* kBuiltinCats[] = {"Video", "Audio", "Documents", "Archives", "Programs", "Other"};

static std::string status_class(const DownloadItem& it) {
    if (it.status == ST_COMPLETED) return "completed";
    if (it.status == ST_ERROR) return "error";
    if (it.status == ST_PAUSED) return "paused";
    if (it.status == ST_QUEUED) return "queued";
    return "downloading";
}

static const char* status_title(const std::string& st) {
    if (st == ST_COMPLETED) return "Completed";
    if (st == ST_ERROR) return "Failed";
    if (st == ST_PAUSED) return "Paused";
    if (st == ST_QUEUED) return "Queued";
    return "Downloading";
}

static Glib::RefPtr<Gio::Menu> main_menu() {
    auto menu = Gio::Menu::create();
    auto queue = Gio::Menu::create();
    queue->append("_Resume All", "app.resume-all");
    queue->append("_Pause All", "app.pause-all");
    queue->append("_Clear Completed", "app.clear-finished");
    menu->append_section(queue);
    auto app = Gio::Menu::create();
    app->append("_Scheduler", "app.scheduler");
    app->append("_Preferences", "app.preferences");
    app->append("_About MPDM", "app.about");
    menu->append_section(app);
    auto quit = Gio::Menu::create();
    quit->append("_Quit", "app.quit");
    menu->append_section(quit);
    return menu;
}

static Glib::RefPtr<Gio::Menu> limit_menu() {
    auto menu = Gio::Menu::create();
    auto presets = Gio::Menu::create();
    presets->append("Unlimited", "app.speed-limit::0");
    presets->append("500 KB/s", "app.speed-limit::500k");
    presets->append("1 MB/s", "app.speed-limit::1m");
    presets->append("2 MB/s", "app.speed-limit::2m");
    presets->append("5 MB/s", "app.speed-limit::5m");
    menu->append_section("Global Speed Limit", presets);
    return menu;
}

static Glib::RefPtr<Gio::Menu> row_menu() {
    static Glib::RefPtr<Gio::Menu> menu;
    if (menu) return menu;
    menu = Gio::Menu::create();
    auto a = Gio::Menu::create();
    a->append("_Details", "row.details");
    a->append("_Open", "row.open");
    a->append("Show in _Folder", "row.show");
    a->append("_Copy Link", "row.copy");
    menu->append_section(a);
    auto b = Gio::Menu::create();
    b->append("Speed _Limit…", "row.limit");
    menu->append_section(b);
    auto c = Gio::Menu::create();
    c->append("_Remove…", "row.remove");
    menu->append_section(c);
    return menu;
}

// ---- construction ------------------------------------------------------------

MainWindow::MainWindow(App& app) : app_(app) {
    win_ = adw_application_window_new(app_.gtk_app());
    gtk_window_set_title(GTK_WINDOW(win_), "MyPersonalDownloadManager");
    gtk_window_set_default_size(GTK_WINDOW(win_), 1100, 720);
    gtk_widget_set_size_request(win_, 360, 420);
    gtk_window_set_icon_name(GTK_WINDOW(win_), "mpdm");
    // Closing the window keeps downloads running; the app lives on in the background.
    gtk_window_set_hide_on_close(GTK_WINDOW(win_), TRUE);
    on_r<gboolean>(win_, "close-request", [this]() -> gboolean {
        if (!app_.prefs().background_hint_shown) {
            app_.prefs().background_hint_shown = true;
            app_.prefs().save();
            GNotification* n = g_notification_new("MPDM is still running");
            g_notification_set_body(n, "Downloads continue in the background. Open MPDM again to see them, "
                                       "or choose Quit from the menu to stop.");
            g_application_send_notification(G_APPLICATION(app_.gtk_app()), "background", n);
            g_object_unref(n);
        }
        return FALSE;
    });

    GtkWidget* sidebar_page = nullptr;
    GtkWidget* content_page = nullptr;
    build_sidebar(sidebar_page);
    build_content(content_page);

    split_ = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_sidebar(split_, ADW_NAVIGATION_PAGE(sidebar_page));
    adw_navigation_split_view_set_content(split_, ADW_NAVIGATION_PAGE(content_page));
    adw_navigation_split_view_set_min_sidebar_width(split_, 220);
    adw_navigation_split_view_set_max_sidebar_width(split_, 270);

    toasts_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(toasts_, GTK_WIDGET(split_));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win_), GTK_WIDGET(toasts_));

    // Narrow windows: collapse to a single pane (sidebar, then content on select).
    AdwBreakpoint* bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 720sp"));
    GValue t = G_VALUE_INIT;
    g_value_init(&t, G_TYPE_BOOLEAN);
    g_value_set_boolean(&t, TRUE);
    adw_breakpoint_add_setter(bp, G_OBJECT(split_), "collapsed", &t);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(win_), bp);
    adw_navigation_split_view_set_show_content(split_, TRUE);
}

void MainWindow::build_sidebar(GtkWidget*& page) {
    GtkWidget* tv = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("MPDM", ""));
    auto* menu_btn = Gtk::make_managed<Gtk::MenuButton>();
    menu_btn->set_icon_name("open-menu-symbolic");
    menu_btn->set_menu_model(main_menu());
    menu_btn->set_primary(true);
    menu_btn->set_tooltip_text("Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(menu_btn->gobj()));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

    sidebar_ = Gtk::make_managed<Gtk::ListBox>();
    sidebar_->add_css_class("navigation-sidebar");
    sidebar_->set_selection_mode(Gtk::SelectionMode::SINGLE);
    sidebar_->signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        for (auto& f : filters_)
            if (f.row == row) { filter_ = f.key; break; }
        refresh();
    });
    sidebar_->signal_row_activated().connect([this](Gtk::ListBoxRow*) {
        adw_navigation_split_view_set_show_content(split_, TRUE);
    });
    sidebar_->set_header_func([this](Gtk::ListBoxRow* row, Gtk::ListBoxRow* before) {
        bool first_cat = false;
        for (size_t i = 0; i < filters_.size(); ++i)
            if (filters_[i].row == row) first_cat = filters_[i].category && (i == 0 || !filters_[i - 1].category);
        if (!first_cat || !before) { row->unset_header(); return; }
        auto* l = Gtk::make_managed<Gtk::Label>("Categories");
        l->set_xalign(0);
        l->add_css_class("caption-heading");
        l->add_css_class("dim-label");
        l->set_margin_start(12);
        l->set_margin_top(14);
        l->set_margin_bottom(6);
        row->set_header(*l);
    });
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(*sidebar_);
    scroll->set_vexpand(true);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(scroll->gobj()));

    // Storage footer: how full the download drive is.
    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    foot->add_css_class("sidebar-footer");
    auto* cap = Gtk::make_managed<Gtk::Label>("Storage");
    cap->set_xalign(0);
    cap->add_css_class("caption-heading");
    cap->add_css_class("dim-label");
    storage_bar_ = Gtk::make_managed<Gtk::LevelBar>();
    storage_bar_->add_css_class("storage");
    storage_bar_->set_min_value(0);
    storage_bar_->set_max_value(1);
    storage_bar_->remove_offset_value(GTK_LEVEL_BAR_OFFSET_LOW);
    storage_bar_->remove_offset_value(GTK_LEVEL_BAR_OFFSET_HIGH);
    storage_bar_->remove_offset_value(GTK_LEVEL_BAR_OFFSET_FULL);
    storage_label_ = Gtk::make_managed<Gtk::Label>("");
    storage_label_->set_xalign(0);
    storage_label_->add_css_class("caption");
    storage_label_->add_css_class("dim-label");
    storage_label_->add_css_class("numeric");
    storage_label_->set_ellipsize(Pango::EllipsizeMode::END);
    foot->append(*cap);
    foot->append(*storage_bar_);
    foot->append(*storage_label_);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(foot->gobj()));

    page = GTK_WIDGET(adw_navigation_page_new(tv, "MPDM"));
    rebuild_filters();
}

void MainWindow::rebuild_filters() {
    std::string keep = filter_;
    for (auto& f : filters_) sidebar_->remove(*f.row);
    filters_.clear();
    auto add = [&](const std::string& key, const std::string& title, const std::string& icon, bool cat) {
        Filter f{key, title, icon, cat};
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        box->set_margin_top(2);
        box->set_margin_bottom(2);
        auto* img = Gtk::make_managed<Gtk::Image>();
        img->set_from_icon_name(icon);
        auto* lbl = Gtk::make_managed<Gtk::Label>(title);
        lbl->set_xalign(0);
        lbl->set_hexpand(true);
        lbl->set_ellipsize(Pango::EllipsizeMode::END);
        f.count = Gtk::make_managed<Gtk::Label>("");
        f.count->add_css_class("sidebar-count");
        f.count->add_css_class("numeric");
        box->append(*img);
        box->append(*lbl);
        box->append(*f.count);
        f.row = Gtk::make_managed<Gtk::ListBoxRow>();
        f.row->set_child(*box);
        sidebar_->append(*f.row);
        filters_.push_back(f);
    };
    add("all", "All Downloads", "folder-download-symbolic", false);
    add(ST_DOWNLOADING, "Downloading", "media-playback-start-symbolic", false);
    add(ST_QUEUED, "Queued", "view-list-symbolic", false);
    add(ST_PAUSED, "Paused", "media-playback-pause-symbolic", false);
    add(ST_COMPLETED, "Completed", "object-select-symbolic", false);
    add(ST_ERROR, "Failed", "dialog-error-symbolic", false);
    for (const char* c : kBuiltinCats) add(std::string("cat:") + c, c, category_icon(c), true);
    for (auto& c : custom_cats_) add("cat:" + c, c, "folder-symbolic", true);
    add("private", "Private", "view-conceal-symbolic", true);
    sidebar_->invalidate_headers();

    Gtk::ListBoxRow* sel = filters_.front().row;
    for (auto& f : filters_)
        if (f.key == keep) sel = f.row;
    sidebar_->select_row(*sel);
}

void MainWindow::build_content(GtkWidget*& page) {
    GtkWidget* tv = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    content_title_ = ADW_WINDOW_TITLE(adw_window_title_new("All Downloads", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(content_title_));

    auto* add_btn = Gtk::make_managed<Gtk::Button>();
    add_btn->set_icon_name("list-add-symbolic");
    add_btn->set_tooltip_text("Add Downloads (Ctrl+N)");
    add_btn->set_action_name("app.add");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), GTK_WIDGET(add_btn->gobj()));

    limit_btn_ = Gtk::make_managed<Gtk::MenuButton>();
    limit_btn_->set_icon_name("power-profile-performance-symbolic");  // a speedometer
    limit_btn_->set_menu_model(limit_menu());
    limit_btn_->set_tooltip_text("Speed Limit");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(limit_btn_->gobj()));

    auto* search_toggle = Gtk::make_managed<Gtk::ToggleButton>();
    search_toggle->set_icon_name("system-search-symbolic");
    search_toggle->set_tooltip_text("Search (Ctrl+F)");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), GTK_WIDGET(search_toggle->gobj()));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

    search_entry_ = Gtk::make_managed<Gtk::SearchEntry>();
    search_entry_->set_placeholder_text("Search downloads");
    search_entry_->set_hexpand(true);
    auto* sclamp = W(adw_clamp_new());
    adw_clamp_set_maximum_size(ADW_CLAMP(sclamp->gobj()), 520);
    adw_clamp_set_child(ADW_CLAMP(sclamp->gobj()), GTK_WIDGET(search_entry_->gobj()));
    search_bar_ = Gtk::make_managed<Gtk::SearchBar>();
    search_bar_->set_child(*sclamp);
    search_bar_->connect_entry(*search_entry_);
    gtk_search_bar_set_key_capture_widget(search_bar_->gobj(), win_);  // type anywhere to search
    Glib::Binding::bind_property(search_toggle->property_active(), search_bar_->property_search_mode_enabled(),
                                 Glib::Binding::Flags::BIDIRECTIONAL);
    search_entry_->signal_search_changed().connect([this] {
        query_ = search_entry_->get_text();
        refresh();
    });
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(search_bar_->gobj()));

    stack_ = Gtk::make_managed<Gtk::Stack>();
    stack_->set_transition_type(Gtk::StackTransitionType::CROSSFADE);

    // empty state
    GtkWidget* status = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "folder-download-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(status), "No Downloads Yet");
    adw_status_page_set_description(ADW_STATUS_PAGE(status),
        "Paste a link to get started, or send one from your browser with the MPDM extension.");
    auto* start_btn = Gtk::make_managed<Gtk::Button>("_Add Download", true);
    start_btn->add_css_class("pill");
    start_btn->add_css_class("suggested-action");
    start_btn->set_halign(Gtk::Align::CENTER);
    start_btn->set_action_name("app.add");
    adw_status_page_set_child(ADW_STATUS_PAGE(status), GTK_WIDGET(start_btn->gobj()));
    stack_->add(*W(status), "empty");

    // list
    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    col->set_margin_top(18);
    col->set_margin_bottom(24);
    col->set_margin_start(12);
    col->set_margin_end(12);
    col->append(*build_overview());

    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    head->set_margin_top(10);
    head->set_margin_start(4);
    list_title_ = Gtk::make_managed<Gtk::Label>("All Downloads");
    list_title_->add_css_class("heading");
    list_count_ = Gtk::make_managed<Gtk::Label>("");
    list_count_->add_css_class("dim-label");
    list_count_->add_css_class("numeric");
    head->append(*list_title_);
    head->append(*list_count_);
    col->append(*head);

    list_ = Gtk::make_managed<Gtk::ListBox>();
    list_->add_css_class("boxed-list");
    list_->set_selection_mode(Gtk::SelectionMode::NONE);
    list_->set_sort_func([this](Gtk::ListBoxRow* a, Gtk::ListBoxRow* b) {
        auto pa = pos_.find(a), pb = pos_.find(b);
        return (pa == pos_.end() ? 0 : pa->second) - (pb == pos_.end() ? 0 : pb->second);
    });
    list_->signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        for (auto& [id, r] : rows_)
            if (r->row == row) { app_.open_download_window(id); break; }
    });
    col->append(*list_);

    no_match_ = Gtk::make_managed<Gtk::Label>("Nothing here");
    no_match_->add_css_class("dim-label");
    no_match_->set_margin_top(32);
    col->append(*no_match_);

    auto* clamp = W(adw_clamp_new());
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp->gobj()), 980);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp->gobj()), 720);
    adw_clamp_set_child(ADW_CLAMP(clamp->gobj()), GTK_WIDGET(col->gobj()));
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_child(*clamp);
    stack_->add(*scroll, "list");

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(stack_->gobj()));
    page = GTK_WIDGET(adw_navigation_page_new(tv, "Downloads"));
}

Gtk::Widget* MainWindow::build_overview() {
    auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 28);
    card->add_css_class("card");
    card->add_css_class("overview");

    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    left->set_valign(Gtk::Align::CENTER);
    left->set_size_request(170, -1);
    auto* cap = Gtk::make_managed<Gtk::Label>("Download speed");
    cap->set_xalign(0);
    cap->add_css_class("caption-heading");
    cap->add_css_class("dim-label");
    auto* figure = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    speed_value_ = Gtk::make_managed<Gtk::Label>("0");
    speed_value_->add_css_class("speed-figure");
    speed_value_->add_css_class("numeric");
    speed_unit_ = Gtk::make_managed<Gtk::Label>("B/s");
    speed_unit_->add_css_class("speed-unit");
    speed_unit_->set_valign(Gtk::Align::END);
    figure->append(*speed_value_);
    figure->append(*speed_unit_);
    speed_sub_ = Gtk::make_managed<Gtk::Label>("Idle");
    speed_sub_->set_xalign(0);
    speed_sub_->add_css_class("caption");
    speed_sub_->add_css_class("dim-label");
    left->append(*cap);
    left->append(*figure);
    left->append(*speed_sub_);

    auto* right = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    right->set_hexpand(true);
    graph_ = Gtk::make_managed<ThroughputGraph>();
    right->append(*graph_);
    auto* legend = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto dot = [&](bool disk, const char* text) {
        auto* d = Gtk::make_managed<Gtk::Box>();
        d->add_css_class("legend-dot");
        if (disk) d->add_css_class("disk");
        d->set_valign(Gtk::Align::CENTER);
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->add_css_class("caption");
        l->add_css_class("dim-label");
        l->set_margin_end(10);
        legend->append(*d);
        legend->append(*l);
    };
    dot(false, "Network");
    dot(true, "Disk writes");
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    legend->append(*spacer);
    peak_label_ = Gtk::make_managed<Gtk::Label>("");
    peak_label_->add_css_class("caption");
    peak_label_->add_css_class("dim-label");
    peak_label_->add_css_class("numeric");
    legend->append(*peak_label_);
    right->append(*legend);

    card->append(*left);
    card->append(*right);
    return card;
}

// ---- download rows -------------------------------------------------------------

std::unique_ptr<MainWindow::Row> MainWindow::make_row(const std::shared_ptr<DownloadItem>& it) {
    auto r = std::make_unique<Row>();
    r->id = it->id;
    const std::string id = it->id;

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    box->add_css_class("dl-row");

    r->icon = Gtk::make_managed<Gtk::Image>();  // the image is the tinted tile itself
    r->icon->add_css_class("dl-icon");
    r->icon->set_pixel_size(18);
    r->icon->set_valign(Gtk::Align::CENTER);
    box->append(*r->icon);

    auto* mid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    mid->set_hexpand(true);
    mid->set_valign(Gtk::Align::CENTER);
    auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    r->name = Gtk::make_managed<Gtk::Label>("");
    r->name->add_css_class("dl-name");
    r->name->set_xalign(0);
    r->name->set_hexpand(true);
    r->name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    r->pill = Gtk::make_managed<Gtk::Label>("");
    r->pill->add_css_class("status-pill");
    r->pill->set_valign(Gtk::Align::CENTER);
    top->append(*r->name);
    top->append(*r->pill);
    r->bar = Gtk::make_managed<Gtk::ProgressBar>();
    r->bar->add_css_class("dl-progress");
    r->detail = Gtk::make_managed<Gtk::Label>("");
    r->detail->set_xalign(0);
    r->detail->add_css_class("caption");
    r->detail->add_css_class("dim-label");
    r->detail->add_css_class("numeric");
    r->detail->set_ellipsize(Pango::EllipsizeMode::END);
    mid->append(*top);
    mid->append(*r->bar);
    mid->append(*r->detail);
    box->append(*mid);

    r->primary = Gtk::make_managed<Gtk::Button>();
    r->primary->add_css_class("flat");
    r->primary->add_css_class("circular");
    r->primary->set_valign(Gtk::Align::CENTER);
    r->primary->signal_clicked().connect([this, id] { on_primary(id); });
    box->append(*r->primary);

    auto* more = Gtk::make_managed<Gtk::MenuButton>();
    more->set_icon_name("view-more-symbolic");
    more->add_css_class("flat");
    more->add_css_class("circular");
    more->set_valign(Gtk::Align::CENTER);
    more->set_tooltip_text("More Options");
    more->set_menu_model(row_menu());
    box->append(*more);

    r->actions = Gio::SimpleActionGroup::create();
    r->actions->add_action("details", [this, id] { app_.open_download_window(id); });
    r->actions->add_action("open", [this, id] { if (auto it = app_.item(id)) open_file(window(), it->dest_path); });
    r->actions->add_action("show", [this, id] { if (auto it = app_.item(id)) show_in_folder(window(), it->dest_path); });
    r->actions->add_action("copy", [this, id] {
        if (auto it = app_.item(id)) { copy_to_clipboard(it->url); app_.toast("Link copied"); }
    });
    r->actions->add_action("limit", [this, id] { app_.ask_item_limit(id); });
    r->actions->add_action("remove", [this, id] { app_.confirm_remove(id); });

    r->row = Gtk::make_managed<Gtk::ListBoxRow>();
    r->row->set_child(*box);
    r->row->set_activatable(true);
    r->row->insert_action_group("row", r->actions);
    return r;
}

void MainWindow::update_row(Row& r, const DownloadItem& it) {
    const std::string cls = status_class(it);
    if (cls != r.state_class) {
        for (Gtk::Widget* w : {(Gtk::Widget*)r.icon, (Gtk::Widget*)r.pill, (Gtk::Widget*)r.bar}) {
            if (!r.state_class.empty()) w->remove_css_class(r.state_class);
            w->add_css_class(cls);
        }
        r.state_class = cls;
    }
    const bool done = it.status == ST_COMPLETED, paused = it.status == ST_PAUSED,
               failed = it.status == ST_ERROR, queued = it.status == ST_QUEUED;

    r.name->set_text(display_name(it));
    r.name->set_tooltip_text(it.url);
    r.icon->set_from_icon_name(it.incognito ? "view-conceal-symbolic"
                               : category_icon(item_category(it, app_.manager().settings.category_exts)));
    r.pill->set_text(it.incognito && it.status == ST_DOWNLOADING ? "Private" : status_title(it.status));
    r.pill->set_visible(it.status != ST_DOWNLOADING || it.incognito);

    auto pct = it.percent();
    double frac = pct ? *pct / 100.0 : (done ? 1.0 : 0.0);
    r.bar->set_fraction(frac);
    r.bar->set_visible(!done);

    std::string of = human_size((double)it.downloaded) +
                     (it.size && !it.stream ? " of " + human_size((double)*it.size) : std::string());
    std::string detail;
    if (failed) {
        detail = it.error.empty() ? "Download failed" : it.error;
    } else if (done) {
        detail = human_size((double)(it.size ? *it.size : it.downloaded)) + "  ·  " + url_host(it.url);
    } else if (paused) {
        detail = "Paused at " + of;
    } else if (queued) {
        detail = "Waiting for a free slot  ·  " + url_host(it.url);
    } else if (it.stream) {
        detail = it.seg_total ? "Part " + std::to_string(it.seg_done) + " of " + std::to_string(it.seg_total)
                              : std::string("Reading the playlist…");
        if (it.speed > 0) detail += "  ·  " + human_speed(it.speed);
    } else {
        detail = (pct ? std::to_string((int)*pct) + "%  ·  " : std::string()) + of;
        if (it.speed > 0) detail += "  ·  " + human_speed(it.speed);
        if (it.eta && *it.eta > 0) detail += "  ·  " + human_eta(*it.eta);
    }
    r.detail->set_text(detail);
    if (failed) {
        r.detail->remove_css_class("dim-label");
        r.detail->add_css_class("error");
    } else {
        r.detail->remove_css_class("error");
        r.detail->add_css_class("dim-label");
    }

    const char* icon = "media-playback-pause-symbolic";
    const char* tip = "Pause";
    if (done) { icon = "folder-open-symbolic"; tip = "Show in Folder"; }
    else if (failed) { icon = "view-refresh-symbolic"; tip = "Retry"; }
    else if (paused) { icon = "media-playback-start-symbolic"; tip = "Resume"; }
    else if (it.stream) { icon = "process-stop-symbolic"; tip = "Stop"; }
    r.primary->set_icon_name(icon);
    r.primary->set_tooltip_text(tip);

    if (auto a = std::dynamic_pointer_cast<Gio::SimpleAction>(r.actions->lookup_action("open")))
        a->set_enabled(done);
}

void MainWindow::on_primary(const std::string& id) {
    auto it = app_.item(id);
    if (!it) return;
    auto& m = app_.manager();
    if (it->status == ST_COMPLETED) show_in_folder(window(), it->dest_path);
    else if (it->status == ST_PAUSED || it->status == ST_ERROR) m.resume(id);
    else if (it->stream) m.remove(id, false);  // stream grabs can't pause
    else m.pause(id);
    refresh();
}

bool MainWindow::matches(const DownloadItem& it, const std::string& f) const {
    if (f == "all") return true;
    if (f == "private") return it.incognito;
    if (f.rfind("cat:", 0) == 0)
        return !it.incognito && item_category(it, app_.manager().settings.category_exts) == f.substr(4);
    return it.status == f;
}

// ---- periodic refresh ------------------------------------------------------------

void MainWindow::refresh() {
    if (!list_) return;  // the sidebar selects its first row while the content pane is still being built
    auto& m = app_.manager();
    auto items = m.ordered();

    // Custom categories come and go with Preferences.
    std::vector<std::string> custom;
    for (auto& [name, exts] : m.settings.category_exts)
        if (std::find(std::begin(kBuiltinCats), std::end(kBuiltinCats), name) == std::end(kBuiltinCats))
            custom.push_back(name);
    if (custom != custom_cats_) { custom_cats_ = custom; rebuild_filters(); }

    // sidebar counts
    for (auto& f : filters_) {
        int n = 0;
        for (auto& it : items) if (matches(*it, f.key)) ++n;
        f.count->set_text(n ? std::to_string(n) : "");
    }

    // rows: add new, drop removed, keep the manager's order
    std::set<std::string> live;
    std::vector<std::string> order;
    for (auto& it : items) { live.insert(it->id); order.push_back(it->id); }
    for (auto i = rows_.begin(); i != rows_.end();) {
        if (live.count(i->first)) { ++i; continue; }
        pos_.erase(i->second->row);
        list_->remove(*i->second->row);
        i = rows_.erase(i);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        auto& it = items[i];
        auto found = rows_.find(it->id);
        if (found == rows_.end()) {
            auto r = make_row(it);
            pos_[r->row] = (int)i;
            list_->append(*r->row);
            found = rows_.emplace(it->id, std::move(r)).first;
        }
        pos_[found->second->row] = (int)i;
        update_row(*found->second, *it);
    }
    if (order != order_) { order_ = order; list_->invalidate_sort(); }

    // filter + search
    std::string q = query_;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    int visible = 0;
    for (auto& it : items) {
        bool show = matches(*it, filter_);
        if (show && !q.empty()) {
            std::string hay = display_name(*it) + " " + it->url;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            show = hay.find(q) != std::string::npos;
        }
        rows_[it->id]->row->set_visible(show);
        visible += show;
    }
    std::string title = "All Downloads";
    for (auto& f : filters_) if (f.key == filter_) title = f.title;
    list_title_->set_text(title);
    list_count_->set_text(std::to_string(visible));
    list_->set_visible(visible > 0);
    no_match_->set_visible(visible == 0);
    no_match_->set_text(q.empty() ? "Nothing in “" + title + "”" : "No downloads match “" + query_ + "”");
    stack_->set_visible_child(items.empty() ? "empty" : "list");

    update_overview(items);
}

void MainWindow::update_overview(const std::vector<std::shared_ptr<DownloadItem>>& items) {
    auto& m = app_.manager();
    double speed = 0;
    int active = 0;
    long long written = 0;
    for (auto& it : items) {
        if (it->status == ST_DOWNLOADING) { speed += it->speed; ++active; }
        written += it->downloaded;
    }

    // disk write rate = bytes committed since the previous tick
    double now = mono_seconds(), disk = 0;
    if (last_disk_bytes_ >= 0 && now > last_disk_t_ && written > last_disk_bytes_)
        disk = (double)(written - last_disk_bytes_) / (now - last_disk_t_);
    last_disk_bytes_ = written;
    last_disk_t_ = now;
    if (active == 0) disk = 0;
    graph_->push(speed, disk);

    std::string hs = human_speed(speed);
    auto sp = hs.find(' ');
    speed_value_->set_text(hs.substr(0, sp));
    speed_unit_->set_text(sp == std::string::npos ? "" : hs.substr(sp + 1));
    speed_sub_->set_text(active ? std::to_string(active) + " of " + std::to_string(m.max_concurrent) + " slots active"
                                : "Idle");
    peak_label_->set_text(graph_->peak() > 0 ? "Peak " + human_speed(graph_->peak()) : "");

    std::string sub;
    if (active) sub = std::to_string(active) + " downloading  ·  " + hs;
    else if (!items.empty()) sub = std::to_string(items.size()) + (items.size() == 1 ? " item" : " items");
    double limit = m.global_rate_bps();
    if (limit > 0) sub += "  ·  limited to " + human_speed(limit);
    adw_window_title_set_subtitle(content_title_, sub.c_str());
    std::string title = "All Downloads";
    for (auto& f : filters_) if (f.key == filter_) title = f.title;
    adw_window_title_set_title(content_title_, title.c_str());
    if (limit > 0) limit_btn_->add_css_class("accent");
    else limit_btn_->remove_css_class("accent");
    limit_btn_->set_tooltip_text(limit > 0 ? "Speed Limit: " + human_speed(limit) : "Speed Limit: Unlimited");

    std::error_code ec;
    auto space = fs::space(m.settings.output_dir, ec);
    if (!ec && space.capacity > 0) {
        storage_bar_->set_value((double)(space.capacity - space.available) / (double)space.capacity);
        storage_label_->set_text(human_size((double)space.available) + " free of " + human_size((double)space.capacity));
    } else {
        storage_label_->set_text("Download folder unavailable");
    }
}

void MainWindow::toggle_search() {
    search_bar_->set_search_mode(!search_bar_->get_search_mode());
    if (search_bar_->get_search_mode()) search_entry_->grab_focus();
}

void MainWindow::add_toast(AdwToast* toast) { adw_toast_overlay_add_toast(toasts_, toast); }

void MainWindow::seed_graph() {
    for (int i = 0; i < 60; ++i) {
        double t = i / 59.0;
        double net = i < 5 ? 0.0 : 8.0e6 + 3.0e6 * std::sin(t * 6.2832 * 2.1) + (i > 22 && i < 52 ? 2.2e6 : 0);
        double disk = net > 0 ? std::max(0.0, net * 0.86 + 6.0e5 * std::sin(t * 6.2832 * 3.4)) : 0.0;
        graph_->push(net, disk);
    }
}

} // namespace mpdm::gtk
