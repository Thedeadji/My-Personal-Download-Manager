#include "mpdm/categories.hpp"

#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace mpdm {

// Extension -> category (lowercase, no dot). Mirrors CATEGORY_EXT in categories.py.
static const std::unordered_map<std::string, std::string>& ext_category() {
    static const std::unordered_map<std::string, std::string> m = [] {
        std::unordered_map<std::string, std::string> x;
        auto add = [&](const char* cat, std::initializer_list<const char*> exts) {
            for (auto e : exts) x[e] = cat;
        };
        add("Video", {"mp4", "mkv", "webm", "avi", "mov", "flv", "m4v", "ts", "wmv", "mpg", "mpeg"});
        add("Audio", {"mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma"});
        add("Documents", {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "epub", "csv"});
        add("Archives", {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "iso", "cab"});
        add("Programs", {"exe", "msi", "msix", "dmg", "pkg", "deb", "rpm", "apk", "appimage"});
        return x;
    }();
    return m;
}

// Category -> on-disk subfolder (IDM-style). "" means "leave in base dir". A name
// that isn't a built-in category is a custom one and uses its own name.
static std::string folder_for(const std::string& category) {
    if (category == "Video") return "Video";
    if (category == "Audio") return "Music";
    if (category == "Documents") return "Documents";
    if (category == "Archives") return "Compressed";
    if (category == "Programs") return "Programs";
    if (category == "Other" || category.empty()) return "";
    return category;  // custom category -> a folder of its own name
}

std::string category_folder(const std::string& category) { return folder_for(category); }

static std::string ext_of(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return "";
    std::string ext = filename.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string classify(const std::string& filename, const std::map<std::string, std::string>& extra) {
    std::string ext = ext_of(filename);
    if (ext.empty()) return "Other";
    // User-defined extensions win over the built-in table.
    for (const auto& kv : extra) {
        std::string tok;
        for (char c : kv.second + " ") {
            if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n' || c == '.') {
                if (!tok.empty()) { if (tok == ext) return kv.first; tok.clear(); }
            } else {
                tok += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
    }
    auto it = ext_category().find(ext);
    return it == ext_category().end() ? "Other" : it->second;
}

std::string category_subdir(const std::string& filename, const std::map<std::string, std::string>& extra) {
    return folder_for(classify(filename, extra));
}

std::string categorized_dir(const std::string& base_dir, const std::string& filename, bool enabled,
                            const std::map<std::string, std::string>& overrides,
                            const std::map<std::string, std::string>& extra) {
    namespace fs = std::filesystem;
    if (!enabled) return base_dir;
    std::string cat = classify(filename, extra);
    auto ov = overrides.find(cat);
    if (ov != overrides.end() && !ov->second.empty()) {
        fs::path p(ov->second);
        return p.is_absolute() ? p.string() : (fs::path(base_dir) / p).string();
    }
    std::string sub = folder_for(cat);
    if (sub.empty()) return base_dir;
    return (fs::path(base_dir) / sub).string();
}

} // namespace mpdm
