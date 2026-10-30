#include "app.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>

#include <unistd.h>

#include "downloadwindow.hpp"
#include "mainwindow.hpp"
#include "mpdm/ratelimit.hpp"
#include "style.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace mpdm::gtk {

// The global-limit presets offered in the header bar menu (state = parse_rate input).
static const char* kLimitPresets[] = {"0", "500k", "1m", "2m", "5m"};

App::App(ShotRequest shot) : shot_(std::move(shot)) {
    auto flags = Gio::Application::Flags::HANDLES_COMMAND_LINE;
    // Screenshot runs must never hand off to (or be mistaken for) the real instance.
    if (!shot_.kind.empty()) flags |= Gio::Application::Flags::NON_UNIQUE;
    app_ = Gtk::Application::create(APP_ID, flags);
    app_->signal_startup().connect(sigc::mem_fun(*this, &App::on_startup));
    app_->signal_command_line().connect(sigc::mem_fun(*this, &App::on_command_line), false);
}

App::~App() {
    popups_.clear();
    main_.reset();
    if (manager_) manager_->shutdown();
}

int App::run(int argc, char** argv) { return app_->run(argc, argv); }

std::string App::exe_path() const {
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("mpdm_gtk") : p.string();
}

std::shared_ptr<DownloadItem> App::item(const std::string& id) {
    for (auto& it : manager_->ordered())
        if (it->id == id) return it;
    return nullptr;
}

GtkWindow* App::main_window() const { return main_ ? main_->window() : nullptr; }
GtkWidget* App::dialog_parent() const { return main_ ? GTK_WIDGET(main_->window()) : nullptr; }

// ---- lifecycle ---------------------------------------------------------------

void App::on_startup() {
    adw_init();
    main_thread_ = std::this_thread::get_id();
    prefs_ = UiPrefs::load();
    apply_appearance();
    install_css();

    manager_ = std::make_unique<DownloadManager>();
    manager_->start_scheduler();
    // "Ask each time" duplicate policy: the engine asks from a worker thread.
    manager_->set_duplicate_resolver([this](const std::string& path) { return ask_duplicate_blocking(path); });
    // Only announce completions that happen while the app is running.
    for (auto& it : manager_->ordered())
        if (it->status == ST_COMPLETED) finish_announced_.insert(it->id);

    setup_actions();
    // Stay alive with the window hidden: closing it keeps downloads running.
    app_->hold();

    Glib::signal_timeout().connect([this] { refresh(); return !quitting_; }, 1000);
    Glib::signal_timeout().connect([this] { poll_jobs(); return !quitting_; }, 500);
}

int App::on_command_line(const Glib::RefPtr<Gio::ApplicationCommandLine>& cl) {
    int argc = 0;
    char** argv = g_application_command_line_get_arguments(cl->gobj(), &argc);
    bool background = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--background") background = true;
    g_strfreev(argv);

    if (!main_) {
        main_ = std::make_unique<MainWindow>(*this);
        if (shot_.demo) seed_demo();
        refresh();
    }
    if (!shot_.kind.empty()) { run_shot(); return 0; }
    // --background: launched at login or by the browser host; stay hidden and
    // just adopt any queued browser jobs (which open their own progress windows).
    if (!background || started_) present_main();
    started_ = true;
    poll_jobs();
    return 0;
}

void App::present_main() {
    if (main_) gtk_window_present(main_->window());
}

void App::quit() {
    quitting_ = true;
    popups_.clear();
    if (main_) gtk_window_destroy(main_->window());
    app_->release();
    app_->quit();
}

void App::apply_appearance() {
    AdwColorScheme scheme = prefs_.appearance == "light"  ? ADW_COLOR_SCHEME_FORCE_LIGHT
                            : prefs_.appearance == "dark" ? ADW_COLOR_SCHEME_FORCE_DARK
                                                          : ADW_COLOR_SCHEME_DEFAULT;
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), scheme);
}

// ---- actions -----------------------------------------------------------------

void App::setup_actions() {
    app_->add_action("add", [this] { present_main(); show_add_dialog(); });
    app_->add_action("preferences", [this] { present_main(); show_preferences(); });
    app_->add_action("scheduler", [this] { present_main(); show_preferences("scheduler"); });
    app_->add_action("pause-all", [this] { manager_->pause_all(); refresh(); });
    app_->add_action("resume-all", [this] { manager_->resume_all(); refresh(); });
    app_->add_action("clear-finished", [this] {
        manager_->clear_finished();
        std::set<std::string> live;
        for (auto& it : manager_->ordered()) live.insert(it->id);
        for (auto i = finish_announced_.begin(); i != finish_announced_.end();)
            i = live.count(*i) ? std::next(i) : finish_announced_.erase(i);
        refresh();
    });
    app_->add_action("about", [this] { show_about(); });
    app_->add_action("show-main", [this] { present_main(); });
    app_->add_action("search", [this] { present_main(); if (main_) main_->toggle_search(); });
    app_->add_action("quit", [this] { quit(); });

    // Paths ride along as string targets (notification buttons use these too).
    app_->add_action_with_parameter("show-file", Glib::VARIANT_TYPE_STRING, [this](const Glib::VariantBase& v) {
        show_in_folder(main_window(), Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get());
    });
    app_->add_action_with_parameter("open-file", Glib::VARIANT_TYPE_STRING, [this](const Glib::VariantBase& v) {
        open_file(main_window(), Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get());
    });
    app_->add_action_with_parameter("open-download", Glib::VARIANT_TYPE_STRING, [this](const Glib::VariantBase& v) {
        open_download_window(Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get());
    });

    limit_action_ = app_->add_action_radio_string("speed-limit", [this](const Glib::ustring& v) {
        long long bps = 0;
        try { bps = parse_rate(v == "0" ? "" : std::string(v)); } catch (...) {}
        manager_->set_global_rate((double)bps);
        manager_->save_settings();
        limit_action_->change_state(v);
        refresh();
    }, "0");
    sync_speed_limit_action();

    app_->set_accel_for_action("app.add", "<Ctrl>n");
    app_->set_accel_for_action("app.preferences", "<Ctrl>comma");
    app_->set_accel_for_action("app.search", "<Ctrl>f");
    app_->set_accel_for_action("app.quit", "<Ctrl>q");
}

void App::sync_speed_limit_action() {
    if (!limit_action_) return;
    double cur = manager_->global_rate_bps();
    Glib::ustring state = "custom";
    for (const char* p : kLimitPresets) {
        long long v = 0;
        try { v = parse_rate(std::string(p) == "0" ? "" : p); } catch (...) {}
        if ((double)v == cur || (v == 0 && cur <= 0)) { state = p; break; }
    }
    limit_action_->change_state(state);
}

// ---- periodic work -------------------------------------------------------------

void App::refresh() {
    if (quitting_ || !manager_) return;
    announce_finished();
    if (main_) main_->refresh();
}

void App::poll_jobs() {
    if (quitting_ || !manager_) return;
    for (const auto& id : manager_->ingest_jobs()) {
        if (prefs_.show_popups) open_download_window(id);
    }
}

void App::announce_finished() {
    for (auto& it : manager_->ordered()) {
        if (it->status != ST_COMPLETED || finish_announced_.count(it->id)) continue;
        finish_announced_.insert(it->id);
        if (!prefs_.notify_finish) continue;
        // An open progress window already turns into a "done" view.
        auto p = popups_.find(it->id);
        if (p != popups_.end() && p->second->visible()) continue;
        std::string name = display_name(*it), path = it->dest_path;
        bool main_visible = main_ && gtk_widget_get_visible(GTK_WIDGET(main_->window()));
        if (main_visible) {
            toast("“" + name + "” finished", "Open", [this, path] { open_file(main_window(), path); });
        } else {
            notify("done-" + it->id, "Download complete", name, path);
        }
    }
}

void App::notify(const std::string& id, const std::string& title, const std::string& body,
                 const std::string& path) {
    GNotification* n = g_notification_new(title.c_str());
    g_notification_set_body(n, body.c_str());
    GIcon* icon = g_themed_icon_new("folder-download-symbolic");
    g_notification_set_icon(n, icon);
    g_object_unref(icon);
    if (!path.empty()) {
        g_notification_set_default_action_and_target_value(n, "app.open-file", g_variant_new_string(path.c_str()));
        g_notification_add_button_with_target_value(n, "Show in Folder", "app.show-file",
                                                    g_variant_new_string(path.c_str()));
    } else {
        g_notification_set_default_action(n, "app.show-main");
    }
    g_application_send_notification(G_APPLICATION(app_->gobj()), id.c_str(), n);
    g_object_unref(n);
}

void App::toast(const std::string& title, const char* button, std::function<void()> on_button) {
    if (!main_) return;
    AdwToast* t = adw_toast_new(title.c_str());
    adw_toast_set_timeout(t, 5);
    if (button) {
        adw_toast_set_button_label(t, button);
        if (on_button) on<>(t, "button-clicked", std::move(on_button));
    }
    main_->add_toast(t);
}

// ---- progress windows ----------------------------------------------------------

void App::open_download_window(const std::string& id) {
    if (!item(id)) return;
    auto& slot = popups_[id];
    if (!slot) slot = std::make_unique<DownloadWindow>(*this, id);
    slot->present();
}

void App::close_download_window(const std::string& id) {
    // Destroy after the current signal emission has finished.
    run_on_main([this, id] { popups_.erase(id); });
}

// ---- duplicate-file prompt (called from engine worker threads) -------------------

std::string App::ask_duplicate_blocking(const std::string& path) {
    if (std::this_thread::get_id() == main_thread_) return "rename";  // never block the UI loop
    auto prom = std::make_shared<std::promise<std::string>>();
    auto fut = prom->get_future();
    run_on_main([this, path, prom] {
        if (quitting_ || !main_) { prom->set_value("rename"); return; }
        ask_duplicate(path, [prom](std::string choice) { prom->set_value(std::move(choice)); });
    });
    while (fut.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready)
        if (quitting_) return "rename";
    return fut.get();
}

// ---- dev screenshots -----------------------------------------------------------

void App::seed_demo() {
    struct Demo { const char* url; const char* name; const char* status; double frac; double speed; long long size; bool stream; };
    const Demo demos[] = {
        {"https://releases.ubuntu.com/24.04.1/ubuntu-24.04.1-desktop-amd64.iso", "ubuntu-24.04.1-desktop-amd64.iso", ST_DOWNLOADING, 0.64, 9.4e6, 6228000000LL, false},
        {"https://media.campus-lectures.example/lecture-07/master.m3u8", "lecture-07-distributed-systems.mp4", ST_DOWNLOADING, 0.42, 3.1e6, 980, true},
        {"https://cdn.example.org/audio/field-recordings-vol2.flac", "field-recordings-vol2.flac", ST_PAUSED, 0.18, 0, 412000000LL, false},
        {"https://files.example.com/design/brand-assets-2026.zip", "brand-assets-2026.zip", ST_QUEUED, 0, 0, 0, false},
        {"https://arxiv.example.org/pdf/2409.01234.pdf", "attention-is-still-all-you-need.pdf", ST_COMPLETED, 1, 0, 2400000LL, false},
        {"https://mirror.example.net/tools/blender-4.2-linux-x64.tar.xz", "blender-4.2-linux-x64.tar.xz", ST_ERROR, 0.07, 0, 340000000LL, false},
    };
    for (auto& d : demos) {
        auto it = manager_->add(d.url, d.name, 0, {}, /*start=*/false, 0, false, d.stream);
        it->status = d.status;
        it->size = d.size ? std::optional<long long>(d.size) : std::nullopt;
        it->downloaded = (long long)(d.size * d.frac);
        it->speed = d.speed;
        it->dest_path = manager_->settings.output_dir + "/" + d.name;
        if (d.speed > 0 && d.size) it->eta = (d.size - it->downloaded) / d.speed;
        if (d.stream) { it->seg_total = 980; it->seg_done = 412; }
        if (std::string(d.status) == ST_ERROR) it->error = "Server closed the connection (HTTP 503)";
        if (std::string(d.status) == ST_COMPLETED) finish_announced_.insert(it->id);
        if (!d.stream && std::string(d.status) == ST_DOWNLOADING) {
            for (int c = 0; c < 6; ++c) {
                SegmentProgress s;
                s.index = c; s.start = 0; s.end = 999;
                s.downloaded = c == 5 ? 260 : std::min(999, 420 + c * 110);
                s.active = s.downloaded < 999;
                s.speed_bps = s.active ? 1.2e6 + c * 1.5e5 : 0;
                s.stole_from = c == 5 ? 2 : -1;
                it->conn_stats.push_back(s);
            }
        }
    }
    // A plausible throughput history for the graph.
    if (main_) main_->seed_graph();
}

void App::run_shot() {
    auto out = shot_.out;
    // Offscreen backends may not tick the frame clock; skip transitions entirely.
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    int ww = 1180, wh = 760;  // MPDM_WIN_SIZE=860x900 tries other window sizes
    if (const char* ws = std::getenv("MPDM_WIN_SIZE")) std::sscanf(ws, "%dx%d", &ww, &wh);
    gtk_window_set_default_size(main_->window(), ww, wh);
    present_main();
    std::string first_dl, first_stream, first_done;
    for (auto& it : manager_->ordered()) {
        if (it->status == ST_COMPLETED && first_done.empty()) first_done = it->id;
        if (it->status == ST_DOWNLOADING && !it->stream && first_dl.empty()) first_dl = it->id;
        if (it->stream && first_stream.empty()) first_stream = it->id;
    }
    GtkWidget* target = GTK_WIDGET(main_->window());
    if (shot_.kind == "popup" && !first_dl.empty()) {
        open_download_window(first_dl);
        target = GTK_WIDGET(popups_[first_dl]->window());
    } else if (shot_.kind == "done" && !first_done.empty()) {
        open_download_window(first_done);
        target = GTK_WIDGET(popups_[first_done]->window());
    } else if (shot_.kind == "stream" && !first_stream.empty()) {
        open_download_window(first_stream);
        target = GTK_WIDGET(popups_[first_stream]->window());
    } else if (shot_.kind == "prefs") {
        show_preferences(shot_.page.empty() ? nullptr : shot_.page.c_str());
    } else if (shot_.kind == "add") {
        show_add_dialog();
    } else if (shot_.kind == "remove" && !first_dl.empty()) {
        confirm_remove(first_dl);
    }
    // Capture right after a frame is painted, when layout is guaranteed current
    // (the live views relabel themselves on timers, which re-queues allocation).
    Glib::signal_timeout().connect_once([this, target, out] {
        GdkFrameClock* clock = gtk_widget_get_frame_clock(target);
        auto done = std::make_shared<bool>(false);
        on<>(clock, "after-paint", [this, target, out, done] {
            if (*done) return;
            *done = true;
            bool ok = save_widget_png(target, out);
            g_printerr("shot %s: %s\n", ok ? "saved" : "FAILED", out.c_str());
            run_on_main([this] { quit(); });
        });
        gtk_widget_queue_draw(target);
    }, 1500);
}

} // namespace mpdm::gtk
