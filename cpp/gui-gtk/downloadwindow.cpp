#include "downloadwindow.hpp"

#include "app.hpp"
#include "charts.hpp"
#include "util.hpp"

namespace mpdm::gtk {

DownloadWindow::DownloadWindow(App& app, const std::string& id) : app_(app), id_(id) {
    auto it = app_.item(id);
    stream_ = it && it->stream;

    win_ = adw_window_new();
    gtk_window_set_application(GTK_WINDOW(win_), app_.gtk_app());
    gtk_window_set_default_size(GTK_WINDOW(win_), 480, -1);
    gtk_window_set_title(GTK_WINDOW(win_), it ? display_name(*it).c_str() : "Download");
    gtk_window_set_icon_name(GTK_WINDOW(win_), "mpdm");
    g_object_add_weak_pointer(G_OBJECT(win_), reinterpret_cast<gpointer*>(&win_));
    // Closing only closes the view; the download keeps running in the app.
    on_r<gboolean>(win_, "close-request", [this]() -> gboolean {
        app_.close_download_window(id_);
        return FALSE;
    });

    if (it) build(*it);
    tick();
    timer_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &DownloadWindow::tick), 500);
}

DownloadWindow::~DownloadWindow() {
    timer_.disconnect();
    if (win_) {
        GtkWidget* w = win_;
        g_object_remove_weak_pointer(G_OBJECT(w), reinterpret_cast<gpointer*>(&win_));
        win_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(w));
    }
}

void DownloadWindow::present() {
    if (win_) gtk_window_present(GTK_WINDOW(win_));
}

bool DownloadWindow::visible() const { return win_ && gtk_widget_get_visible(win_); }

static Gtk::Label* dim_label(const std::string& text, const char* extra = nullptr) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_xalign(0);
    l->add_css_class("dim-label");
    if (extra) l->add_css_class(extra);
    return l;
}

static GtkWidget* boxed_list() {
    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->add_css_class("boxed-list");
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    return GTK_WIDGET(list->gobj());
}

GtkWidget* DownloadWindow::property_row(GtkWidget* list, const char* title, Gtk::Label*& value) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    value = Gtk::make_managed<Gtk::Label>("—");
    value->add_css_class("dim-label");
    value->add_css_class("numeric");
    value->set_xalign(1);
    value->set_ellipsize(Pango::EllipsizeMode::START);
    value->set_max_width_chars(30);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(value->gobj()));
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

GtkWidget* DownloadWindow::step_row(GtkWidget* list, const char* title, const char* subtitle) {
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    auto* stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    auto* pending = Gtk::make_managed<Gtk::Image>();
    pending->set_from_icon_name("content-loading-symbolic");
    pending->add_css_class("dim-label");
    auto* done = Gtk::make_managed<Gtk::Image>();
    done->set_from_icon_name("object-select-symbolic");
    done->add_css_class("success");
    stack->add(*pending, "pending");
    stack->add(*W(adw_spinner_new()), "active");
    stack->add(*done, "done");
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), GTK_WIDGET(stack->gobj()));
    g_object_set_data(G_OBJECT(row), "mpdm-step", stack->gobj());
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

void DownloadWindow::set_step(GtkWidget* row, bool active, bool complete) {
    auto* stack = static_cast<GtkStack*>(g_object_get_data(G_OBJECT(row), "mpdm-step"));
    gtk_stack_set_visible_child_name(stack, complete ? "done" : active ? "active" : "pending");
}

void DownloadWindow::build(const DownloadItem& it) {
    GtkWidget* tv = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    title_ = ADW_WINDOW_TITLE(adw_window_title_new("Downloading", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(title_));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);

    auto* col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 18);
    col->set_margin_top(6);
    col->set_margin_bottom(18);
    col->set_margin_start(18);
    col->set_margin_end(18);

    // hero: file icon + name + source
    auto* hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    icon_ = Gtk::make_managed<Gtk::Image>();
    icon_->add_css_class("dl-icon");
    icon_->add_css_class("large");
    icon_->set_pixel_size(26);
    icon_->set_valign(Gtk::Align::CENTER);
    auto* names = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
    names->set_valign(Gtk::Align::CENTER);
    names->set_hexpand(true);
    name_ = Gtk::make_managed<Gtk::Label>("");
    name_->add_css_class("hero-name");
    name_->set_xalign(0);
    name_->set_wrap(true);
    name_->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    name_->set_lines(2);
    name_->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    source_ = dim_label("", "caption");
    source_->set_ellipsize(Pango::EllipsizeMode::END);
    names->append(*name_);
    names->append(*source_);
    hero->append(*icon_);
    hero->append(*names);
    col->append(*hero);

    // progress
    auto* prog = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    bar_ = Gtk::make_managed<Gtk::ProgressBar>();
    bar_->add_css_class("dl-progress");
    bar_->add_css_class("large");
    auto* nums = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    amount_ = dim_label("", "numeric");
    amount_->set_hexpand(true);
    amount_->set_valign(Gtk::Align::END);
    percent_ = Gtk::make_managed<Gtk::Label>("0%");
    percent_->add_css_class("percent-figure");
    percent_->add_css_class("numeric");
    nums->append(*amount_);
    nums->append(*percent_);
    prog->append(*bar_);
    prog->append(*nums);
    error_ = Gtk::make_managed<Gtk::Label>("");
    error_->add_css_class("error");
    error_->set_xalign(0);
    error_->set_wrap(true);
    error_->set_visible(false);
    prog->append(*error_);
    col->append(*prog);

    if (stream_) {
        GtkWidget* steps = boxed_list();
        step1_ = step_row(steps, "Read the playlist", "Picks the best available quality");
        step2_ = step_row(steps, "Download the parts", "Decrypts AES-128 segments when needed");
        step3_ = step_row(steps, "Join into one MP4", "Remuxed with ffmpeg, no re-encoding");
        col->append(*W(steps));
    } else {
        GtkWidget* props = boxed_list();
        speed_row_ = property_row(props, "Speed", v_speed_);
        property_row(props, "Time left", v_eta_);
        GtkWidget* folder_row = folder_row_ = property_row(props, "Saving to", v_folder_);
        auto* folder_btn = Gtk::make_managed<Gtk::Button>();
        folder_btn->set_icon_name("folder-open-symbolic");
        folder_btn->add_css_class("flat");
        folder_btn->set_valign(Gtk::Align::CENTER);
        folder_btn->set_tooltip_text("Show in Folder");
        folder_btn->signal_clicked().connect([this] {
            if (auto it = app_.item(id_)) show_in_folder(window(), it->dest_path);
        });
        adw_action_row_add_suffix(ADW_ACTION_ROW(folder_row), GTK_WIDGET(folder_btn->gobj()));
        limit_row_ = property_row(props, "Speed limit", v_limit_);
        property_row(props, "Resume", v_resume_);
        col->append(*W(props));

        auto* lanes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        auto* lh = Gtk::make_managed<Gtk::Label>("Connections");
        lh->add_css_class("heading");
        lh->set_xalign(0);
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        card->add_css_class("card");
        card->add_css_class("lanes-card");
        lanes_ = Gtk::make_managed<ConnectionLanes>();
        lanes_note_ = dim_label("", "caption");
        lanes_note_->set_wrap(true);
        card->append(*lanes_);
        card->append(*lanes_note_);
        lanes->append(*lh);
        lanes->append(*card);
        lanes_group_ = lanes;
        col->append(*lanes);
    }

    GtkWidget* opts = boxed_list();
    open_when_done_ = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(open_when_done_), "Show in folder when finished");
    gtk_list_box_append(GTK_LIST_BOX(opts), open_when_done_);
    col->append(*W(opts));

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_propagate_natural_height(true);
    scroll->set_max_content_height(640);
    scroll->set_child(*col);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(scroll->gobj()));

    // footer actions
    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    foot->set_margin_top(12);
    foot->set_margin_bottom(12);
    foot->set_margin_start(12);
    foot->set_margin_end(12);
    auto button = [&](const char* label, const char* css) {
        auto* b = Gtk::make_managed<Gtk::Button>(label, true);
        if (css) b->add_css_class(css);
        return b;
    };
    cancel_ = button("_Cancel Download", "destructive-action");
    cancel_->add_css_class("flat");
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    pause_ = button("_Pause", nullptr);
    retry_ = button("_Retry", "suggested-action");
    show_ = button("_Show in Folder", nullptr);
    open_ = button("_Open", "suggested-action");
    for (Gtk::Widget* w : {(Gtk::Widget*)cancel_, (Gtk::Widget*)spacer, (Gtk::Widget*)pause_,
                           (Gtk::Widget*)retry_, (Gtk::Widget*)show_, (Gtk::Widget*)open_})
        foot->append(*w);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(foot->gobj()));

    pause_->signal_clicked().connect([this] {
        auto it = app_.item(id_);
        if (!it) return;
        if (it->status == ST_PAUSED) app_.manager().resume(id_);
        else app_.manager().pause(id_);
        tick();
    });
    cancel_->signal_clicked().connect([this] {
        app_.manager().remove(id_, /*delete_file=*/false);  // abort + drop from queue, keep the partial
        app_.close_download_window(id_);
        if (win_) gtk_widget_set_visible(win_, FALSE);
        app_.refresh();
    });
    retry_->signal_clicked().connect([this] { app_.manager().resume(id_); tick(); });
    show_->signal_clicked().connect([this] {
        if (auto it = app_.item(id_)) show_in_folder(window(), it->dest_path);
    });
    open_->signal_clicked().connect([this] {
        if (auto it = app_.item(id_)) open_file(window(), it->dest_path);
        gtk_window_close(window());
    });

    adw_window_set_content(ADW_WINDOW(win_), tv);
}

void DownloadWindow::set_state_class(const std::string& cls) {
    if (cls == state_class_) return;
    for (Gtk::Widget* w : {(Gtk::Widget*)icon_, (Gtk::Widget*)bar_}) {
        if (!state_class_.empty()) w->remove_css_class(state_class_);
        w->add_css_class(cls);
    }
    state_class_ = cls;
}

bool DownloadWindow::tick() {
    if (!win_ || !title_) return false;
    auto it = app_.item(id_);
    if (!it) {  // removed from the queue elsewhere -> close the view
        app_.close_download_window(id_);
        return false;
    }
    const bool done = it->status == ST_COMPLETED, failed = it->status == ST_ERROR,
               paused = it->status == ST_PAUSED, queued = it->status == ST_QUEUED;
    const bool running = !done && !failed && !paused;
    auto pct = it->percent();
    int p = pct ? (int)(*pct + 0.5) : (done ? 100 : 0);

    set_state_class(done ? "completed" : failed ? "error" : paused ? "paused" : queued ? "queued" : "downloading");
    const char* title = done     ? "Download Complete"
                        : failed ? "Download Failed"
                        : paused ? "Paused"
                        : queued ? "Queued"
                        : stream_ ? "Grabbing Stream"
                                  : "Downloading";
    adw_window_title_set_title(title_, title);
    adw_window_title_set_subtitle(title_, running && !queued ? (std::to_string(p) + "%").c_str() : "");

    name_->set_text(display_name(*it));
    gtk_window_set_title(window(), display_name(*it).c_str());
    std::string host = url_host(it->url);
    source_->set_text(stream_ ? "HLS/DASH stream  ·  " + host : host);
    icon_->set_from_icon_name(category_icon(item_category(*it, app_.manager().settings.category_exts)));

    bar_->set_fraction(pct ? *pct / 100.0 : (done ? 1.0 : 0.0));
    percent_->set_text(std::to_string(p) + "%");
    if (stream_) {
        amount_->set_text(it->seg_total ? "Part " + std::to_string(it->seg_done) + " of " + std::to_string(it->seg_total)
                                        : std::string(done ? "Saved" : "Reading the playlist…"));
        set_step(step1_, it->seg_total == 0 && running, it->seg_total > 0 || done);
        set_step(step2_, it->seg_total > 0 && running, done);
        set_step(step3_, false, done);
    } else {
        amount_->set_text(human_size((double)it->downloaded) + " of " +
                          (it->size ? human_size((double)*it->size) : std::string("unknown size")));
        v_speed_->set_text(it->status == ST_DOWNLOADING && it->speed > 0 ? human_speed(it->speed) : "—");
        std::string eta = done ? "Finished" : (it->status == ST_DOWNLOADING && it->eta) ? human_eta(*it->eta) : "—";
        v_eta_->set_text(eta.empty() ? "—" : eta);
        v_folder_->set_text(it->dest_path.empty() ? app_.manager().settings.output_dir : parent_dir(it->dest_path));
        double glob = app_.manager().global_rate_bps();
        v_limit_->set_text(it->max_rate_bps > 0 ? human_speed((double)it->max_rate_bps)
                           : glob > 0          ? human_speed(glob) + " (global)"
                                               : "Unlimited");
        v_resume_->set_text(done ? "Complete" : paused ? "Progress saved" : "Supported");
        gtk_widget_set_visible(speed_row_, !done);  // only the outcome matters once it's done
        gtk_widget_set_visible(limit_row_, !done);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row_), done ? "Saved to" : "Saving to");
        lanes_->set_data(it->conn_stats);
        lanes_group_->set_visible(!done && !it->conn_stats.empty());
        const SegmentProgress* stolen = nullptr;
        for (auto& c : it->conn_stats)
            if (c.stole_from >= 0) { stolen = &c; break; }
        lanes_note_->set_visible(stolen != nullptr);
        if (stolen)
            lanes_note_->set_text("C" + std::to_string(stolen->index + 1) + " finished its own range early and took over "
                                  "part of C" + std::to_string(stolen->stole_from + 1) + "’s remainder.");
    }
    error_->set_visible(failed);
    error_->set_text(it->error.empty() ? "The download stopped with an error." : it->error);

    pause_->set_visible((running && !stream_) || paused);
    pause_->set_label(paused ? "_Resume" : "_Pause");
    if (paused) pause_->add_css_class("suggested-action");
    else pause_->remove_css_class("suggested-action");
    cancel_->set_visible(!done);
    cancel_->set_label(failed ? "_Remove" : "_Cancel Download");
    retry_->set_visible(failed);
    show_->set_visible(done);
    open_->set_visible(done);

    if (done && !opened_folder_ && adw_switch_row_get_active(ADW_SWITCH_ROW(open_when_done_))) {
        opened_folder_ = true;
        show_in_folder(window(), it->dest_path);
    }
    return true;
}

} // namespace mpdm::gtk
