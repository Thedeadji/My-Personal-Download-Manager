#include "mpdm/native_host.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
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

    Settings settings = settings_from_env();
    if (job.contains("connections") && !job["connections"].is_null()) {
        try {
            settings.connections = std::max(1, job["connections"].get<int>());
        } catch (...) {
        }
    }
    if (job.contains("output_dir") && job["output_dir"].is_string())
        settings.output_dir = job["output_dir"].get<std::string>();

    std::string dest;
    if (job.contains("filename") && job["filename"].is_string() &&
        !job["filename"].get<std::string>().empty()) {
        std::string fname = sanitize_filename(job["filename"].get<std::string>());
        std::string out_dir =
            categorized_dir(settings.output_dir, fname, settings.categorize, settings.category_dirs);
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        dest = unique_dest(out_dir, fname);
    }

    std::map<std::string, std::string> headers;
    if (job.contains("headers") && job["headers"].is_object())
        for (auto it = job["headers"].begin(); it != job["headers"].end(); ++it)
            if (it.value().is_string()) headers[it.key()] = it.value().get<std::string>();

    bool stream_flag = job.value("stream", false);
    double last_emit = 0.0;

    try {
        std::string path;
        if (is_stream_job(url, stream_flag)) {
            std::string out = dest.empty() ? (fs::path(settings.output_dir) / "video.mp4").string() : dest;
            std::string low = out;
            for (auto& c : low) c = static_cast<char>(::tolower(c));
            if (!(low.size() >= 4 &&
                  (low.rfind(".mp4") == low.size() - 4 || low.rfind(".mkv") == low.size() - 4 ||
                   low.rfind(".webm") == low.size() - 5)))
                out += ".mp4";
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
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    else log_line("handoff: CreateProcess failed (" + std::to_string(GetLastError()) + ")");
    log_line("handoff job " + job_id + " -> " + gui.string());
    return true;  // job file written; a running instance will adopt it regardless
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
#ifdef _WIN32
    if (hand_off_to_app(job, job_id)) {
        send({{"type", "done"}, {"id", job_id}});  // handed to the app, which owns it
        return;
    }
    // GUI not found -> download headlessly so the request isn't lost.
    log_line("app unavailable; headless fallback " + job_id);
#endif
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

// ---- native-messaging host registration (Windows) --------------------------

#ifdef _WIN32
static const char* HOST_NAME = "com.mypersonaldownloadmanager.host";

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

static int do_register(const std::string& ext_id, bool chrome, bool edge) {
    if (ext_id.empty()) {
        std::cerr << "error: --register needs --extension-id <ID> (from chrome://extensions)\n";
        return 2;
    }
    fs::path exe = []() {
        wchar_t b[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
        return fs::path(std::wstring(b, n));
    }();
    // Manifest in %LOCALAPPDATA%\MPDM so registering needs no admin even when the
    // exe lives in Program Files; the manifest's "path" points at the exe.
    const char* lad = std::getenv("LOCALAPPDATA");
    fs::path manifestDir = lad ? (fs::path(lad) / "MPDM") : exe.parent_path();
    std::error_code mec; fs::create_directories(manifestDir, mec);
    fs::path manifest = manifestDir / (std::string(HOST_NAME) + ".json");
    json m = {{"name", HOST_NAME},
              {"description", "MyPersonalDownloadManager native host"},
              {"path", exe.string()},
              {"type", "stdio"},
              {"allowed_origins", json::array({std::string("chrome-extension://") + ext_id + "/"})}};
    try {
        std::ofstream f(manifest, std::ios::binary | std::ios::trunc);
        f << m.dump(2);
    } catch (...) {
        std::cerr << "error: could not write manifest " << manifest.string() << "\n";
        return 1;
    }
    std::wstring wmanifest = manifest.wstring();
    std::wstring wname(HOST_NAME, HOST_NAME + std::strlen(HOST_NAME));
    bool ok = true;
    if (chrome)
        ok &= reg_set_default(L"Software\\Google\\Chrome\\NativeMessagingHosts\\" + wname, wmanifest);
    if (edge)
        ok &= reg_set_default(L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\" + wname, wmanifest);
    std::cout << (ok ? "registered native host:\n  " : "registered manifest but a registry write failed:\n  ")
              << manifest.string() << "\n  extension: " << ext_id << "\n"
              << (chrome ? "  Chrome: yes\n" : "") << (edge ? "  Edge: yes\n" : "");
    return ok ? 0 : 1;
}

static int do_unregister(bool chrome, bool edge) {
    std::wstring wname(HOST_NAME, HOST_NAME + std::strlen(HOST_NAME));
    if (chrome) RegDeleteKeyW(HKEY_CURRENT_USER, (L"Software\\Google\\Chrome\\NativeMessagingHosts\\" + wname).c_str());
    if (edge) RegDeleteKeyW(HKEY_CURRENT_USER, (L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\" + wname).c_str());
    std::cout << "unregistered native host (" << HOST_NAME << ")\n";
    return 0;
}
#endif  // _WIN32

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
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--selftest") return selftest();
#ifdef _WIN32
    {
        bool reg = false, unreg = false, chrome = false, edge = false;
        std::string ext_id;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--register") reg = true;
            else if (a == "--unregister") unreg = true;
            else if (a == "--chrome") chrome = true;
            else if (a == "--edge") edge = true;
            else if (a == "--extension-id" && i + 1 < argc) ext_id = argv[++i];
        }
        if (reg || unreg) {
            if (!chrome && !edge) chrome = edge = true;  // default: both
            return reg ? do_register(ext_id, chrome, edge) : do_unregister(chrome, edge);
        }
    }
#endif
#ifdef _WIN32
    // Binary stdio so the 4-byte framing isn't mangled by CRLF translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return serve();
}

} // namespace mpdm
