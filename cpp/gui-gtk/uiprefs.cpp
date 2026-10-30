#include "uiprefs.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "mpdm/manager.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace mpdm::gtk {

static fs::path prefs_path() { return fs::path(state_dir()) / "gui-gtk.json"; }

UiPrefs UiPrefs::load() {
    UiPrefs p;
    std::ifstream f(prefs_path());
    if (!f) return p;
    json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return p;
    p.appearance = j.value("appearance", p.appearance);
    p.ask_on_remove = j.value("ask_on_remove", p.ask_on_remove);
    p.remove_delete_default = j.value("remove_delete_default", p.remove_delete_default);
    p.notify_finish = j.value("notify_finish", p.notify_finish);
    p.show_popups = j.value("show_popups", p.show_popups);
    p.autostart = j.value("autostart", p.autostart);
    p.background_hint_shown = j.value("background_hint_shown", p.background_hint_shown);
    return p;
}

void UiPrefs::save() const {
    json j = {
        {"appearance", appearance},
        {"ask_on_remove", ask_on_remove},
        {"remove_delete_default", remove_delete_default},
        {"notify_finish", notify_finish},
        {"show_popups", show_popups},
        {"autostart", autostart},
        {"background_hint_shown", background_hint_shown},
    };
    fs::path p = prefs_path(), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << j.dump(2);
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

static fs::path autostart_file() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config";
    return base / "autostart" / "com.mypersonaldownloadmanager.MPDM.desktop";
}

bool login_autostart_enabled() {
    std::error_code ec;
    return fs::exists(autostart_file(), ec);
}

void set_login_autostart(bool on, const std::string& exe_path) {
    fs::path f = autostart_file();
    std::error_code ec;
    if (!on) { fs::remove(f, ec); return; }
    fs::create_directories(f.parent_path(), ec);
    std::ofstream out(f, std::ios::trunc);
    out << "[Desktop Entry]\nType=Application\nName=MyPersonalDownloadManager\n"
        << "Exec=" << exe_path << " --background\n"
        << "Icon=mpdm\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
}

} // namespace mpdm::gtk
