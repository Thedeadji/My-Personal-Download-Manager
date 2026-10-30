#include "util.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "mpdm/categories.hpp"

namespace fs = std::filesystem;

namespace mpdm::gtk {

std::string human_size(double n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, i ? "%.1f %s" : "%.0f %s", n, units[i]);
    return buf;
}

std::string human_speed(double bps) { return human_size(bps) + "/s"; }

std::string human_eta(double s) {
    if (s <= 0 || !std::isfinite(s)) return "";
    long t = static_cast<long>(s);
    long h = t / 3600, m = (t % 3600) / 60;
    char buf[48];
    if (h) std::snprintf(buf, sizeof buf, "%ld h %ld min left", h, m);
    else if (m) std::snprintf(buf, sizeof buf, "%ld min left", m);
    else std::snprintf(buf, sizeof buf, "%ld s left", t);
    return buf;
}

std::string rate_input(double bps) {
    if (bps <= 0) return "";
    char buf[32];
    if (bps >= 1024.0 * 1024.0) std::snprintf(buf, sizeof buf, "%gm", std::round(bps / 1048576.0 * 100) / 100);
    else std::snprintf(buf, sizeof buf, "%gk", std::round(bps / 1024.0 * 100) / 100);
    return buf;
}

std::string url_host(const std::string& url) {
    auto p = url.find("://");
    std::string rest = p == std::string::npos ? url : url.substr(p + 3);
    auto end = rest.find_first_of("/?#");
    rest = rest.substr(0, end);
    if (auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
    return rest;
}

std::string parent_dir(const std::string& path) {
    return path.empty() ? std::string() : fs::path(path).parent_path().string();
}

std::string display_name(const DownloadItem& it) {
    if (!it.filename.empty()) return it.filename;
    if (!it.dest_path.empty()) return fs::path(it.dest_path).filename().string();
    std::string u = it.url.substr(0, it.url.find_first_of("?#"));
    auto slash = u.find_last_of('/');
    std::string seg = slash == std::string::npos ? u : u.substr(slash + 1);
    return seg.empty() ? it.url : seg;
}

std::string item_category(const DownloadItem& it, const std::map<std::string, std::string>& extra) {
    return classify(!it.filename.empty() ? it.filename : it.url, extra);
}

const char* category_icon(const std::string& c) {
    if (c == "Video") return "video-x-generic-symbolic";
    if (c == "Audio") return "audio-x-generic-symbolic";
    if (c == "Documents") return "x-office-document-symbolic";
    if (c == "Archives") return "package-x-generic-symbolic";
    if (c == "Programs") return "application-x-executable-symbolic";
    return "text-x-generic-symbolic";
}

void run_on_main(std::function<void()> fn) {
    using Fn = std::function<void()>;
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer d) -> gboolean { (*static_cast<Fn*>(d))(); return G_SOURCE_REMOVE; },
        new Fn(std::move(fn)), [](gpointer d) { delete static_cast<Fn*>(d); });
}

void open_file(GtkWindow* parent, const std::string& path) {
    if (path.empty()) return;
    GFile* f = g_file_new_for_path(path.c_str());
    GtkFileLauncher* l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, parent, nullptr, nullptr, nullptr);
    g_object_unref(l);
    g_object_unref(f);
}

void show_in_folder(GtkWindow* parent, const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    GFile* f = g_file_new_for_path(path.c_str());
    GtkFileLauncher* l = gtk_file_launcher_new(f);
    if (fs::exists(path, ec)) {
        gtk_file_launcher_open_containing_folder(l, parent, nullptr, nullptr, nullptr);
    } else {
        // Not on disk yet (still downloading into a temp dir): open the folder itself.
        GFile* dir = g_file_new_for_path(parent_dir(path).c_str());
        gtk_file_launcher_set_file(l, dir);
        gtk_file_launcher_launch(l, parent, nullptr, nullptr, nullptr);
        g_object_unref(dir);
    }
    g_object_unref(l);
    g_object_unref(f);
}

void copy_to_clipboard(const std::string& text) {
    GdkClipboard* cb = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(cb, text.c_str());
}

bool save_widget_png(GtkWidget* widget, const std::string& path) {
    int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
    if (w <= 0 || h <= 0) return false;
    GtkSnapshot* snap = gtk_snapshot_new();
    // A toplevel paints its own background and its paintable draws nothing, so
    // lay the Adwaita window color down and draw each child at its position.
    GdkRGBA bg;
    gdk_rgba_parse(&bg, adw_style_manager_get_dark(adw_style_manager_get_default()) ? "#222226" : "#fafafb");
    graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, (float)w, (float)h);
    gtk_snapshot_append_color(snap, &bg, &all);
    for (GtkWidget* c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
        graphene_rect_t b;
        if (!gtk_widget_get_visible(c) || !gtk_widget_compute_bounds(c, widget, &b)) continue;
        GdkPaintable* p = gtk_widget_paintable_new(c);
        gtk_snapshot_save(snap);
        graphene_point_t at = GRAPHENE_POINT_INIT(b.origin.x, b.origin.y);
        gtk_snapshot_translate(snap, &at);
        gdk_paintable_snapshot(p, snap, b.size.width, b.size.height);
        gtk_snapshot_restore(snap);
        g_object_unref(p);
    }
    GskRenderNode* node = gtk_snapshot_free_to_node(snap);
    bool ok = false;
    if (node) {
        // The window's own renderer gives a faithful image; Cairo is the fallback.
        GskRenderer* r = gtk_native_get_renderer(gtk_widget_get_native(widget));
        if (GdkTexture* tex = r ? gsk_renderer_render_texture(r, node, &all) : nullptr) {
            ok = gdk_texture_save_to_png(tex, path.c_str());
            g_object_unref(tex);
        } else {
            GskRenderer* cr = gsk_cairo_renderer_new();
            if (gsk_renderer_realize_for_display(cr, gtk_widget_get_display(widget), nullptr)) {
                if (GdkTexture* t = gsk_renderer_render_texture(cr, node, &all)) {
                    ok = gdk_texture_save_to_png(t, path.c_str());
                    g_object_unref(t);
                }
                gsk_renderer_unrealize(cr);
            }
            g_object_unref(cr);
        }
        gsk_render_node_unref(node);
    }
    return ok;
}

} // namespace mpdm::gtk
