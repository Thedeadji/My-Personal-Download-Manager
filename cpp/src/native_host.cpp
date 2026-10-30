#include "mpdm/native_host.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "mpdm/categories.hpp"
#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/storage.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#else
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm {

// ---- framing ---------------------------------------------------------------

void write_frame(std::ostream& out, const std::string& body) {
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    char len[4] = {static_cast<char>(n & 0xFF), static_cast<char>((n >> 8) & 0xFF),
                   static_cast<char>((n >> 16) & 0xFF), static_cast<char>((n >> 24) & 0xFF)};
    out.write(len, 4);
    out.write(body.data(), static_cast<std::streamsize>(n));
    out.flush();
}

std::optional<std::string> read_frame(std::istream& in) {
    char len[4];
    in.read(len, 4);
    if (in.gcount() < 4) return std::nullopt;
    std::uint32_t n = static_cast<std::uint8_t>(len[0]) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len[1])) << 8) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len[2])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(len[3])) << 24);
    std::string data(n, '\0');
    if (n) {
        in.read(&data[0], static_cast<std::streamsize>(n));
        if (static_cast<std::uint32_t>(in.gcount()) < n) return std::nullopt;
    }
    return data;
}

// ---- output (thread-safe) + logging ---------------------------------------

static std::mutex g_write_mu;

static void send(const json& obj) {
    std::string body = obj.dump();
    std::lock_guard<std::mutex> lk(g_write_mu);
    write_frame(std::cout, body);
}

static void log_line(const std::string& msg) {
    try {
        fs::path p = fs::path(state_dir()) / "native_host.log";
        std::ofstream f(p, std::ios::app);
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        f << buf << "  " << msg << "\n";
    } catch (...) {
    }
}

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool is_stream_job(const std::string& url, bool stream_flag) {
    if (stream_flag) return true;
    std::string low = url;
    for (auto& c : low) c = static_cast<char>(::tolower(c));
    return low.find(".m3u8") != std::string::npos || low.find(".mpd") != std::string::npos;
}

static std::string status_name(Status s) {
    switch (s) {
        case Status::Pending: return "pending";
        case Status::Probing: return "probing";
        case Status::Downloading: return "downloading";
        case Status::Paused: return "paused";
        case Status::Merging: return "merging";
        case Status::Completed: return "completed";
        case Status::Failed: return "failed";
    }
    return "";
}

// ---- job handling (headless fallback) -------------------------------------

static void run_job_headless(json job) {
    std::string job_id = job.value("id", "");
    if (job_id.empty()) job_id = std::to_string(static_cast<long long>(now_seconds() * 1000));
    std::string url = job.value("url", "");
    if (url.empty()) {
        send({{"type", "error"}, {"id", job_id}, {"message", "no url"}});
        return;
    }

    // Load the user's saved settings (so the duplicate-file policy, categories, etc.
    // match the app), with env overlaid. "ask" can't prompt headlessly -> rename.
    Settings settings = load_user_settings();
    if (job.contains("connections") && !job["connections"].is_null()) {
        try {
            settings.connections = std::max(1, job["connections"].get<int>());
        } catch (...) {
        }
    }
    if (job.contains("output_dir") && job["output_dir"].is_string())
        settings.output_dir = job["output_dir"].get<std::string>();

    std::string dest;  // empty -> the engine resolves the folder + applies the dup policy
    std::string preferred;
    if (job.contains("filename") && job["filename"].is_string() &&
        !job["filename"].get<std::string>().empty())
        preferred = sanitize_filename(job["filename"].get<std::string>());

    std::map<std::string, std::string> headers;
    if (job.contains("headers") && job["headers"].is_object())
        for (auto it = job["headers"].begin(); it != job["headers"].end(); ++it)
            if (it.value().is_string()) headers[it.key()] = it.value().get<std::string>();

    bool stream_flag = job.value("stream", false);
    double last_emit = 0.0;

    try {
        std::string path;
        if (is_stream_job(url, stream_flag)) {
            std::string base = preferred.empty() ? "video.mp4" : preferred;
            std::string low = base;
            for (auto& c : low) c = static_cast<char>(::tolower(c));
            if (!(low.size() >= 4 &&
                  (low.rfind(".mp4") == low.size() - 4 || low.rfind(".mkv") == low.size() - 4 ||
                   low.rfind(".webm") == low.size() - 5)))
                base += ".mp4";
            std::string out = dest;
            if (out.empty()) {
                std::string dir = categorized_dir(settings.output_dir, base, settings.categorize,
                                                  settings.category_dirs, settings.category_exts);
                std::error_code ec; fs::create_directories(dir, ec);
                out = unique_dest(dir, base);
            }
            GrabProgressCb cb = [&](int done, int total) {
                double now = now_seconds();
                if (now - last_emit < 0.3) return;
                last_emit = now;
                json m = {{"type", "progress"}, {"id", job_id}, {"downloaded", done},
                          {"total", total}, {"status", "downloading"}};
                m["percent"] = total ? json(done * 100.0 / total) : json(nullptr);
                send(m);
            };
            path = grab(url, out, settings, "auto", "best", cb);
        } else {
            ProgressCb cb = [&](const Progress& p) {
                double now = now_seconds();
                if (p.status != Status::Completed && p.status != Status::Failed &&
                    (now - last_emit) < 0.3)
                    return;
                last_emit = now;
                json m = {{"type", "progress"},
                          {"id", job_id},
                          {"speed", p.speed_bps},
                          {"downloaded", p.downloaded},
                          {"status", status_name(p.status)},
                          {"filename", p.filename}};
                m["percent"] = p.percent() ? json(*p.percent()) : json(nullptr);
                m["total"] = p.total ? json(*p.total) : json(nullptr);
                send(m);
            };
            Download dl(url, settings, dest, cb, headers);
            if (dest.empty() && !preferred.empty()) dl.set_preferred_filename(preferred);
            path = dl.start();
        }
        send({{"type", "done"}, {"id", job_id}, {"path", path}});
        log_line("done " + job_id + " -> " + path);
    } catch (const std::exception& e) {
        send({{"type", "error"}, {"id", job_id}, {"message", e.what()}});
        log_line(std::string("error ") + job_id + ": " + e.what());
    }
}

// ---- hand off to the app (Windows) -----------------------------------------
//
// Browser downloads are handed to the main app: the host drops the job in jobs/
// and launches the GUI (single-instance, so a running copy adopts it). The app
// owns the download (shows it in the queue and opens the Industrial popup), so
// the host returns immediately.

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Directory that holds this host exe (the GUI lives alongside it when installed).
static fs::path host_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return fs::current_path();
    return fs::path(std::wstring(buf, n)).parent_path();
}

static fs::path find_gui() {
    fs::path d = host_dir();
    for (const char* name : {"MPDM.exe", "mpdm_gui.exe"}) {
        std::error_code ec;
        fs::path p = d / name;
        if (fs::exists(p, ec)) return p;
    }
    // Dev tree: ...\cpp\build\Release\mpdm_host.exe -> ...\cpp\build-gui\gui\Release\mpdm_gui.exe
    return d / "mpdm_gui.exe";
}

// Drop the job for the app to adopt, then launch the GUI (single-instance) so a
// running copy picks it up or a new one starts. Returns false if the GUI exe is
// missing (caller falls back to a headless download).
static bool hand_off_to_app(const json& job, const std::string& job_id) {
    fs::path gui = find_gui();
    std::error_code ec;
    if (!fs::exists(gui, ec)) {
        log_line("handoff: GUI not found at " + gui.string());
        return false;
    }
    fs::path jobsDir = fs::path(state_dir()) / "jobs";
    fs::create_directories(jobsDir, ec);
    fs::path jobFile = jobsDir / (job_id + ".json");
    try {
        fs::path tmp = jobFile; tmp += ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << job.dump(); }
        fs::rename(tmp, jobFile, ec);
    } catch (...) {
        return false;
    }

    std::wstring cmd = L"\"" + gui.wstring() + L"\" --background";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Browsers run the host inside a job object and kill the whole job when the
    // port closes (Firefox always does). Break the app out of it so it survives;
    // if the job doesn't allow breakaway, launch it normally.
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
    if (!ok)
        ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    else log_line("handoff: CreateProcess failed (" + std::to_string(GetLastError()) + ")");
    log_line("handoff job " + job_id + " -> " + gui.string());
    return true;  // job file written; a running instance will adopt it regardless
}

#else  // ---- POSIX (Linux) hand-off: same model as Windows --------------------

static fs::path host_dir() {
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : exe.parent_path();
}

// Search $PATH for an executable name.
static fs::path which(const std::string& name) {
    const char* p = std::getenv("PATH");
    if (!p) return {};
    std::string path = p;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        fs::path cand = fs::path(path.substr(start, end - start)) / name;
        if (!cand.empty() && ::access(cand.c_str(), X_OK) == 0) return cand;
        start = end + 1;
    }
    return {};
}

// The running AppImage file ("" when not inside one). Its runtime sets $APPIMAGE;
// the host's own path is a temporary mount that disappears when it exits.
static std::string appimage_file() {
    const char* a = std::getenv("APPIMAGE");
    return (a && *a) ? a : "";
}

// The GUI sits next to the host when installed (/usr/bin), or one level down in a
// build tree (build-linux/mpdm_host + build-linux/gui/mpdm_gui). Either desktop
// front-end will do: the Qt one (mpdm_gui) or the GTK one (mpdm_gtk). Inside an
// AppImage the GUI is the AppImage itself, launched through its own runtime.
static fs::path find_gui() {
    if (std::string a = appimage_file(); !a.empty()) return a;
    fs::path d = host_dir();
    for (const char* name : {"mpdm_gui", "mpdm_gtk"}) {
        for (const fs::path& p : {d / name, d / "gui" / name, d / "gui-gtk" / name, d.parent_path() / "bin" / name})
            if (::access(p.c_str(), X_OK) == 0) return p;
    }
    for (const char* name : {"mpdm_gui", "mpdm_gtk"})
        if (fs::path p = which(name); !p.empty()) return p;
    return {};
}

static bool hand_off_to_app(const json& job, const std::string& job_id) {
    fs::path gui = find_gui();
    if (gui.empty()) {
        log_line("handoff: no MPDM desktop app (mpdm_gui / mpdm_gtk) next to the host or on PATH");
        return false;
    }
    std::error_code ec;
    fs::path jobsDir = fs::path(state_dir()) / "jobs";
    fs::create_directories(jobsDir, ec);
    fs::path jobFile = jobsDir / (job_id + ".json");
    try {
        fs::path tmp = jobFile; tmp += ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << job.dump(); }
        fs::rename(tmp, jobFile, ec);
    } catch (...) {
        return false;
    }

    // Launch the GUI fully detached (double fork + setsid) so the browser closing
    // this host — which can signal our process group — never takes the app down.
    // A running single-instance copy just adopts the job file and the new one exits.
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        if (::fork() == 0) {
            // Don't inherit the native-messaging pipes.
            int nul = ::open("/dev/null", 2);
            if (nul >= 0) { ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2); if (nul > 2) ::close(nul); }
            ::execl(gui.c_str(), gui.c_str(), "--background", (char*)nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    if (pid > 0) ::waitpid(pid, nullptr, 0);
    else log_line("handoff: fork failed");
    log_line("handoff job " + job_id + " -> " + gui.string());
    return true;
}

// Linux "Save as": use the desktop's own dialog via zenity (GNOME/GTK) or kdialog
// (KDE). Returns "" if neither is available or the user cancels.
static std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    int rc = ::pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return rc == 0 ? out : std::string();
}

static std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

static std::string browse_save_as(const std::string& suggested) {
    std::string dir = default_download_dir();
    std::string start = (fs::path(dir) / (suggested.empty() ? "download" : suggested)).string();
    if (!which("zenity").empty())
        return run_capture("zenity --file-selection --save --confirm-overwrite --title='Save as' --filename=" +
                           shell_quote(start) + " 2>/dev/null");
    if (!which("kdialog").empty())
        return run_capture("kdialog --getsavefilename " + shell_quote(start) + " 2>/dev/null");
    log_line("browse: neither zenity nor kdialog is installed");
    return "";
}

#endif  // _WIN32

#ifdef _WIN32
// Open the system "Save As" dialog and return the chosen full path (UTF-8), or ""
// if the user cancels. The browser popup can't show a native dialog, so it asks
// the host to do it.
static std::string browse_save_as(const std::string& suggested_utf8) {
    std::string result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool did_init = SUCCEEDED(hr);
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        if (!suggested_utf8.empty()) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, suggested_utf8.c_str(), -1, nullptr, 0);
            if (wn > 0) {
                std::wstring w(wn - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, suggested_utf8.c_str(), -1, w.data(), wn);
                dlg->SetFileName(w.c_str());
            }
        }
        DWORD opts = 0; dlg->GetOptions(&opts); dlg->SetOptions(opts | FOS_OVERWRITEPROMPT);
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                    if (n > 0) { std::string s(n - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), n, nullptr, nullptr);
                        result = s; }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (did_init) CoUninitialize();
    return result;
}
#endif  // _WIN32

static void run_job(json job) {
    std::string job_id = job.value("id", "");
    if (job_id.empty()) job_id = std::to_string(static_cast<long long>(now_seconds() * 1000));
    if (job.value("url", "").empty()) {
        send({{"type", "error"}, {"id", job_id}, {"message", "no url"}});
        return;
    }
    job["id"] = job_id;
    send({{"type", "accepted"}, {"id", job_id}, {"filename", job.value("filename", "")}});
    if (hand_off_to_app(job, job_id)) {
        send({{"type", "done"}, {"id", job_id}});  // handed to the app, which owns it
        return;
    }
    // GUI not found -> download headlessly so the request isn't lost.
    log_line("app unavailable; headless fallback " + job_id);
    run_job_headless(job);
}

// ---- serve loop ------------------------------------------------------------

static int serve() {
    log_line("host started");
    std::vector<std::thread> threads;
    for (;;) {
        auto body = read_frame(std::cin);
        if (!body) break;
        json msg;
        try {
            msg = json::parse(*body);
        } catch (const std::exception& e) {
            send({{"type", "error"}, {"message", std::string("bad json: ") + e.what()}});
            continue;
        }
        std::string type = msg.value("type", "");
        if (type == "ping") {
            send({{"type", "pong"}});
        } else if (type == "browse") {
            send({{"type", "browsed"}, {"path", browse_save_as(msg.value("filename", ""))}});
        } else if (type == "download") {
            threads.emplace_back(run_job, msg);
        } else {
            send({{"type", "error"}, {"message", "unknown type: " + type}});
        }
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
    log_line("host stopping");
    return 0;
}

// ---- native-messaging host registration ------------------------------------
//
// Two independent lists are kept, so several browsers can be connected at once:
//   - Chromium family (Chrome, Edge, Brave, Vivaldi, Chromium, Opera): extension
//     IDs go into one manifest's "allowed_origins"; every Chromium browser's
//     registry key (Windows) / config folder (Linux) points at that manifest.
//   - Firefox: add-on IDs go into a second manifest's "allowed_extensions"
//     (Firefox uses its own format and location).
// Registering an ID ADDS it to its list; unregistering one ID removes only that
// one. A list that becomes empty removes its manifest + registrations.

static const char* HOST_NAME = "com.mypersonaldownloadmanager.host";
// The extension's fixed Firefox add-on ID (manifest.json browser_specific_settings).
static const char* FIREFOX_EXT_ID = "mpdm@mypersonaldownloadmanager";

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Firefox IDs look like "name@domain" or "{uuid}"; Chromium IDs are 32 letters a-p.
static bool is_firefox_id(const std::string& id) {
    return id.find('@') != std::string::npos || (!id.empty() && id.front() == '{');
}
// Accept "chrome-extension://<id>/" as well as the bare ID.
static std::string normalize_chromium_id(std::string id) {
    id = trim(id);
    const std::string pfx = "chrome-extension://";
    if (id.rfind(pfx, 0) == 0) id = id.substr(pfx.size());
    while (!id.empty() && id.back() == '/') id.pop_back();
    return id;
}
static bool valid_chromium_id(const std::string& id) {
    if (id.size() != 32) return false;
    for (char c : id) if (c < 'a' || c > 'p') return false;
    return true;
}

// Read the extension ID from the console when none was passed on the command
// line (the installer's "Register browser host" shortcut / a bare --register).
static std::string prompt_ext_id() {
    std::cout << "\nLoad the MPDM extension in your browser, then paste its ID below.\n"
                 "  Chrome/Edge/Brave: chrome://extensions -> Developer mode -> Load unpacked\n"
                 "                     -> the 'extension' folder next to this app; copy the ID.\n"
                 "  Firefox:           type  firefox  (its add-on ID is fixed).\n\nExtension ID: "
              << std::flush;
    std::string id;
    std::getline(std::cin, id);
    id = trim(id);
    if (id == "firefox" || id == "Firefox") id = FIREFOX_EXT_ID;
    return id;
}

static fs::path self_exe() {
#ifdef _WIN32
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    return fs::path(std::wstring(b, n));
#else
    // Browsers launch whatever the manifest names; for an AppImage that must be the
    // .AppImage file (its AppRun routes browser launches to the host), not the
    // short-lived mount this binary runs from.
    if (const char* a = std::getenv("APPIMAGE"); a && *a) return fs::path(a);
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path("mpdm_host") : p;
#endif
}

static json read_json_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return json();
    try { json j; f >> j; return j; } catch (...) { return json(); }
}
static bool write_json_file(const fs::path& p, const json& j) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2);
    return bool(f);
}

static json host_manifest(const char* listKey, const json& list) {
    return {{"name", HOST_NAME},
            {"description", "MyPersonalDownloadManager native host"},
            {"path", self_exe().string()},
            {"type", "stdio"},
            {listKey, list}};
}

#ifdef _WIN32
// Manifests live in %LOCALAPPDATA%\MPDM so registering needs no admin even when
// the exe is in Program Files; registry keys under HKCU point at them.
static fs::path manifest_dir() {
    const char* lad = std::getenv("LOCALAPPDATA");
    return lad ? fs::path(lad) / "MPDM" : self_exe().parent_path();
}
static fs::path chromium_manifest() { return manifest_dir() / (std::string(HOST_NAME) + ".json"); }
static fs::path firefox_manifest() { return manifest_dir() / (std::string(HOST_NAME) + ".firefox.json"); }

static const wchar_t* kChromiumRegRoots[] = {
    L"Software\\Google\\Chrome",  // also read by Opera
    L"Software\\Microsoft\\Edge",
    L"Software\\Chromium",
    L"Software\\BraveSoftware\\Brave-Browser",
    L"Software\\Vivaldi",
};
static const wchar_t* kFirefoxRegRoot = L"Software\\Mozilla";

static std::wstring host_key(const wchar_t* root) {
    std::wstring wname(HOST_NAME, HOST_NAME + std::strlen(HOST_NAME));
    return std::wstring(root) + L"\\NativeMessagingHosts\\" + wname;
}
static bool reg_set_default(const std::wstring& subkey, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static std::vector<std::string> install_chromium(const json& m) {
    std::vector<std::string> done;
    if (!write_json_file(chromium_manifest(), m)) return done;
    for (auto* root : kChromiumRegRoots)
        if (reg_set_default(host_key(root), chromium_manifest().wstring()))
            done.push_back(fs::path(root).filename().string());
    return done;
}
static void uninstall_chromium() {
    for (auto* root : kChromiumRegRoots) RegDeleteKeyW(HKEY_CURRENT_USER, host_key(root).c_str());
    std::error_code ec;
    fs::remove(chromium_manifest(), ec);
}
static std::vector<std::string> install_firefox(const json& m) {
    std::vector<std::string> done;
    if (write_json_file(firefox_manifest(), m) &&
        reg_set_default(host_key(kFirefoxRegRoot), firefox_manifest().wstring()))
        done.push_back("Firefox");
    return done;
}
static void uninstall_firefox() {
    RegDeleteKeyW(HKEY_CURRENT_USER, host_key(kFirefoxRegRoot).c_str());
    std::error_code ec;
    fs::remove(firefox_manifest(), ec);
}

#else  // ---- Linux: manifests are files in each browser's config folder ----

static fs::path home_path() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path(".");
}
// Canonical copy of each list (what --list reads), kept in the MPDM state dir.
static fs::path chromium_manifest() { return fs::path(state_dir()) / "native-host" / "chromium.json"; }
static fs::path firefox_manifest() { return fs::path(state_dir()) / "native-host" / "firefox.json"; }

static const char* kChromiumDirs[] = {  // under ~/.config
    "google-chrome", "google-chrome-beta", "google-chrome-unstable", "chromium",
    "microsoft-edge", "BraveSoftware/Brave-Browser", "vivaldi", "opera",
};
static std::vector<fs::path> firefox_dirs() {  // regular and snap Firefox
    return {home_path() / ".mozilla", home_path() / "snap" / "firefox" / "common" / ".mozilla"};
}
static std::string manifest_name() { return std::string(HOST_NAME) + ".json"; }

static std::vector<std::string> install_chromium(const json& m) {
    std::vector<std::string> done;
    write_json_file(chromium_manifest(), m);
    std::error_code ec;
    for (auto* b : kChromiumDirs) {
        fs::path bdir = home_path() / ".config" / b;
        if (!fs::is_directory(bdir, ec)) continue;  // that browser isn't installed
        if (write_json_file(bdir / "NativeMessagingHosts" / manifest_name(), m)) done.push_back(b);
    }
    return done;
}
static void uninstall_chromium() {
    std::error_code ec;
    for (auto* b : kChromiumDirs)
        fs::remove(home_path() / ".config" / b / "NativeMessagingHosts" / manifest_name(), ec);
    fs::remove(chromium_manifest(), ec);
}
static std::vector<std::string> install_firefox(const json& m) {
    std::vector<std::string> done;
    write_json_file(firefox_manifest(), m);
    std::error_code ec;
    for (const auto& d : firefox_dirs()) {
        if (!fs::is_directory(d, ec)) continue;
        if (write_json_file(d / "native-messaging-hosts" / manifest_name(), m))
            done.push_back(d.string().find("snap") != std::string::npos ? "Firefox (snap)" : "Firefox");
    }
    return done;
}
static void uninstall_firefox() {
    std::error_code ec;
    for (const auto& d : firefox_dirs()) fs::remove(d / "native-messaging-hosts" / manifest_name(), ec);
    fs::remove(firefox_manifest(), ec);
}
#endif

// Currently registered IDs, read back from the canonical manifests.
static std::vector<std::string> registered_chromium() {
    std::vector<std::string> ids;
    json m = read_json_file(chromium_manifest());
#ifndef _WIN32
    // Older Linux builds only wrote per-browser manifests; pick those up too.
    if (!m.is_object())
        for (auto* b : kChromiumDirs) {
            m = read_json_file(home_path() / ".config" / b / "NativeMessagingHosts" / manifest_name());
            if (m.is_object()) break;
        }
#endif
    if (m.is_object() && m.contains("allowed_origins") && m["allowed_origins"].is_array())
        for (auto& o : m["allowed_origins"])
            if (o.is_string()) {
                std::string id = normalize_chromium_id(o.get<std::string>());
                if (!id.empty()) ids.push_back(id);
            }
    return ids;
}
static std::vector<std::string> registered_firefox() {
    std::vector<std::string> ids;
    json m = read_json_file(firefox_manifest());
    if (m.is_object() && m.contains("allowed_extensions") && m["allowed_extensions"].is_array())
        for (auto& o : m["allowed_extensions"])
            if (o.is_string()) ids.push_back(o.get<std::string>());
    return ids;
}

static void apply_chromium(const std::vector<std::string>& ids) {
    if (ids.empty()) { uninstall_chromium(); return; }
    json origins = json::array();
    for (auto& id : ids) origins.push_back("chrome-extension://" + id + "/");
    auto done = install_chromium(host_manifest("allowed_origins", origins));
    std::cout << "Chromium browsers (" << ids.size() << " ID" << (ids.size() == 1 ? "" : "s") << "): ";
    for (size_t i = 0; i < done.size(); ++i) std::cout << (i ? ", " : "") << done[i];
    std::cout << (done.empty() ? "none found" : "") << "\n";
}
static void apply_firefox(const std::vector<std::string>& ids) {
    if (ids.empty()) { uninstall_firefox(); return; }
    auto done = install_firefox(host_manifest("allowed_extensions", json(ids)));
    std::cout << "Firefox (" << ids.size() << " ID" << (ids.size() == 1 ? "" : "s") << "): ";
    for (size_t i = 0; i < done.size(); ++i) std::cout << (i ? ", " : "") << done[i];
    std::cout << (done.empty() ? "Firefox not found" : "") << "\n";
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// --register: add each ID to its list (Chromium or Firefox). No IDs -> prompt.
static int do_register(std::vector<std::string> ids, bool firefox) {
    if (firefox) ids.push_back(FIREFOX_EXT_ID);
    if (ids.empty()) {
        std::string p = prompt_ext_id();
        if (!p.empty()) ids.push_back(p);
    }
    if (ids.empty()) { std::cerr << "error: no extension ID given.\n"; return 2; }

    auto chromium = registered_chromium();
    auto ff = registered_firefox();
    for (auto raw : ids) {
        raw = trim(raw);
        if (is_firefox_id(raw)) {
            if (!has(ff, raw)) ff.push_back(raw);
            std::cout << "+ Firefox add-on " << raw << "\n";
        } else {
            std::string id = normalize_chromium_id(raw);
            if (!valid_chromium_id(id)) {
                std::cerr << "error: '" << raw << "' is not an extension ID (32 letters a-p, "
                             "shown on chrome://extensions).\n";
                return 2;
            }
            if (!has(chromium, id)) chromium.push_back(id);
            std::cout << "+ extension " << id << "\n";
        }
    }
    // Re-apply both lists so each manifest's exe path tracks this binary.
    if (!chromium.empty()) apply_chromium(chromium);
    if (!ff.empty()) apply_firefox(ff);
    return 0;
}

// --unregister: remove the given IDs (or everything when none are given).
static int do_unregister(const std::vector<std::string>& ids, bool firefox) {
    if (ids.empty() && !firefox) {
        uninstall_chromium();
        uninstall_firefox();
        std::cout << "unregistered native host (" << HOST_NAME << ") from all browsers\n";
        return 0;
    }
    auto chromium = registered_chromium();
    auto ff = registered_firefox();
    std::vector<std::string> gone = ids;
    if (firefox) gone.push_back(FIREFOX_EXT_ID);
    for (const auto& raw : gone) {
        std::string id = is_firefox_id(raw) ? trim(raw) : normalize_chromium_id(raw);
        chromium.erase(std::remove(chromium.begin(), chromium.end(), id), chromium.end());
        ff.erase(std::remove(ff.begin(), ff.end(), id), ff.end());
        std::cout << "- " << id << "\n";
    }
    apply_chromium(chromium);
    apply_firefox(ff);
    return 0;
}

// --list: machine-readable registrations (used by the app's Settings page).
static int do_list() {
    std::cout << json{{"chromium", registered_chromium()}, {"firefox", registered_firefox()}}.dump() << "\n";
    return 0;
}


static int selftest() {
    std::stringstream buf;
    write_frame(buf, json({{"type", "download"}, {"url", "https://example.com/x"},
                           {"connections", 4}})
                         .dump());
    buf.seekg(0);
    auto got = read_frame(buf);
    if (!got) {
        std::cerr << "selftest FAILED: no frame\n";
        return 1;
    }
    json j = json::parse(*got);
    if (j.value("type", "") != "download" || j.value("connections", 0) != 4) {
        std::cerr << "selftest FAILED: " << *got << "\n";
        return 1;
    }
    std::cout << "native messaging framing OK: " << *got << "\n";
    return 0;
}

int run_native_host(int argc, char** argv) {
    bool reg = false, unreg = false, list = false, firefox = false;
    std::vector<std::string> ids;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") return selftest();
        if (a == "--register") reg = true;
        else if (a == "--unregister") unreg = true;
        else if (a == "--list") list = true;
        else if (a == "--firefox") firefox = true;
        else if (a == "--extension-id" && i + 1 < argc) ids.push_back(argv[++i]);
        // --chrome / --edge (older installers) are accepted and ignored: every
        // Chromium browser is registered together now.
    }
    if (list) return do_list();
    if (reg) return do_register(ids, firefox);
    if (unreg) return do_unregister(ids, firefox);
#ifdef _WIN32
    // Binary stdio so the 4-byte framing isn't mangled by CRLF translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return serve();
}

} // namespace mpdm
