#include "mpdm/muxer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace mpdm {

static std::string ffmpeg_bin() {
    if (const char* e = std::getenv("FFMPEG")) return e;
    return "ffmpeg";
}

static void append_file(std::ofstream& out, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("missing segment: " + path);
    char buf[1 << 16];
    while (in) {
        in.read(buf, sizeof(buf));
        out.write(buf, in.gcount());
    }
}

std::string assemble_segments(const std::vector<std::string>& seg_paths,
                              const std::string& out_path,
                              const std::optional<std::string>& init_path, bool cleanup) {
    if (seg_paths.empty()) throw std::runtime_error("no segments to assemble");

    fs::path cat = fs::path(out_path).parent_path() / ".__mpdm_concat.bin";
    {
        std::ofstream out(cat, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write temp concat file");
        if (init_path) append_file(out, *init_path);
        for (const auto& p : seg_paths) append_file(out, p);
    }

    // Quote paths for the shell; -c copy = remux without re-encoding.
    std::string cmd = "\"" + ffmpeg_bin() + "\" -y -loglevel error -i \"" + cat.string() +
                      "\" -c copy \"" + out_path + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe needs the whole line wrapped when it has quotes
#endif
    int rc = std::system(cmd.c_str());

    std::error_code ec;
    fs::remove(cat, ec);
    if (cleanup) {
        for (const auto& p : seg_paths) fs::remove(p, ec);
        if (init_path) fs::remove(*init_path, ec);
    }
    if (rc != 0)
        throw std::runtime_error("ffmpeg failed (code " + std::to_string(rc) +
                                 "); is ffmpeg on PATH?");
    return out_path;
}

} // namespace mpdm
