// Preferences — one AdwPreferencesDialog with General / Downloads / Network /
// Browser / Scheduler pages. Values are applied when the dialog closes (the
// Adwaita instant-apply model; there is no Save/Cancel), except the style and
// scheduler rules, which take effect immediately.
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "mpdm/config.hpp"
#include "mpdm/ratelimit.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace mpdm::gtk {

namespace {

struct CatDef { const char* name; const char* sub; const char* exts; };
const CatDef kBuiltin[] = {
    {"Video", "Video", "mp4 mkv webm avi mov flv m4v ts wmv mpg mpeg"},
    {"Audio", "Music", "mp3 m4a aac flac wav ogg opus wma"},
    {"Documents", "Documents", "pdf doc docx xls xlsx ppt pptx txt epub csv"},
    {"Archives", "Compressed", "zip rar 7z tar gz tgz bz2 xz iso cab"},
    {"Programs", "Programs", "exe msi msix dmg pkg deb rpm apk appimage"},
    {"Other", "", ""},
};
const int kPresetConns[] = {16, 8, 4, 2, 1};  // connection-type presets (last entry = Custom)
const char* kOnDone[] = {"none", "run", "sleep", "shutdown"};
const char* kDupPolicy[] = {"rename", "overwrite", "skip", "ask"};

const CatDef* builtin(const std::string& n) {
    for (auto& c : kBuiltin) if (n == c.name) return &c;
    return nullptr;
}

std::string pretty_exts(std::string s) {
    for (auto& ch : s) if (ch == ',' || ch == ';') ch = ' ';
    std::istringstream in(s);
    std::string w, out;
    while (in >> w) out += (out.empty() ? "" : "  ·  ") + w;
    return out;
}

struct State {
    bool reset = false;
    bool syncing = false;
    std::string out_dir, temp_dir;
    std::map<std::string, std::string> cat_dirs, cat_exts;
    std::map<std::string, bool> remember;

    GtkWidget *style, *max_conc, *autostart, *show_popups, *notify, *ask_remove, *on_done, *on_cmd, *login;
    GtkWidget *out_row, *temp_row, *categorize, *dup, *file_times, *ignore_mtime;
    GtkWidget* cat_group;
    std::vector<GtkWidget*> cat_rows;
    GtkWidget *conn_type, *conns, *dynseg, *retries, *retry_wait, *def_limit, *glob_limit;
    GtkWidget *proxy_mode, *proxy_type, *proxy_host, *proxy_port, *proxy_user, *proxy_pass, *verify_tls;
    GtkWidget *ua, *ua_custom, *clip;
    GtkWidget* hosts_group;
    std::vector<GtkWidget*> host_rows;
    GtkWidget* rules_group;
    std::vector<GtkWidget*> rule_rows;
};

// ---- row builders -------------------------------------------------------------

GtkWidget* add_page(AdwPreferencesDialog* d, const char* name, const char* title, const char* icon) {
    GtkWidget* p = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p), name);
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p), title);
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p), icon);
    adw_preferences_dialog_add(d, ADW_PREFERENCES_PAGE(p));
    return p;
}

GtkWidget* add_group(GtkWidget* page, const char* title, const char* desc = nullptr) {
    GtkWidget* g = adw_preferences_group_new();
    if (title) adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
    if (desc) adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g), desc);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
    return g;
}

void add_row(GtkWidget* group, GtkWidget* row) { adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row); }

GtkWidget* switch_row(GtkWidget* group, const char* title, const char* subtitle, bool on) {
    GtkWidget* r = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(r), on);
    add_row(group, r);
    return r;
}

GtkWidget* combo_row(GtkWidget* group, const char* title, const char* subtitle,
                     std::vector<const char*> items, guint selected) {
    items.push_back(nullptr);
    GtkWidget* r = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
    GtkStringList* model = gtk_string_list_new(items.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(r), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(r), selected);
    add_row(group, r);
    return r;
}

GtkWidget* spin_row(GtkWidget* group, const char* title, const char* subtitle, double lo, double hi, double v) {
    GtkWidget* r = adw_spin_row_new_with_range(lo, hi, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
    adw_spin_row_set_value(ADW_SPIN_ROW(r), v);
    add_row(group, r);
    return r;
}

GtkWidget* entry_row(GtkWidget* group, const char* title, const std::string& text) {
    GtkWidget* r = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    gtk_editable_set_text(GTK_EDITABLE(r), text.c_str());
    if (group) add_row(group, r);
    return r;
}

GtkWidget* icon_button(const char* icon, const char* tooltip) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b, tooltip);
    return b;
}

bool sw(GtkWidget* r) { return adw_switch_row_get_active(ADW_SWITCH_ROW(r)); }
guint sel(GtkWidget* r) { return adw_combo_row_get_selected(ADW_COMBO_ROW(r)); }
int spin(GtkWidget* r) { return (int)adw_spin_row_get_value(ADW_SPIN_ROW(r)); }
std::string text(GtkWidget* r) { return gtk_editable_get_text(GTK_EDITABLE(r)); }

// Folder picker -> callback with the chosen absolute path.
void choose_folder(GtkWindow* parent, const char* title, const std::string& start,
                   std::function<void(std::string)> done) {
    GtkFileDialog* fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, title);
    if (!start.empty()) {
        GFile* f = g_file_new_for_path(start.c_str());
        gtk_file_dialog_set_initial_folder(fd, f);
        g_object_unref(f);
    }
    auto* cb = new std::function<void(std::string)>(std::move(done));
    gtk_file_dialog_select_folder(fd, parent, nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* cb = static_cast<std::function<void(std::string)>*>(data);
            if (GFile* f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr)) {
                if (char* p = g_file_get_path(f)) { (*cb)(p); g_free(p); }
                g_object_unref(f);
            }
            delete cb;
        },
        cb);
    g_object_unref(fd);
}

// ---- proxy URL <-> fields ----------------------------------------------------------

struct ProxyParts { int type = 0; std::string host, port, user, pass; };

ProxyParts parse_proxy(const std::string& url) {
    ProxyParts p;
    std::string rest = url;
    auto sch = rest.find("://");
    if (sch != std::string::npos) {
        std::string s = rest.substr(0, sch);
        p.type = s.rfind("socks", 0) == 0 ? 2 : s == "https" ? 1 : 0;
        rest = rest.substr(sch + 3);
    }
    rest = rest.substr(0, rest.find('/'));
    if (auto at = rest.rfind('@'); at != std::string::npos) {
        std::string cred = rest.substr(0, at);
        rest = rest.substr(at + 1);
        auto c = cred.find(':');
        p.user = cred.substr(0, c);
        if (c != std::string::npos) p.pass = cred.substr(c + 1);
    }
    if (auto c = rest.rfind(':'); c != std::string::npos && rest.find(']') == std::string::npos) {
        p.port = rest.substr(c + 1);
        rest = rest.substr(0, c);
    }
    p.host = rest;
    return p;
}

std::string system_proxy() {
    for (const char* v : {"http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"})
        if (const char* e = std::getenv(v); e && *e) return e;
    return "";
}

// Run mpdm_host (next to this binary, one level up in a build tree, or on PATH)
// and collect its output. The host owns the browser manifests; --list prints
// {"chromium": [...], "firefox": [...]}.
struct HostResult { bool ok = false; std::string out; };

HostResult run_host(const fs::path& exe_dir, std::vector<std::string> args) {
    fs::path host;
    std::error_code ec;
    for (fs::path cand : {exe_dir / "mpdm_host", exe_dir.parent_path() / "mpdm_host"})
        if (fs::exists(cand, ec)) { host = cand; break; }
    if (host.empty())
        if (char* p = g_find_program_in_path("mpdm_host")) { host = p; g_free(p); }
    if (host.empty()) return {false, "mpdm_host wasn’t found next to MPDM or on PATH"};

    std::string hs = host.string();
    std::vector<char*> argv{hs.data()};
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    char *out = nullptr, *err = nullptr;
    int status = 0;
    GError* e = nullptr;
    HostResult r;
    if (g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, &out, &err, &status, &e)) {
        r.ok = g_spawn_check_wait_status(status, nullptr);
        r.out = std::string(out ? out : "") + (err ? err : "");
    } else {
        r.out = e ? e->message : "couldn’t run mpdm_host";
    }
    while (!r.out.empty() && std::isspace((unsigned char)r.out.back())) r.out.pop_back();
    if (e) g_error_free(e);
    g_free(out);
    g_free(err);
    return r;
}

} // namespace

// ------------------------------------------------------------------------------------

void App::show_preferences(const char* page_name) {
    auto st = std::make_shared<State>();
    auto& S = manager_->settings;
    st->out_dir = S.output_dir;
    st->temp_dir = S.temp_dir;
    st->cat_dirs = S.category_dirs;
    st->cat_exts = S.category_exts;
    st->remember = S.remember_category_folder;

    AdwDialog* dlg = adw_preferences_dialog_new();
    AdwPreferencesDialog* pd = ADW_PREFERENCES_DIALOG(dlg);
    adw_preferences_dialog_set_search_enabled(pd, TRUE);
    adw_dialog_set_content_width(dlg, 640);
    adw_dialog_set_content_height(dlg, 680);
    auto ptoast = [pd](const std::string& msg) { adw_preferences_dialog_add_toast(pd, adw_toast_new(msg.c_str())); };

    // ===== General =====
    GtkWidget* general = add_page(pd, "general", "General", "preferences-system-symbolic");
    GtkWidget* g = add_group(general, "Appearance");
    guint style_sel = prefs_.appearance == "light" ? 1 : prefs_.appearance == "dark" ? 2 : 0;
    st->style = combo_row(g, "Style", nullptr, {"Follow System", "Light", "Dark"}, style_sel);
    on<GParamSpec*>(st->style, "notify::selected", [this, st](GParamSpec*) {
        const char* modes[] = {"system", "light", "dark"};
        prefs_.appearance = modes[std::min<guint>(sel(st->style), 2)];
        apply_appearance();
    });

    g = add_group(general, "Downloads");
    st->max_conc = spin_row(g, "Simultaneous downloads", "How many downloads run at the same time", 1, 10,
                            manager_->max_concurrent);
    st->autostart = switch_row(g, "Start right away", "Otherwise new downloads wait in the queue", prefs_.autostart);
    st->show_popups = switch_row(g, "Show progress window", "Open a window for each download as it starts",
                                 prefs_.show_popups);
    st->notify = switch_row(g, "Notify when finished", nullptr, prefs_.notify_finish);
    st->ask_remove = switch_row(g, "Confirm before removing", nullptr, prefs_.ask_on_remove);

    g = add_group(general, "When Everything Finishes");
    guint od = 0;
    for (guint i = 0; i < 4; ++i) if (manager_->on_done_action == kOnDone[i]) od = i;
    st->on_done = combo_row(g, "Action", nullptr, {"Do nothing", "Run a command", "Suspend", "Shut down"}, od);
    st->on_cmd = entry_row(g, "Command", manager_->on_done_command);
    gtk_widget_set_visible(st->on_cmd, od == 1);
    on<GParamSpec*>(st->on_done, "notify::selected", [st](GParamSpec*) {
        gtk_widget_set_visible(st->on_cmd, sel(st->on_done) == 1);
    });

    g = add_group(general, "Startup");
    st->login = switch_row(g, "Run in background at login", "Keeps MPDM ready for browser downloads",
                           login_autostart_enabled());

    g = add_group(general, nullptr);
    GtkWidget* reset = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(reset), "Reset to Defaults…");
    gtk_widget_add_css_class(reset, "destructive-action");
    add_row(g, reset);
    on<>(reset, "activated", [this, st, dlg] {
        AdwDialog* c = adw_alert_dialog_new("Reset Preferences?",
            "All preferences go back to their defaults. Your downloads and schedule are kept.");
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(c), "cancel", "_Cancel");
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(c), "reset", "_Reset");
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(c), "reset", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(c), "cancel");
        on<const char*>(c, "response", [this, st, dlg](const char* r) {
            if (std::string(r) != "reset") return;
            st->reset = true;
            manager_->settings = Settings{};
            manager_->max_concurrent = 3;
            manager_->set_global_rate(0);
            manager_->set_on_complete("none");
            manager_->clip_monitor_pref = false;
            manager_->save_settings();
            bool hint = prefs_.background_hint_shown;
            prefs_ = UiPrefs{};
            prefs_.background_hint_shown = hint;
            prefs_.save();
            set_login_autostart(false, exe_path());
            apply_appearance();
            sync_speed_limit_action();
            adw_dialog_close(dlg);
            toast("Preferences reset to defaults");
        });
        adw_dialog_present(c, GTK_WIDGET(dlg));
    });

    // ===== Downloads =====
    GtkWidget* downloads = add_page(pd, "downloads", "Downloads", "folder-download-symbolic");
    g = add_group(downloads, "Location");
    st->out_row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(st->out_row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->out_row), "Download folder");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->out_row), st->out_dir.c_str());
    gtk_widget_add_css_class(st->out_row, "property");
    GtkWidget* out_btn = icon_button("folder-open-symbolic", "Choose Folder");
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->out_row), out_btn);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(st->out_row), out_btn);
    add_row(g, st->out_row);
    on<>(out_btn, "clicked", [this, st] {
        choose_folder(main_window(), "Download Folder", st->out_dir, [st](std::string p) {
            st->out_dir = p;
            adw_action_row_set_subtitle(ADW_ACTION_ROW(st->out_row), p.c_str());
        });
    });

    st->temp_row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(st->temp_row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->temp_row), "Temporary folder");
    auto temp_subtitle = [st] {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(st->temp_row), st->temp_dir.empty()
            ? "Off — parts download straight into the destination" : st->temp_dir.c_str());
    };
    temp_subtitle();
    GtkWidget* temp_clear = icon_button("edit-clear-symbolic", "Turn Off");
    GtkWidget* temp_btn = icon_button("folder-open-symbolic", "Choose Folder");
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->temp_row), temp_clear);
    adw_action_row_add_suffix(ADW_ACTION_ROW(st->temp_row), temp_btn);
    add_row(g, st->temp_row);
    on<>(temp_clear, "clicked", [st, temp_subtitle] { st->temp_dir.clear(); temp_subtitle(); });
    on<>(temp_btn, "clicked", [this, st, temp_subtitle] {
        choose_folder(main_window(), "Temporary Folder", st->temp_dir, [st, temp_subtitle](std::string p) {
            st->temp_dir = p;
            temp_subtitle();
        });
    });

    st->categorize = switch_row(g, "Sort into category folders", "Video, Music, Documents and so on", S.categorize);
    guint dp = 0;
    for (guint i = 0; i < 4; ++i) if (S.duplicate_policy == kDupPolicy[i]) dp = i;
    st->dup = combo_row(g, "If the file already exists", nullptr,
                        {"Keep both", "Replace it", "Skip the download", "Ask each time"}, dp);

    g = add_group(downloads, "Files");
    st->file_times = switch_row(g, "Keep the server’s file date", nullptr, S.set_file_times);
    st->ignore_mtime = switch_row(g, "Resume even if the file date changed",
                                  "Only the size and ETag must still match", S.resume_ignore_mtime);

    st->cat_group = add_group(downloads, "Categories", "Which file types go to which folder.");
    GtkWidget* new_cat = icon_button("list-add-symbolic", "New Category");
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(st->cat_group), new_cat);

    // Rebuilders refer to themselves weakly (rows trigger them) to avoid a ref cycle.
    auto rebuild_cats = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_rebuild = rebuild_cats;
    *rebuild_cats = [this, st, weak_rebuild] {
        for (GtkWidget* r : st->cat_rows) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(st->cat_group), r);
        st->cat_rows.clear();
        std::vector<std::string> names;
        for (auto& c : kBuiltin) names.push_back(c.name);
        for (auto& [n, e] : st->cat_exts) if (!builtin(n)) names.push_back(n);

        for (const std::string& name : names) {
            const CatDef* b = builtin(name);
            std::string extra = st->cat_exts.count(name) ? st->cat_exts[name] : "";
            std::string all = b ? std::string(b->exts) + (extra.empty() ? "" : " " + extra) : extra;
            std::string sub = b ? b->sub : name;
            std::string def_folder = sub.empty() ? st->out_dir : st->out_dir + "/" + sub;

            GtkWidget* exp = adw_expander_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(exp), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exp), name.c_str());
            adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(exp), name == "Other"
                ? "Files that don’t match any other category" : pretty_exts(all).c_str());
            GtkWidget* icon = gtk_image_new_from_icon_name(b ? category_icon(name) : "folder-symbolic");
            adw_expander_row_add_prefix(ADW_EXPANDER_ROW(exp), icon);

            GtkWidget* folder = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(folder), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder), "Folder");
            auto dir_it = st->cat_dirs.find(name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(folder),
                dir_it != st->cat_dirs.end() ? dir_it->second.c_str() : def_folder.c_str());
            GtkWidget* undo = icon_button("edit-undo-symbolic", "Use Default Folder");
            gtk_widget_set_visible(undo, dir_it != st->cat_dirs.end());
            GtkWidget* pick = icon_button("folder-open-symbolic", "Choose Folder");
            adw_action_row_add_suffix(ADW_ACTION_ROW(folder), undo);
            adw_action_row_add_suffix(ADW_ACTION_ROW(folder), pick);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), folder);
            on<>(undo, "clicked", [st, name, folder, undo, def_folder] {
                st->cat_dirs.erase(name);
                adw_action_row_set_subtitle(ADW_ACTION_ROW(folder), def_folder.c_str());
                gtk_widget_set_visible(undo, FALSE);
            });
            on<>(pick, "clicked", [this, st, name, folder, undo, def_folder] {
                std::string start = st->cat_dirs.count(name) ? st->cat_dirs[name] : def_folder;
                choose_folder(main_window(), (name + " Folder").c_str(), start, [st, name, folder, undo](std::string p) {
                    st->cat_dirs[name] = p;
                    adw_action_row_set_subtitle(ADW_ACTION_ROW(folder), p.c_str());
                    gtk_widget_set_visible(undo, TRUE);
                });
            });

            if (name != "Other") {
                GtkWidget* types = entry_row(nullptr, b ? "Extra file types" : "File types", extra);
                adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), types);
                on<>(types, "changed", [st, name, types, exp, b] {
                    std::string v = text(types);
                    if (v.empty() && b) st->cat_exts.erase(name);
                    else st->cat_exts[name] = v;
                    std::string all = b ? std::string(b->exts) + (v.empty() ? "" : " " + v) : v;
                    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(exp), pretty_exts(all).c_str());
                });
            }

            GtkWidget* rem = adw_switch_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rem), "Remember the last folder used");
            adw_switch_row_set_active(ADW_SWITCH_ROW(rem), st->remember.count(name) && st->remember[name]);
            adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), rem);
            on<GParamSpec*>(rem, "notify::active", [st, name, rem](GParamSpec*) { st->remember[name] = sw(rem); });

            if (!b) {
                GtkWidget* del = adw_button_row_new();
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(del), "Remove Category");
                gtk_widget_add_css_class(del, "destructive-action");
                adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), del);
                on<>(del, "activated", [st, name, weak_rebuild] {
                    st->cat_exts.erase(name);
                    st->cat_dirs.erase(name);
                    st->remember.erase(name);
                    if (auto r = weak_rebuild.lock()) run_on_main([r] { (*r)(); });
                });
            }
            add_row(st->cat_group, exp);
            st->cat_rows.push_back(exp);
        }
    };
    (*rebuild_cats)();
    on<>(new_cat, "clicked", [st, dlg, rebuild_cats] {
        AdwDialog* c = adw_alert_dialog_new("New Category", "Files with these extensions get their own folder.");
        AdwAlertDialog* ad = ADW_ALERT_DIALOG(c);
        adw_alert_dialog_add_response(ad, "cancel", "_Cancel");
        adw_alert_dialog_add_response(ad, "add", "_Add");
        adw_alert_dialog_set_response_appearance(ad, "add", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(ad, "add");
        adw_alert_dialog_set_close_response(ad, "cancel");
        GtkWidget* list = gtk_list_box_new();
        gtk_widget_add_css_class(list, "boxed-list");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        GtkWidget* name = entry_row(nullptr, "Name", "");
        GtkWidget* exts = entry_row(nullptr, "Extensions, e.g. iso img bin", "");
        gtk_list_box_append(GTK_LIST_BOX(list), name);
        gtk_list_box_append(GTK_LIST_BOX(list), exts);
        adw_alert_dialog_set_extra_child(ad, list);
        on<const char*>(c, "response", [st, name, exts, rebuild_cats](const char* r) {
            std::string n = text(name);
            n.erase(0, n.find_first_not_of(' '));
            n.erase(n.find_last_not_of(' ') + 1);
            if (std::string(r) != "add" || n.empty() || builtin(n)) return;
            st->cat_exts[n] = text(exts);
            (*rebuild_cats)();
        });
        adw_dialog_set_focus(c, name);
        adw_dialog_present(c, GTK_WIDGET(dlg));
    });

    // ===== Network =====
    GtkWidget* network = add_page(pd, "network", "Network", "network-workgroup-symbolic");
    g = add_group(network, "Connections");
    guint ct = 5;
    for (guint i = 0; i < 5; ++i) if (kPresetConns[i] == S.connections) ct = i;
    st->conn_type = combo_row(g, "Connection type", nullptr,
        {"Fiber / high speed", "Broadband", "DSL / cable", "Slow / mobile", "Single connection", "Custom"}, ct);
    st->conns = spin_row(g, "Connections per download", nullptr, 1, 32, S.connections);
    on<GParamSpec*>(st->conn_type, "notify::selected", [st](GParamSpec*) {
        if (st->syncing || sel(st->conn_type) >= 5) return;
        st->syncing = true;
        adw_spin_row_set_value(ADW_SPIN_ROW(st->conns), kPresetConns[sel(st->conn_type)]);
        st->syncing = false;
    });
    on<GParamSpec*>(st->conns, "notify::value", [st](GParamSpec*) {
        if (st->syncing) return;
        guint idx = 5;
        for (guint i = 0; i < 5; ++i) if (kPresetConns[i] == spin(st->conns)) idx = i;
        st->syncing = true;
        adw_combo_row_set_selected(ADW_COMBO_ROW(st->conn_type), idx);
        st->syncing = false;
    });
    st->dynseg = switch_row(g, "Dynamic segmentation", "Idle connections take over part of the slowest range",
                            S.dynamic_segmentation);

    g = add_group(network, "Retries");
    st->retries = spin_row(g, "Retry attempts", "Before a download gives up", 0, 50, S.max_retries);
    st->retry_wait = spin_row(g, "Seconds between retries", "0 waits a little longer after each failure", 0, 600,
                              (double)S.retry_interval);

    g = add_group(network, "Speed Limits", "Use values like 500k or 1.5m. Leave empty for unlimited.");
    st->def_limit = entry_row(g, "Each download", rate_input((double)S.max_rate_bps));
    st->glob_limit = entry_row(g, "All downloads together", rate_input(manager_->global_rate_bps()));

    g = add_group(network, "Proxy");
    ProxyParts pp = parse_proxy(S.proxy);
    st->proxy_mode = combo_row(g, "Connection", nullptr, {"Direct, no proxy", "System proxy", "Manual proxy"},
                               S.proxy.empty() ? 0 : 2);
    st->proxy_type = combo_row(g, "Type", nullptr, {"HTTP", "HTTPS", "SOCKS5"}, pp.type);
    st->proxy_host = entry_row(g, "Server", pp.host);
    st->proxy_port = entry_row(g, "Port", pp.port);
    gtk_editable_set_width_chars(GTK_EDITABLE(st->proxy_port), 6);
    adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(st->proxy_port), GTK_INPUT_PURPOSE_DIGITS);
    st->proxy_user = entry_row(g, "Username (optional)", pp.user);
    st->proxy_pass = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->proxy_pass), "Password (optional)");
    gtk_editable_set_text(GTK_EDITABLE(st->proxy_pass), pp.pass.c_str());
    add_row(g, st->proxy_pass);
    auto sync_proxy = [st] {
        bool manual = sel(st->proxy_mode) == 2;
        for (GtkWidget* w : {st->proxy_type, st->proxy_host, st->proxy_port, st->proxy_user, st->proxy_pass})
            gtk_widget_set_visible(w, manual);
    };
    sync_proxy();
    on<GParamSpec*>(st->proxy_mode, "notify::selected", [sync_proxy](GParamSpec*) { sync_proxy(); });

    g = add_group(network, "Identity &amp; Security");
    guint ua = S.user_agent == DEFAULT_USER_AGENT ? 0 : S.user_agent == BROWSER_USER_AGENT ? 1 : 2;
    st->ua = combo_row(g, "User agent", "Some servers only answer browsers",
                       {"MPDM (honest)", "Browser (Chrome)", "Custom"}, ua);
    st->ua_custom = entry_row(g, "Custom user agent", ua == 2 ? S.user_agent : "");
    gtk_widget_set_visible(st->ua_custom, ua == 2);
    on<GParamSpec*>(st->ua, "notify::selected", [st](GParamSpec*) {
        gtk_widget_set_visible(st->ua_custom, sel(st->ua) == 2);
    });
    st->verify_tls = switch_row(g, "Verify TLS certificates", nullptr, S.verify_tls);

    // ===== Browser =====
    // Several browsers can be connected at once; each extension ID is one entry
    // in the host's Chromium or Firefox list.
    GtkWidget* browser = add_page(pd, "browser", "Browser", "web-browser-symbolic");
    const fs::path exe_dir = fs::path(exe_path()).parent_path();
    st->hosts_group = add_group(browser, "Connected Browsers", "These browsers can hand their downloads to MPDM.");

    auto reload_hosts = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak_hosts = reload_hosts;
    *reload_hosts = [st, exe_dir, ptoast, weak_hosts] {
        for (GtkWidget* r : st->host_rows) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(st->hosts_group), r);
        st->host_rows.clear();
        auto add_row_to_group = [st](GtkWidget* row) {
            add_row(st->hosts_group, row);
            st->host_rows.push_back(row);
        };
        HostResult list = run_host(exe_dir, {"--list"});
        auto j = nlohmann::json::parse(list.out, nullptr, /*allow_exceptions=*/false);
        int shown = 0;
        for (const char* family : {"chromium", "firefox"}) {
            if (!j.is_object() || !j.contains(family) || !j[family].is_array()) continue;
            bool ff = std::string(family) == "firefox";
            for (auto& v : j[family]) {
                if (!v.is_string()) continue;
                std::string id = v.get<std::string>();
                GtkWidget* row = adw_action_row_new();
                adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), ff ? "Firefox" : "Chrome, Edge, Brave, Vivaldi");
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), id.c_str());
                gtk_widget_add_css_class(row, "property");
                adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("web-browser-symbolic"));
                GtkWidget* del = icon_button("user-trash-symbolic", "Disconnect");
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), del);
                on<>(del, "clicked", [exe_dir, id, ptoast, weak_hosts] {
                    HostResult r = run_host(exe_dir, {"--unregister", "--extension-id", id});
                    ptoast(r.ok ? std::string("Disconnected") : "Couldn’t disconnect: " + r.out);
                    if (auto rh = weak_hosts.lock()) run_on_main([rh] { (*rh)(); });
                });
                add_row_to_group(row);
                ++shown;
            }
        }
        if (shown == 0) {
            GtkWidget* none = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(none), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(none),
                                          list.ok ? "No browsers connected yet" : "Can’t read the connections");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(none), list.ok ? "Add one below" : list.out.c_str());
            gtk_widget_add_css_class(none, "dim-label");
            add_row_to_group(none);
        }
    };
    (*reload_hosts)();

    g = add_group(browser, "Add a Browser",
        "Chrome, Edge, Brave or Vivaldi: open chrome://extensions, turn on Developer mode, choose "
        "Load unpacked and pick MPDM’s “extension” folder (/usr/share/mpdm/extension when installed), "
        "then paste the ID it shows below. Repeat for each browser.\n\n"
        "Firefox: choose Connect Firefox, then load the extension from about:debugging → This Firefox → "
        "Load Temporary Add-on.");
    GtkWidget* add_id = entry_row(g, "Extension ID", "");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(add_id), TRUE);
    on<>(add_id, "apply", [add_id, exe_dir, ptoast, reload_hosts] {
        std::string id = text(add_id);
        if (id.empty()) { ptoast("Paste the extension’s ID first"); return; }
        HostResult r = run_host(exe_dir, {"--register", "--extension-id", id});
        if (r.ok) {
            gtk_editable_set_text(GTK_EDITABLE(add_id), "");
            ptoast("Connected — reload the extension in that browser");
        } else {
            ptoast("Couldn’t connect: " + r.out);
        }
        (*reload_hosts)();
    });
    GtkWidget* firefox = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(firefox), "Connect Firefox");
    adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(firefox), "list-add-symbolic");
    add_row(g, firefox);
    on<>(firefox, "activated", [exe_dir, ptoast, reload_hosts] {
        HostResult r = run_host(exe_dir, {"--register", "--firefox"});
        if (!r.ok) ptoast("Couldn’t connect Firefox: " + r.out);
        else if (r.out.find("not found") != std::string::npos)
            ptoast("Saved, but no Firefox profile was found — start Firefox once, then try again");
        else ptoast("Firefox connected — load the extension in Firefox if you haven’t");
        (*reload_hosts)();
    });

    g = add_group(browser, "Clipboard");
    st->clip = switch_row(g, "Watch the clipboard", "Offer to add links you copy", manager_->clip_monitor_pref);

    // ===== Scheduler =====
    GtkWidget* sched = add_page(pd, "scheduler", "Scheduler", "alarm-symbolic");
    st->rules_group = add_group(sched, "Rules", "Pause, resume or throttle the whole queue at set times.");
    auto rebuild_rules = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = rebuild_rules;
    *rebuild_rules = [this, st, weak] {
        for (GtkWidget* r : st->rule_rows) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(st->rules_group), r);
        st->rule_rows.clear();
        auto rules = manager_->list_schedules();
        if (rules.empty()) {
            GtkWidget* none = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(none), "No rules yet");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(none), "Add one below, e.g. resume everything at 01:00");
            gtk_widget_add_css_class(none, "dim-label");
            add_row(st->rules_group, none);
            st->rule_rows.push_back(none);
        }
        for (const auto& r : rules) {
            std::string what = r.action == "pause_all" ? "Pause everything"
                               : r.action == "resume_all" ? "Resume everything"
                                                           : "Limit to " + human_speed((double)r.rate_bps);
            std::string when = (r.repeat == "once" ? "Once at " : "Every day at ") + r.at;
            GtkWidget* row = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), what.c_str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), when.c_str());
            GtkWidget* en = gtk_switch_new();
            gtk_switch_set_active(GTK_SWITCH(en), r.enabled);
            gtk_widget_set_valign(en, GTK_ALIGN_CENTER);
            GtkWidget* del = icon_button("user-trash-symbolic", "Delete Rule");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), en);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), del);
            std::string id = r.id;
            on<GParamSpec*>(en, "notify::active", [this, id, en](GParamSpec*) {
                manager_->set_schedule_enabled(id, gtk_switch_get_active(GTK_SWITCH(en)));
            });
            on<>(del, "clicked", [this, id, weak] {
                manager_->remove_schedule(id);
                if (auto rb = weak.lock()) run_on_main([rb] { (*rb)(); });
            });
            add_row(st->rules_group, row);
            st->rule_rows.push_back(row);
        }
    };
    (*rebuild_rules)();

    g = add_group(sched, "New Rule");
    GtkWidget* r_action = combo_row(g, "Action", nullptr, {"Pause everything", "Resume everything", "Set speed limit"}, 1);
    GtkWidget* r_at = entry_row(g, "Time — HH:MM, or 2026-01-01T09:00 for once", "");
    GtkWidget* r_repeat = combo_row(g, "Repeat", nullptr, {"Every day", "Once"}, 0);
    GtkWidget* r_rate = entry_row(g, "Speed limit, e.g. 2m", "");
    gtk_widget_set_visible(r_rate, FALSE);
    on<GParamSpec*>(r_action, "notify::selected", [r_action, r_rate](GParamSpec*) {
        gtk_widget_set_visible(r_rate, sel(r_action) == 2);
    });
    GtkWidget* r_add = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r_add), "Add Rule");
    adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(r_add), "list-add-symbolic");
    add_row(g, r_add);
    on<>(r_add, "activated", [this, r_action, r_at, r_repeat, r_rate, rebuild_rules, ptoast] {
        const char* acts[] = {"pause_all", "resume_all", "set_global_rate"};
        try {
            long long bps = sel(r_action) == 2 ? parse_rate(text(r_rate)) : 0;
            manager_->add_schedule(acts[std::min<guint>(sel(r_action), 2)], text(r_at),
                                   sel(r_repeat) == 1 ? "once" : "daily", {}, bps);
            gtk_editable_set_text(GTK_EDITABLE(r_at), "");
            gtk_editable_set_text(GTK_EDITABLE(r_rate), "");
            (*rebuild_rules)();
            ptoast("Rule added");
        } catch (const std::exception& e) {
            ptoast(std::string("Couldn’t add the rule: ") + e.what());
        }
    });

    // ---- apply on close ----
    on<>(dlg, "closed", [this, st] {
        if (st->reset) return;
        auto& S = manager_->settings;
        auto rate = [](GtkWidget* r) -> long long { try { return parse_rate(text(r)); } catch (...) { return 0; } };
        S.output_dir = st->out_dir;
        S.temp_dir = st->temp_dir;
        S.categorize = sw(st->categorize);
        S.category_dirs = st->cat_dirs;
        S.category_exts = st->cat_exts;
        S.remember_category_folder = st->remember;
        S.duplicate_policy = kDupPolicy[std::min<guint>(sel(st->dup), 3)];
        S.set_file_times = sw(st->file_times);
        S.resume_ignore_mtime = sw(st->ignore_mtime);
        S.connections = spin(st->conns);
        S.dynamic_segmentation = sw(st->dynseg);
        S.max_retries = spin(st->retries);
        S.retry_interval = spin(st->retry_wait);
        S.max_rate_bps = rate(st->def_limit);
        S.verify_tls = sw(st->verify_tls);
        guint ua = sel(st->ua);
        S.user_agent = ua == 0 ? DEFAULT_USER_AGENT : ua == 1 ? BROWSER_USER_AGENT : text(st->ua_custom);
        guint pm = sel(st->proxy_mode);
        if (pm == 0) S.proxy.clear();
        else if (pm == 1) S.proxy = system_proxy();
        else {
            std::string host = text(st->proxy_host);
            if (host.empty()) S.proxy.clear();
            else {
                const char* schemes[] = {"http", "https", "socks5h"};
                std::string url = std::string(schemes[std::min<guint>(sel(st->proxy_type), 2)]) + "://";
                std::string user = text(st->proxy_user), pass = text(st->proxy_pass);
                if (!user.empty()) url += user + (pass.empty() ? "" : ":" + pass) + "@";
                url += host;
                if (!text(st->proxy_port).empty()) url += ":" + text(st->proxy_port);
                S.proxy = url;
            }
        }
        manager_->max_concurrent = spin(st->max_conc);
        manager_->clip_monitor_pref = sw(st->clip);
        manager_->set_global_rate((double)rate(st->glob_limit));
        manager_->set_on_complete(kOnDone[std::min<guint>(sel(st->on_done), 3)], text(st->on_cmd));
        manager_->save_settings();

        prefs_.autostart = sw(st->autostart);
        prefs_.show_popups = sw(st->show_popups);
        prefs_.notify_finish = sw(st->notify);
        prefs_.ask_on_remove = sw(st->ask_remove);
        prefs_.save();
        if (sw(st->login) != login_autostart_enabled()) set_login_autostart(sw(st->login), exe_path());
        sync_speed_limit_action();
        refresh();
    });

    if (page_name) adw_preferences_dialog_set_visible_page_name(pd, page_name);
    adw_dialog_present(dlg, dialog_parent());
}

} // namespace mpdm::gtk
