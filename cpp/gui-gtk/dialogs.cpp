// Small modal flows: add downloads, remove, per-download speed limit, the
// "file already exists" prompt and About. All are libadwaita dialogs presented
// over the main window.
#include <string>

#include "app.hpp"
#include "mpdm/ratelimit.hpp"
#include "util.hpp"

namespace mpdm::gtk {

static GtkWidget* boxed_list() {
    GtkWidget* list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    return list;
}

static GtkWidget* switch_row(GtkWidget* list, const char* title, const char* subtitle, bool active) {
    GtkWidget* row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    return row;
}

static bool looks_like_link(const std::string& s) {
    for (const char* p : {"http://", "https://", "ftp://"})
        if (s.rfind(p, 0) == 0) return true;
    return false;
}

// ---- add downloads ---------------------------------------------------------------

void App::show_add_dialog() {
    AdwDialog* dlg = adw_alert_dialog_new("Add Downloads",
        "One link per line. Patterns like img[1-50].jpg expand into many downloads.");
    AdwAlertDialog* ad = ADW_ALERT_DIALOG(dlg);
    adw_alert_dialog_add_response(ad, "cancel", "_Cancel");
    adw_alert_dialog_add_response(ad, "add", "_Add");
    adw_alert_dialog_set_response_appearance(ad, "add", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ad, "add");
    adw_alert_dialog_set_close_response(ad, "cancel");
    adw_alert_dialog_set_response_enabled(ad, "add", FALSE);
    adw_dialog_set_content_width(dlg, 520);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 10);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(view), FALSE);
    gtk_widget_add_css_class(view, "inline");
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 120);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_overflow(scroll, GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(box), scroll);

    GtkWidget* opts = boxed_list();
    GtkWidget* start = switch_row(opts, "Start right away", nullptr, prefs_.autostart);
    GtkWidget* priv = switch_row(opts, "Private download",
                                 "Saved to a hidden folder and kept out of the history", false);
    gtk_box_append(GTK_BOX(box), opts);
    adw_alert_dialog_set_extra_child(ad, box);

    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    on<>(buf, "changed", [ad, buf] {
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buf, &a, &b);
        char* t = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
        bool any = false;
        for (char* p = t; *p; ++p) if (!g_ascii_isspace(*p)) { any = true; break; }
        g_free(t);
        adw_alert_dialog_set_response_enabled(ad, "add", any);
    });

    // Pre-fill from the clipboard when it holds a link.
    g_object_ref(buf);
    gdk_clipboard_read_text_async(
        gdk_display_get_clipboard(gdk_display_get_default()), nullptr,
        [](GObject* src, GAsyncResult* res, gpointer data) {
            auto* buf = static_cast<GtkTextBuffer*>(data);
            char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, nullptr);
            if (text && gtk_text_buffer_get_char_count(buf) == 0) {
                std::string s = text;
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                if (looks_like_link(s)) gtk_text_buffer_set_text(buf, s.c_str(), -1);
            }
            g_free(text);
            g_object_unref(buf);
        },
        buf);

    on<const char*>(dlg, "response", [this, buf, start, priv](const char* response) {
        if (std::string(response) != "add") return;
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buf, &a, &b);
        char* t = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
        std::string text = t;
        g_free(t);
        bool autostart = adw_switch_row_get_active(ADW_SWITCH_ROW(start));
        bool incognito = adw_switch_row_get_active(ADW_SWITCH_ROW(priv));
        prefs_.autostart = autostart;
        prefs_.save();
        auto added = manager_->add_batch(text, 0, autostart, 0, incognito);
        refresh();
        if (added.empty()) { toast("No valid links found"); return; }
        // One progress window per download, like browser downloads — but a pasted
        // wildcard batch would flood the screen, so only for a handful.
        if (prefs_.show_popups && autostart && added.size() <= 8) {
            for (auto& it : added) if (it) open_download_window(it->id);
        } else {
            toast(added.size() == 1 ? std::string("1 download added")
                                    : std::to_string(added.size()) + " downloads added");
        }
    });

    adw_dialog_set_focus(dlg, view);
    adw_dialog_present(dlg, dialog_parent());
}

// ---- remove -------------------------------------------------------------------------

void App::confirm_remove(const std::string& id) {
    auto it = item(id);
    if (!it) return;
    std::string name = display_name(*it);
    auto remove = [this, id, name](bool delete_file) {
        manager_->remove(id, delete_file);
        finish_announced_.erase(id);
        close_download_window(id);
        refresh();
        toast(delete_file ? "Removed “" + name + "” and its file" : "Removed “" + name + "”");
    };
    if (!prefs_.ask_on_remove) { remove(prefs_.remove_delete_default); return; }

    AdwDialog* dlg = adw_alert_dialog_new("Remove Download?", nullptr);
    AdwAlertDialog* ad = ADW_ALERT_DIALOG(dlg);
    adw_alert_dialog_format_body(ad, "“%s” will be removed from the list.", name.c_str());
    adw_alert_dialog_add_response(ad, "cancel", "_Cancel");
    adw_alert_dialog_add_response(ad, "remove", "_Remove");
    adw_alert_dialog_set_response_appearance(ad, "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ad, "cancel");
    adw_alert_dialog_set_close_response(ad, "cancel");

    GtkWidget* opts = boxed_list();
    GtkWidget* del = switch_row(opts, "Also delete the file", nullptr, prefs_.remove_delete_default);
    GtkWidget* remember = switch_row(opts, "Don’t ask again", "Change this later in Preferences", false);
    adw_alert_dialog_set_extra_child(ad, opts);

    on<const char*>(dlg, "response", [this, del, remember, remove](const char* response) {
        if (std::string(response) != "remove") return;
        bool delete_file = adw_switch_row_get_active(ADW_SWITCH_ROW(del));
        if (adw_switch_row_get_active(ADW_SWITCH_ROW(remember))) {
            prefs_.ask_on_remove = false;
            prefs_.remove_delete_default = delete_file;
            prefs_.save();
        }
        remove(delete_file);
    });
    adw_dialog_present(dlg, dialog_parent());
}

// ---- per-download speed limit ----------------------------------------------------------

void App::ask_item_limit(const std::string& id) {
    auto it = item(id);
    if (!it) return;
    AdwDialog* dlg = adw_alert_dialog_new("Speed Limit", nullptr);
    AdwAlertDialog* ad = ADW_ALERT_DIALOG(dlg);
    adw_alert_dialog_format_body(ad, "Cap “%s”. Leave empty for no limit.", display_name(*it).c_str());
    adw_alert_dialog_add_response(ad, "cancel", "_Cancel");
    adw_alert_dialog_add_response(ad, "apply", "_Set Limit");
    adw_alert_dialog_set_response_appearance(ad, "apply", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ad, "apply");
    adw_alert_dialog_set_close_response(ad, "cancel");

    GtkWidget* opts = boxed_list();
    GtkWidget* entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), "Limit, e.g. 500k or 2m");
    gtk_editable_set_text(GTK_EDITABLE(entry), rate_input((double)it->max_rate_bps).c_str());
    adw_entry_row_set_activates_default(ADW_ENTRY_ROW(entry), TRUE);
    gtk_list_box_append(GTK_LIST_BOX(opts), entry);
    adw_alert_dialog_set_extra_child(ad, opts);

    on<const char*>(dlg, "response", [this, id, entry](const char* response) {
        if (std::string(response) != "apply") return;
        try {
            long long bps = parse_rate(gtk_editable_get_text(GTK_EDITABLE(entry)));
            manager_->set_item_rate(id, bps);
            toast(bps ? "Limited to " + human_speed((double)bps) : std::string("Speed limit removed"));
        } catch (...) {
            toast("Couldn’t read that speed — try 500k or 2m");
        }
        refresh();
    });
    adw_dialog_set_focus(dlg, entry);
    adw_dialog_present(dlg, dialog_parent());
}

// ---- "file already exists" -------------------------------------------------------------

void App::ask_duplicate(const std::string& path, std::function<void(std::string)> done) {
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    AdwDialog* dlg = adw_alert_dialog_new("File Already Exists", nullptr);
    AdwAlertDialog* ad = ADW_ALERT_DIALOG(dlg);
    adw_alert_dialog_format_body(ad, "“%s” is already in the download folder. Keep both files, "
                                     "replace the existing one, or skip this download?", name.c_str());
    adw_alert_dialog_add_response(ad, "skip", "_Skip");
    adw_alert_dialog_add_response(ad, "overwrite", "_Replace");
    adw_alert_dialog_add_response(ad, "rename", "_Keep Both");
    adw_alert_dialog_set_response_appearance(ad, "overwrite", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ad, "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ad, "rename");
    adw_alert_dialog_set_close_response(ad, "rename");
    on<const char*>(dlg, "response", [done](const char* response) { done(response); });
    present_main();
    adw_dialog_present(dlg, dialog_parent());
}

// ---- about -------------------------------------------------------------------------

void App::show_about() {
    AdwDialog* dlg = adw_about_dialog_new();
    AdwAboutDialog* ab = ADW_ABOUT_DIALOG(dlg);
    adw_about_dialog_set_application_name(ab, "MyPersonalDownloadManager");
    adw_about_dialog_set_application_icon(ab, "mpdm");
    adw_about_dialog_set_developer_name(ab, "MPDM");
    adw_about_dialog_set_version(ab, "1.0.0");
    adw_about_dialog_set_comments(ab, "A multi-connection download manager with resume, scheduling, "
                                      "speed limits and HLS/DASH stream grabbing.");
    adw_about_dialog_set_license_type(ab, GTK_LICENSE_MIT_X11);
    adw_dialog_present(dlg, dialog_parent());
}

} // namespace mpdm::gtk
