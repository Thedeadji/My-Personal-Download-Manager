// App stylesheet. Everything builds on libadwaita's named colors and CSS
// variables, so light/dark and the user's accent color apply automatically.
#pragma once
#include <gtk/gtk.h>

namespace mpdm::gtk {

inline constexpr const char* kAppCss = R"css(
/* ---- download rows ---- */
.dl-row { padding: 12px 14px; }
.dl-name { font-weight: 600; }
.dl-icon {
  min-width: 40px; min-height: 40px; border-radius: 12px;
  color: var(--accent-color);
  background-color: color-mix(in srgb, var(--accent-bg-color) 14%, transparent);
}
.dl-icon.large { min-width: 56px; min-height: 56px; border-radius: 16px; }
.dl-icon.completed { color: var(--success-color); background-color: color-mix(in srgb, var(--success-bg-color) 16%, transparent); }
.dl-icon.error     { color: var(--error-color);   background-color: color-mix(in srgb, var(--error-bg-color) 16%, transparent); }
.dl-icon.paused    { color: var(--warning-color); background-color: color-mix(in srgb, var(--warning-bg-color) 18%, transparent); }
.dl-icon.queued    { color: alpha(currentColor, .7); background-color: alpha(currentColor, .08); }

.status-pill {
  font-size: 0.75em; font-weight: 700;
  padding: 1px 8px; border-radius: 999px;
  color: alpha(currentColor, .7); background-color: alpha(currentColor, .08);
}
.status-pill.downloading { color: var(--accent-color);  background-color: color-mix(in srgb, var(--accent-bg-color) 15%, transparent); }
.status-pill.paused      { color: var(--warning-color); background-color: color-mix(in srgb, var(--warning-bg-color) 18%, transparent); }
.status-pill.completed   { color: var(--success-color); background-color: color-mix(in srgb, var(--success-bg-color) 16%, transparent); }
.status-pill.error       { color: var(--error-color);   background-color: color-mix(in srgb, var(--error-bg-color) 16%, transparent); }

progressbar.dl-progress > trough,
progressbar.dl-progress > trough > progress { min-height: 6px; border-radius: 999px; }
progressbar.dl-progress.large > trough,
progressbar.dl-progress.large > trough > progress { min-height: 10px; }
progressbar.dl-progress.paused > trough > progress    { background-color: var(--warning-bg-color); }
progressbar.dl-progress.completed > trough > progress { background-color: var(--success-bg-color); }
progressbar.dl-progress.error > trough > progress     { background-color: var(--error-bg-color); }
progressbar.dl-progress.queued > trough > progress    { background-color: alpha(currentColor, .3); }

/* ---- overview card ---- */
.overview { padding: 18px 20px; }
.speed-figure { font-size: 2.6em; font-weight: 800; letter-spacing: -0.02em; }
.speed-unit { font-size: 1.1em; font-weight: 600; opacity: .55; margin-bottom: 7px; }
.legend-dot { min-width: 8px; min-height: 8px; border-radius: 999px; background-color: var(--accent-bg-color); }
.legend-dot.disk { background-color: alpha(currentColor, .35); }

/* ---- sidebar ---- */
.sidebar-count { font-size: .85em; opacity: .55; }
.sidebar-footer { padding: 12px 16px 14px 16px; }
levelbar.storage > trough > block.filled { background-color: var(--accent-bg-color); }

/* ---- progress window ---- */
.hero-name { font-size: 1.15em; font-weight: 700; }
.percent-figure { font-size: 1.6em; font-weight: 800; }
.lanes-card { padding: 14px 16px; }
)css";

inline void install_css() {
    GtkCssProvider* p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, kAppCss);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

} // namespace mpdm::gtk
