#pragma once
// Structured capability report for the embedded renderer.
//
// MiniWeb deliberately implements a bounded subset of CSS, DOM and JavaScript.
// This collector records features that were *encountered while rendering* and
// that are ignored or approximated, so an Agent can distinguish a page defect
// from a known renderer limitation. Absence of an entry is not proof of support.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace diag {

enum class Kind { IgnoredCss = 0, Approximate = 1, Structural = 2, MissingJs = 3 };

struct Entry {
    std::string feature;
    std::string detail;
    int count = 0;
};

namespace detail {
inline std::string trim(const std::string& value) {
    size_t begin = 0, end = value.size();
    while (begin < end && std::isspace((unsigned char)value[begin])) ++begin;
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}
inline std::string shorten(const std::string& value, size_t limit = 96) {
    std::string text = trim(value);
    if (text.size() <= limit) return text;
    return text.substr(0, limit - 3) + "...";
}
inline std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}
} // namespace detail

class Report {
public:
    void reset() {
        entries_.clear();
        truncated_ = 0;
    }
    void add(Kind kind, const std::string& feature, const std::string& detail = "") {
        const std::string name = detail::shorten(feature);
        if (name.empty()) return;
        const auto key = std::make_pair((int)kind, name);
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            ++found->second.count;
            if (found->second.detail.empty()) found->second.detail = detail::shorten(detail);
            return;
        }
        if (entries_.size() >= kMaxEntries) {
            ++truncated_;
            return;
        }
        Entry entry;
        entry.feature = name;
        entry.detail = detail::shorten(detail);
        entry.count = 1;
        entries_.emplace(key, std::move(entry));
    }
    bool empty() const { return entries_.empty(); }
    std::vector<Entry> entries(Kind kind) const {
        std::vector<Entry> out;
        for (const auto& item : entries_)
            if (item.first.first == (int)kind) out.push_back(item.second);
        std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            return a.count > b.count || (a.count == b.count && a.feature < b.feature);
        });
        return out;
    }
    // One-line, model-readable summary capped so a noisy page cannot flood a
    // tool result. Full detail is available through json().
    std::string summary() const {
        if (entries_.empty())
            return "Capability report: no known unsupported features were detected in this document; "
                   "absence is not proof of full support.";
        static const Kind order[] = {Kind::Structural, Kind::MissingJs, Kind::Approximate, Kind::IgnoredCss};
        std::vector<std::string> parts;
        size_t shown = 0, total = entries_.size();
        for (Kind kind : order) {
            size_t in_kind = 0;
            for (const auto& entry : entries(kind)) {
                if (shown >= kSummaryItems || in_kind >= kSummaryPerKind) break;
                std::string label;
                switch (kind) {
                case Kind::IgnoredCss: label = "CSS " + entry.feature; break;
                case Kind::Approximate: label = entry.feature; break;
                case Kind::Structural: label = entry.feature; break;
                case Kind::MissingJs: label = "JS " + entry.feature; break;
                }
                if (entry.count > 1) label += " x" + std::to_string(entry.count);
                parts.push_back(std::move(label));
                ++shown;
                ++in_kind;
            }
        }
        std::string out = "Capability report: ";
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) out += ", ";
            out += parts[i];
        }
        if (total > shown) out += ", +" + std::to_string(total - shown) + " more (see --report)";
        out += ". Treat layout and rendering near these features as unreliable; this is not Chromium verification.";
        return out;
    }
    std::string json() const {
        static const char* keys[] = {"ignored_css", "approximate", "structural", "missing_js"};
        std::ostringstream out;
        out << '{';
        for (int kind = 0; kind < 4; ++kind) {
            if (kind) out << ',';
            out << '"' << keys[kind] << "\":[";
            const auto list = entries((Kind)kind);
            for (size_t i = 0; i < list.size(); ++i) {
                if (i) out << ',';
                out << "{\"feature\":\"" << detail::json_escape(list[i].feature) << "\",\"detail\":\""
                    << detail::json_escape(list[i].detail) << "\",\"count\":" << list[i].count << '}';
            }
            out << ']';
        }
        out << ",\"truncated\":" << truncated_ << '}';
        return out.str();
    }

private:
    static constexpr size_t kMaxEntries = 128;
    static constexpr size_t kSummaryItems = 12;
    static constexpr size_t kSummaryPerKind = 3;
    std::map<std::pair<int, std::string>, Entry> entries_;
    size_t truncated_ = 0;
};

inline Report& report() {
    static Report instance;
    return instance;
}
inline void ignored_css(const std::string& property, const std::string& value = "") {
    report().add(Kind::IgnoredCss, property, value);
}
inline void approximate(const std::string& feature, const std::string& detail = "") {
    report().add(Kind::Approximate, feature, detail);
}
inline void structural(const std::string& feature, const std::string& detail = "") {
    report().add(Kind::Structural, feature, detail);
}
inline void missing_js(const std::string& api, const std::string& detail = "") {
    report().add(Kind::MissingJs, api, detail);
}
inline bool has(Kind kind, const std::string& feature_part) {
    for (const auto& entry : report().entries(kind))
        if (entry.feature.find(feature_part) != std::string::npos) return true;
    return false;
}

} // namespace diag
