#include "mpdm/dash.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <stdexcept>

namespace mpdm {

static std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string local_name(const pugi::xml_node& n) {
    std::string s = n.name();
    auto c = s.find(':');
    return c == std::string::npos ? s : s.substr(c + 1);
}

static void collect(const pugi::xml_node& node, const std::string& name,
                    std::vector<pugi::xml_node>& out) {
    for (auto ch : node.children()) {
        if (local_name(ch) == name) out.push_back(ch);
        collect(ch, name, out);
    }
}
static std::vector<pugi::xml_node> find_all(const pugi::xml_node& root, const std::string& name) {
    std::vector<pugi::xml_node> v;
    collect(root, name, v);
    return v;
}
static pugi::xml_node child_named(const pugi::xml_node& el, const std::string& name) {
    for (auto ch : el.children())
        if (local_name(ch) == name) return ch;
    return pugi::xml_node();
}
static pugi::xml_node adaptation_of(const pugi::xml_node& root, const pugi::xml_node& rep) {
    for (auto ad : find_all(root, "AdaptationSet")) {
        std::vector<pugi::xml_node> reps;
        collect(ad, "Representation", reps);
        for (auto r : reps)
            if (r == rep) return ad;
    }
    return pugi::xml_node();
}

static std::string fmt_num(const std::string& fmt, long long n) {
    if (fmt.empty()) return std::to_string(n);
    std::smatch m;
    if (std::regex_match(fmt, m, std::regex("%0(\\d+)d"))) {
        int w = std::stoi(m[1].str());
        std::string s = std::to_string(n);
        if (static_cast<int>(s.size()) < w) s = std::string(w - s.size(), '0') + s;
        return s;
    }
    return std::to_string(n);
}

static std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    size_t p;
    while ((p = s.find(from)) != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

static std::string fmt_media(const std::string& media, const std::string& rep_id, long long number,
                             int bw) {
    std::string out = replace_all(media, "$RepresentationID$", rep_id);
    out = replace_all(out, "$Bandwidth$", std::to_string(bw));
    // Replace $Number$ or $Number%0Nd$ occurrences.
    std::regex re("\\$Number(%0\\d+d)?\\$");
    std::string result;
    auto begin = std::sregex_iterator(out.begin(), out.end(), re);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        auto m = *it;
        result += out.substr(last, m.position() - last);
        result += fmt_num(m[1].matched ? m[1].str() : "", number);
        last = m.position() + m.length();
    }
    result += out.substr(last);
    return result;
}

static std::string expand_init(const std::string& tmpl, const std::string& rep_id, int bw) {
    std::string out = replace_all(tmpl, "$RepresentationID$", rep_id);
    out = replace_all(out, "$Bandwidth$", std::to_string(bw));
    return out;
}

static double mpd_duration_seconds(const pugi::xml_node& root) {
    std::string dur = root.attribute("mediaPresentationDuration").value();
    std::smatch m;
    if (!std::regex_search(dur, m, std::regex(R"(PT(?:(\d+)H)?(?:(\d+)M)?(?:([\d.]+)S)?)")))
        return 0.0;
    double h = m[1].matched ? std::atof(m[1].str().c_str()) : 0;
    double mi = m[2].matched ? std::atof(m[2].str().c_str()) : 0;
    double s = m[3].matched ? std::atof(m[3].str().c_str()) : 0;
    return h * 3600 + mi * 60 + s;
}

static std::pair<std::optional<std::string>, std::vector<std::string>>
parse_segment_template(const pugi::xml_node& tmpl, const pugi::xml_node& root,
                       const std::string& rep_id, int bw, const std::string& base_url) {
    std::string media = tmpl.attribute("media").value();
    std::string init = tmpl.attribute("initialization").value();
    long long start_number = tmpl.attribute("startNumber") ? tmpl.attribute("startNumber").as_llong() : 1;

    std::optional<std::string> init_url;
    if (!init.empty()) init_url = urljoin(base_url, expand_init(init, rep_id, bw));

    std::vector<std::string> seg_urls;
    pugi::xml_node timeline = child_named(tmpl, "SegmentTimeline");
    if (timeline) {
        long long number = start_number;
        for (auto s : timeline.children()) {
            if (local_name(s) != "S") continue;
            long long repeat = s.attribute("r") ? s.attribute("r").as_llong() : 0;
            for (long long k = 0; k <= repeat; ++k)
                seg_urls.push_back(urljoin(base_url, fmt_media(media, rep_id, number++, bw)));
        }
    } else {
        long long duration = tmpl.attribute("duration").as_llong();
        long long timescale = tmpl.attribute("timescale") ? tmpl.attribute("timescale").as_llong() : 1;
        double total = mpd_duration_seconds(root);
        if (duration && total > 0) {
            long long count = static_cast<long long>((total * timescale) / duration) + 1;
            for (long long i = 0; i < count; ++i)
                seg_urls.push_back(urljoin(base_url, fmt_media(media, rep_id, start_number + i, bw)));
        } else {
            throw std::runtime_error("could not determine segment count for this MPD");
        }
    }
    return {init_url, seg_urls};
}

DashStream parse_manifest(const std::string& text, const std::string& url, const std::string& prefer) {
    pugi::xml_document doc;
    if (!doc.load_string(text.c_str())) throw std::runtime_error("invalid MPD XML");
    pugi::xml_node root = doc.document_element();

    std::string base_url = url;
    auto bases = find_all(root, "BaseURL");
    if (!bases.empty()) base_url = urljoin(url, strip(bases[0].child_value()));

    struct Rep { int bw; std::string id; pugi::xml_node node; };
    std::vector<Rep> reps;
    for (auto rep : find_all(root, "Representation"))
        reps.push_back({rep.attribute("bandwidth").as_int(), rep.attribute("id").value(), rep});
    if (reps.empty()) throw std::runtime_error("no Representations found in MPD");
    std::stable_sort(reps.begin(), reps.end(), [](const Rep& a, const Rep& b) { return a.bw > b.bw; });
    Rep chosen = (prefer == "worst") ? reps.back() : reps.front();

    // Representation media BaseURL (chain Adaptation + Representation BaseURLs).
    std::string rep_base = base_url;
    bool had_baseurl = false;
    if (auto ad = adaptation_of(root, chosen.node)) {
        auto b = child_named(ad, "BaseURL");
        if (b && *strip(b.child_value()).c_str()) { rep_base = urljoin(rep_base, strip(b.child_value())); had_baseurl = true; }
    }
    if (auto b = child_named(chosen.node, "BaseURL")) {
        std::string t = strip(b.child_value());
        if (!t.empty()) { rep_base = urljoin(rep_base, t); had_baseurl = true; }
    }

    auto seg_node = [&](const std::string& name) -> pugi::xml_node {
        if (auto n = child_named(chosen.node, name)) return n;
        if (auto ad = adaptation_of(root, chosen.node)) return child_named(ad, name);
        return pugi::xml_node();
    };
    pugi::xml_node tmpl = seg_node("SegmentTemplate");
    pugi::xml_node seg_list = seg_node("SegmentList");
    pugi::xml_node seg_base = seg_node("SegmentBase");

    DashStream out;
    out.mime = chosen.node.attribute("mimeType").value();
    out.bandwidth = chosen.bw;

    if (tmpl) {
        auto r = parse_segment_template(tmpl, root, chosen.id, chosen.bw, rep_base);
        out.init_url = r.first;
        out.segment_urls = r.second;
    } else if (seg_list) {
        if (auto in = child_named(seg_list, "Initialization")) {
            std::string src = in.attribute("sourceURL").value();
            if (!src.empty()) out.init_url = urljoin(rep_base, src);
        }
        for (auto ch : seg_list.children()) {
            if (local_name(ch) != "SegmentURL") continue;
            std::string media = ch.attribute("media").value();
            if (!media.empty()) out.segment_urls.push_back(urljoin(rep_base, media));
        }
        if (out.segment_urls.empty()) throw std::runtime_error("SegmentList had no <SegmentURL> entries");
    } else if (seg_base || had_baseurl) {
        out.segment_urls.push_back(rep_base);  // single self-contained file
    } else {
        throw std::runtime_error("Unsupported MPD: no SegmentTemplate/List/Base/BaseURL");
    }
    return out;
}

DashStream parse_mpd(const std::string& url, const HttpOptions& opt, const std::string& prefer) {
    return parse_manifest(get_bytes(url, opt), url, prefer);
}

} // namespace mpdm
