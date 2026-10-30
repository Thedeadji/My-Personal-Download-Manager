// GUI-only preferences (appearance, prompts, popups). Engine settings live in the
// shared settings.json via DownloadManager; these are kept beside it in
// gui-gtk.json so the Qt and GTK front-ends don't fight over each other's keys.
#pragma once
#include <string>

namespace mpdm::gtk {

struct UiPrefs {
    std::string appearance = "system";  // system | light | dark
    bool ask_on_remove = true;
    bool remove_delete_default = false;
    bool notify_finish = true;          // toast / desktop notification on completion
    bool show_popups = true;            // open the progress window when a download starts
    bool autostart = true;              // start new downloads right away
    bool background_hint_shown = false; // "still running in the background" told once

    static UiPrefs load();
    void save() const;
};

// "Run in the background at login" — a ~/.config/autostart entry.
bool login_autostart_enabled();
void set_login_autostart(bool on, const std::string& exe_path);

} // namespace mpdm::gtk
