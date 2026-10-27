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

// ---- job handling (headless) ----------------------------------------------

static void run_job(json job) {
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

    send({{"type", "accepted"},
          {"id", job_id},
          {"filename", dest.empty() ? "" : fs::path(dest).filename().string()}});

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
    // Binary stdio so the 4-byte framing isn't mangled by CRLF translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return serve();
}

} // namespace mpdm
