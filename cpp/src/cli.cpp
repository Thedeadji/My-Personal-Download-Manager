#include "mpdm/cli.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mpdm/batch.hpp"
#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/ratelimit.hpp"
#include "mpdm/storage.hpp"
#include "mpdm/web.hpp"

namespace fs = std::filesystem;

namespace mpdm {

static const char* APP = "MyPersonalDownloadManager (MPDM) 1.0";

// ---- formatting helpers (port of cli.py) ----------------------------------

static std::string human(double n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    for (int i = 0; i < 5; ++i) {
        if (n < 1024.0 || i == 4) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f%s", n, units[i]);
            return buf;
        }
        n /= 1024.0;
    }
    return "0B";
}

static std::string human_time(double sec) {
    long s = static_cast<long>(sec);
    long h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    char buf[32];
    if (h) std::snprintf(buf, sizeof(buf), "%ldh%02ldm", h, m);
    else if (m) std::snprintf(buf, sizeof(buf), "%ldm%02lds", m, ss);
    else std::snprintf(buf, sizeof(buf), "%lds", ss);
    return buf;
}

static std::string make_bar(const Progress& p, int width = 28) {
    auto pct = p.percent();
    if (!pct) return "[" + std::string(width, '?') + "]";
    int filled = static_cast<int>(width * *pct / 100.0);
    if (filled > width) filled = width;
    return "[" + std::string(filled, '#') + std::string(width - filled, '-') + "]";
}

static void print_progress(const Progress& p) {
    if (p.status == Status::Probing || p.status == Status::Pending) {
        std::cout << "\rprobing... " << std::flush;
        return;
    }
    auto pct = p.percent();
    char pctbuf[16];
    if (pct) std::snprintf(pctbuf, sizeof(pctbuf), "%5.1f%%", *pct);
    else std::snprintf(pctbuf, sizeof(pctbuf), "  ?  ");
    std::string total = p.total ? human(static_cast<double>(*p.total)) : "?";
    std::string eta = p.eta_seconds ? human_time(*p.eta_seconds) : "?";
    std::cout << "\r" << make_bar(p) << " " << pctbuf << "  " << human(static_cast<double>(p.downloaded))
              << "/" << total << "  " << human(p.speed_bps) << "/s  " << p.active_segments
              << " conn  ETA " << eta << "   " << std::flush;
    if (p.status == Status::Completed || p.status == Status::Failed) std::cout << "\n";
}

// ---- tiny arg parser -------------------------------------------------------

namespace {
struct Args {
    std::vector<std::string> positional;
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
    bool has(const std::string& k) const { return flags.count(k) || opts.count(k); }
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;
};
} // namespace

// value_keys: options that take a value; the rest are boolean flags.
static Args parse_args(const std::vector<std::string>& in,
                       const std::map<std::string, std::string>& aliases,
                       const std::set<std::string>& value_keys) {
    Args a;
    for (size_t i = 0; i < in.size(); ++i) {
        std::string tok = in[i];
        if (tok.size() >= 1 && tok[0] == '-') {
            auto al = aliases.find(tok);
            std::string key = al != aliases.end() ? al->second : tok;
            // strip leading dashes for canonical name
            std::string canon = key;
            while (!canon.empty() && canon[0] == '-') canon.erase(canon.begin());
            if (value_keys.count(canon)) {
                if (i + 1 < in.size()) a.opts[canon] = in[++i];
            } else {
                a.flags.insert(canon);
            }
        } else {
            a.positional.push_back(tok);
        }
    }
    return a;
}

// ---- subcommands -----------------------------------------------------------

static int cmd_get(const std::vector<std::string>& rest) {
    std::map<std::string, std::string> aliases = {
        {"-o", "output"}, {"--output", "output"}, {"-d", "output-dir"},
        {"--output-dir", "output-dir"}, {"-n", "connections"}, {"--connections", "connections"},
        {"-l", "limit"}, {"--limit", "limit"}, {"--ua", "ua"}, {"--proxy", "proxy"},
        {"--incognito", "incognito"}, {"-q", "quiet"}, {"--quiet", "quiet"}};
    std::set<std::string> value_keys = {"output", "output-dir", "connections", "limit", "ua", "proxy"};
    Args a = parse_args(rest, aliases, value_keys);
    if (a.positional.empty()) {
        std::cerr << "Error: get needs a URL.\n";
        return 2;
    }

    Settings s = settings_from_env();
    if (a.has("connections")) {
        try { s.connections = std::max(1, std::stoi(a.get("connections"))); } catch (...) {}
    }
    if (a.has("output-dir")) s.output_dir = a.get("output-dir");
    if (a.has("ua")) s.user_agent = resolve_user_agent(a.get("ua"));
    if (a.has("proxy")) s.proxy = a.get("proxy");
    if (a.flags.count("incognito")) {
        s.output_dir = ensure_hidden_dir((fs::path(s.output_dir) / INCOGNITO_DIRNAME).string());
        s.categorize = false;
    }
    if (a.has("limit")) {
        try {
            s.max_rate_bps = parse_rate(a.get("limit"));
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 2;
        }
    }

    std::vector<std::string> urls = expand_urls(a.positional[0]);
    bool quiet = a.flags.count("quiet") > 0;
    if (urls.size() > 1 && a.has("output")) {
        std::cerr << "Error: -o/--output can't be used with a multi-URL pattern.\n";
        return 2;
    }

    int rc = 0;
    for (size_t i = 0; i < urls.size(); ++i) {
        if (urls.size() > 1 && !quiet)
            std::cout << "[" << (i + 1) << "/" << urls.size() << "] " << urls[i] << "\n";
        std::string dest = (urls.size() == 1) ? a.get("output") : "";
        ProgressCb cb = quiet ? ProgressCb{} : ProgressCb{print_progress};
        try {
            Download dl(urls[i], s, dest, cb);
            auto t0 = std::chrono::steady_clock::now();
            std::string path = dl.start();
            double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "Saved to " << path << "  (" << human_time(secs) << ")\n";
        } catch (const std::exception& e) {
            std::cerr << "\nError: " << e.what() << "\n";
            rc = 1;
        }
    }
    return rc;
}

static int cmd_grab(const std::vector<std::string>& rest) {
    std::map<std::string, std::string> aliases = {
        {"-o", "output"},     {"--output", "output"}, {"-t", "type"},
        {"--type", "type"},   {"--quality", "quality"}, {"-n", "connections"},
        {"--connections", "connections"}, {"--ua", "ua"}, {"--proxy", "proxy"},
        {"-q", "quiet"},      {"--quiet", "quiet"}};
    std::set<std::string> value_keys = {"output", "type", "quality", "connections", "ua", "proxy"};
    Args a = parse_args(rest, aliases, value_keys);
    if (a.positional.empty()) {
        std::cerr << "Error: grab needs a URL.\n";
        return 2;
    }
    if (!a.has("output")) {
        std::cerr << "Error: grab requires -o/--output <out.mp4>.\n";
        return 2;
    }

    Settings s = settings_from_env();
    if (a.has("connections")) {
        try { s.connections = std::max(1, std::stoi(a.get("connections"))); } catch (...) {}
    }
    if (a.has("ua")) s.user_agent = resolve_user_agent(a.get("ua"));
    if (a.has("proxy")) s.proxy = a.get("proxy");

    bool quiet = a.flags.count("quiet") > 0;
    std::string type = a.get("type", "auto");
    std::string quality = a.get("quality", "best");

    GrabProgressCb cb;
    if (!quiet) {
        cb = [](int done, int total) {
            double pct = total ? done * 100.0 / total : 0.0;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\rFetching segments %d/%d (%4.1f%%)   ", done, total, pct);
            std::cout << buf << std::flush;
        };
    }
    try {
        std::string out = grab(a.positional[0], a.get("output"), s, type, quality, cb);
        if (!quiet) std::cout << "\n";
        std::cout << "Saved stream to " << out << "\n";
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

static int cmd_clean(const std::vector<std::string>& rest) {
    std::map<std::string, std::string> aliases = {
        {"-d", "output-dir"}, {"--output-dir", "output-dir"}, {"--purge", "purge"}};
    std::set<std::string> value_keys = {"output-dir"};
    Args a = parse_args(rest, aliases, value_keys);

    Settings s = settings_from_env();
    std::string target = a.has("output-dir") ? a.get("output-dir") : s.output_dir;
    bool purge = a.flags.count("purge") > 0;
    CleanResult res = clean_dir(target, purge);

    std::cout << "Cleaned " << target << ":\n";
    std::cout << "  resume sidecars removed: " << res.sidecars.size() << "\n";
    std::cout << "  temp checkpoints removed: " << res.tmp.size() << "\n";
    if (purge) {
        std::cout << "  incomplete files removed: " << res.partials.size() << "\n";
        for (const auto& p : res.partials) std::cout << "    - " << p << "\n";
    } else {
        std::cout << "  (incomplete target files kept; re-run with --purge to delete them)\n";
    }
    if (!res.kept_locked.empty()) {
        std::cout << "  could not remove (locked/in use): " << res.kept_locked.size() << "\n";
        for (const auto& p : res.kept_locked) std::cout << "    ! " << p << "\n";
    }
    return 0;
}

static int cmd_web(const std::vector<std::string>& rest) {
    std::map<std::string, std::string> aliases = {
        {"--host", "host"}, {"--port", "port"}, {"--no-open", "no-open"}};
    std::set<std::string> value_keys = {"host", "port"};
    Args a = parse_args(rest, aliases, value_keys);
    std::string host = a.get("host", "127.0.0.1");
    int port = 8765;
    if (a.has("port")) {
        try { port = std::stoi(a.get("port")); } catch (...) {}
    }
    bool open = a.flags.count("no-open") == 0;
    return run_web(host, port, open);
}

static void usage() {
    std::cout << APP << " — a from-scratch download manager.\n\n"
              << "usage:\n"
              << "  mpdm get <url> [-o file] [-d dir] [-n N] [-l rate] [--ua X] [--proxy X] "
                 "[--incognito] [-q]\n"
              << "  mpdm grab <url> -o out.mp4 [-t auto|hls|dash] [--quality best|worst|720] "
                 "[-n N] [--ua X] [--proxy X] [-q]\n"
              << "  mpdm clean [-d dir] [--purge]\n"
              << "  mpdm web [--host H] [--port P] [--no-open]\n"
              << "  mpdm probe <url>\n";
}

int run_cli(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "--version") {
        std::cout << APP << "\n";
        return 0;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "get") return cmd_get(rest);
    if (cmd == "grab") return cmd_grab(rest);
    if (cmd == "clean") return cmd_clean(rest);
    if (cmd == "web") return cmd_web(rest);
    if (cmd == "probe") {
        if (rest.empty()) { std::cerr << "Error: probe needs a URL.\n"; return 2; }
        try {
            RemoteInfo i = probe(rest[0]);
            std::cout << "probe " << rest[0] << "\n"
                      << "  final_url    : " << i.final_url << "\n"
                      << "  size         : " << (i.size ? std::to_string(*i.size) : "unknown") << "\n"
                      << "  accept_ranges: " << (i.accept_ranges ? "yes" : "no") << "\n"
                      << "  filename     : " << i.filename << "\n"
                      << "  content_type : " << (i.content_type ? *i.content_type : "") << "\n";
        } catch (const std::exception& e) {
            std::cerr << "probe error: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }
    std::cerr << "Unknown command: " << cmd << "\n\n";
    usage();
    return 2;
}

} // namespace mpdm
