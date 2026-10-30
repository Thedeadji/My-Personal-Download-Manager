// Settings — the subset of mpdm/config.py the engine/manager need.
#pragma once
#include <map>
#include <string>

namespace mpdm {

inline constexpr const char* DEFAULT_USER_AGENT = "MyPersonalDownloadManager/0.1 (+research)";
inline constexpr const char* INCOGNITO_DIRNAME = "InvisibleFile";
inline constexpr const char* BROWSER_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

// The user's real Downloads folder (%USERPROFILE%\Downloads, ~/Downloads),
// used as the default output directory. Always absolute, so a download never
// depends on the process's working directory — the native host launches the app
// from its install folder (e.g. C:\Program Files\MPDM), which isn't writable.
std::string default_download_dir();

struct Settings {
    int connections = 8;
    long long min_segment_size = 256 * 1024;   // 256 KiB
    long connect_timeout = 15;                  // seconds
    long read_timeout = 60;                     // seconds (libcurl low-speed window)
    int max_retries = 5;
    double retry_backoff = 1.5;
    long retry_interval = 0;  // fixed seconds between retries; 0 = exponential backoff
    std::string user_agent = DEFAULT_USER_AGENT;
    std::string output_dir = default_download_dir();
    bool verify_tls = true;
    std::string proxy;
    long long max_rate_bps = 0;                 // 0 = unlimited (per-download)
    bool categorize = true;
    std::map<std::string, std::string> category_dirs;
    // User-defined/extra file types per category (name -> space-separated extensions,
    // no dots). These take priority over the built-in map, and a name that isn't a
    // built-in category becomes a new custom category (its folder is its name, or a
    // category_dirs override).
    std::map<std::string, std::string> category_exts;
    // Per-category "remember the folder I last saved into": when on, a completed
    // download whose folder differs from the category default updates that default.
    std::map<std::string, bool> remember_category_folder;
    // Download parts here, then move the finished file to its real folder. "" = off
    // (download straight into the destination, as before).
    std::string temp_dir;
    // Stamp finished files with the server's Last-Modified date.
    bool set_file_times = false;
    bool dynamic_segmentation = true;
    // What to do when the target file already exists (and no resumable partial
    // matches): "rename" (save as name(1).ext — default), "overwrite" (replace
    // it), "skip" (keep the existing file), or "ask" (prompt each time).
    std::string duplicate_policy = "rename";
    // When resuming, ignore a changed server modification time (Last-Modified);
    // only the size + ETag must still match. Off = a changed date restarts it.
    bool resume_ignore_mtime = true;
};

// Map a UA choice to a header string: ""/"default"/"honest" -> honest default;
// "browser"/"chrome" -> the browser preset; anything else is used verbatim.
std::string resolve_user_agent(const std::string& value);

// Overlay MDM_* environment variables onto an existing Settings (env wins).
void apply_env(Settings& s);

// Overlay MDM_* environment variables onto defaults (env wins). Mirrors
// Settings.from_env() in the Python config.
Settings settings_from_env();

} // namespace mpdm
