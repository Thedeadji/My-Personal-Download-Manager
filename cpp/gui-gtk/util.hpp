// Small helpers shared by the GTK4 / libadwaita GUI: formatting, bridging the
// libadwaita C API into gtkmm, and marshalling work onto the GTK main thread.
#pragma once
#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "mpdm/manager.hpp"

namespace mpdm::gtk {

// ---- formatting -------------------------------------------------------------
std::string human_size(double bytes);           // "1.2 GB"
std::string human_speed(double bps);            // "9.4 MB/s"
std::string human_eta(double seconds);          // "4 min left"
std::string rate_input(double bps);             // editable form parse_rate accepts: "1.5m", "" = none
std::string display_name(const DownloadItem& it);  // filename, else dest/URL basename
std::string url_host(const std::string& url);   // "releases.ubuntu.com"
std::string parent_dir(const std::string& path);

// Symbolic icon for an item's file category (video, audio, …).
const char* category_icon(const std::string& category);
std::string item_category(const DownloadItem& it,   // honors user-defined types
                          const std::map<std::string, std::string>& extra);

// ---- libadwaita <-> gtkmm ---------------------------------------------------
// Wrap a freshly created (floating) C widget so gtkmm containers can hold it.
inline Gtk::Widget* W(GtkWidget* w) { return Gtk::manage(Glib::wrap(w)); }

// g_signal_connect with a C++ callable. The template arguments are the signal's
// parameters after the emitting instance, e.g. on<const char*>(dlg, "response", …).
template <typename R, typename... Args>
struct SignalThunk {
    std::function<R(Args...)> fn;
    static R call(gpointer, Args... a, gpointer data) { return static_cast<SignalThunk*>(data)->fn(a...); }
    static void destroy(gpointer data, GClosure*) { delete static_cast<SignalThunk*>(data); }
};

template <typename... Args, typename F>
gulong on(gpointer instance, const char* signal, F&& f) {
    using T = SignalThunk<void, Args...>;
    auto* t = new T{std::forward<F>(f)};
    return g_signal_connect_data(instance, signal, G_CALLBACK(&T::call), t, &T::destroy, GConnectFlags(0));
}

// Same, for signals whose handler returns a value (e.g. "close-request").
template <typename R, typename... Args, typename F>
gulong on_r(gpointer instance, const char* signal, F&& f) {
    using T = SignalThunk<R, Args...>;
    auto* t = new T{std::forward<F>(f)};
    return g_signal_connect_data(instance, signal, G_CALLBACK(&T::call), t, &T::destroy, GConnectFlags(0));
}

// Queue `fn` on the default main context. Safe to call from any thread.
void run_on_main(std::function<void()> fn);

// ---- desktop integration ----------------------------------------------------
void open_file(GtkWindow* parent, const std::string& path);         // default app
void show_in_folder(GtkWindow* parent, const std::string& path);    // file manager, file selected
void copy_to_clipboard(const std::string& text);

// Render a widget (and everything inside it) to a PNG. Dev/screenshot use only.
bool save_widget_png(GtkWidget* widget, const std::string& path);

} // namespace mpdm::gtk
