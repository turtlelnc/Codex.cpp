#include "util.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace util {

std::string read_file(const std::string& path, bool* ok) {
    std::ifstream in(fs::u8path(path), std::ios::binary | std::ios::ate);
    if(in && (in.tellg()<0 || in.tellg()>32*1024*1024)){if(ok)*ok=false;return {};}
    if(in)in.seekg(0);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

bool write_file(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string to_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && is_ws(s[i])) i++;
    return s.substr(i);
}

std::string rtrim(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && is_ws(s[i - 1])) i--;
    return s.substr(0, i);
}

std::string trim(const std::string& s) { return ltrim(rtrim(s)); }

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_ws(s[i])) i++;
        size_t start = i;
        while (i < s.size() && !is_ws(s[i])) i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == c) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static const std::unordered_map<std::string, uint32_t>& named_colors() {
    static const std::unordered_map<std::string, uint32_t> m = {
        {"transparent", 0x00000000}, {"black", 0x000000ff}, {"white", 0xffffffff},
        {"red", 0xff0000ff}, {"green", 0x008000ff}, {"blue", 0x0000ffff},
        {"yellow", 0xffff00ff}, {"cyan", 0x00ffffff}, {"aqua", 0x00ffffff},
        {"magenta", 0xff00ffff}, {"fuchsia", 0xff00ffff}, {"gray", 0x808080ff},
        {"grey", 0x808080ff}, {"silver", 0xc0c0c0ff}, {"maroon", 0x800000ff},
        {"olive", 0x808000ff}, {"lime", 0x00ff00ff}, {"teal", 0x008080ff},
        {"navy", 0x000080ff}, {"purple", 0x800080ff}, {"orange", 0xffa500ff},
        {"pink", 0xffc0cbff}, {"brown", 0xa52a2aff}, {"gold", 0xffd700ff},
        {"lightgray", 0xd3d3d3ff}, {"lightgrey", 0xd3d3d3ff}, {"darkgray", 0xa9a9a9ff},
        {"darkgrey", 0xa9a9a9ff}, {"dimgray", 0x696969ff}, {"dimgrey", 0x696969ff},
        {"whitesmoke", 0xf5f5f5ff}, {"gainsboro", 0xdcdcdcff}, {"beige", 0xf5f5dcff},
        {"ivory", 0xfffff0ff}, {"khaki", 0xf0e68cff}, {"coral", 0xff7f50ff},
        {"salmon", 0xfa8072ff}, {"tomato", 0xff6347ff}, {"crimson", 0xdc143cff},
        {"darkred", 0x8b0000ff}, {"darkgreen", 0x006400ff}, {"darkblue", 0x00008bff},
        {"lightblue", 0xadd8e6ff}, {"lightgreen", 0x90ee90ff}, {"skyblue", 0x87ceebff},
        {"steelblue", 0x4682b4ff}, {"royalblue", 0x4169e1ff}, {"slategray", 0x708090ff},
        {"slategrey", 0x708090ff}, {"aliceblue", 0xf0f8ffff}, {"ghostwhite", 0xf8f8ffff},
        {"mintcream", 0xf5fffaff}, {"seashell", 0xfff5eeff}, {"linen", 0xfaf0e6ff},
        {"oldlace", 0xfdf5e6ff}, {"floralwhite", 0xfffaf0ff}, {"honeydew", 0xf0fff0ff},
        {"lavender", 0xe6e6faff}, {"thistle", 0xd8bfd8ff}, {"plum", 0xdda0ddff},
        {"orchid", 0xda70d6ff}, {"violet", 0xee82eeff}, {"indigo", 0x4b0082ff},
        {"goldenrod", 0xdaa520ff}, {"peru", 0xcd853fff}, {"chocolate", 0xd2691eff},
        {"sienna", 0xa0522dff}, {"tan", 0xd2b48cff}, {"wheat", 0xf5deb3ff},
        {"azure", 0xf0ffffff}, {"chartreuse", 0x7fff00ff}, {"limegreen", 0x32cd32ff},
        {"forestgreen", 0x228b22ff}, {"seagreen", 0x2e8b57ff}, {"springgreen", 0x00ff7fff},
        {"turquoise", 0x40e0d0ff}, {"aquamarine", 0x7fffd4ff}, {"powderblue", 0xb0e0e6ff},
        {"cadetblue", 0x5f9ea0ff}, {"cornflowerblue", 0x6495edff}, {"dodgerblue", 0x1e90ffff},
        {"deepskyblue", 0x00bfffff}, {"midnightblue", 0x191970ff}, {"rebeccapurple", 0x663399ff},
        {"mistyrose", 0xffe4e1ff}, {"peachpuff", 0xffdab9ff}, {"papayawhip", 0xffefd5ff},
        {"moccasin", 0xffe4b5ff}, {"navajowhite", 0xffdeadff}, {"antiquewhite", 0xfaebd7ff},
        {"burlywood", 0xdeb887ff}, {"rosybrown", 0xbc8f8fff}, {"indianred", 0xcd5c5cff},
        {"firebrick", 0xb22222ff}, {"darkorange", 0xff8c00ff}, {"orangered", 0xff4500ff},
        {"hotpink", 0xff69b4ff}, {"deeppink", 0xff1493ff}, {"mediumvioletred", 0xc71585ff},
        {"palevioletred", 0xdb7093ff}, {"mediumpurple", 0x9370dbff}, {"blueviolet", 0x8a2be2ff},
        {"darkviolet", 0x9400d3ff}, {"darkorchid", 0x9932ccff}, {"mediumorchid", 0xba55d3ff},
        {"mediumseagreen", 0x3cb371ff}, {"mediumspringgreen", 0x00fa9aff},
        {"mediumturquoise", 0x48d1ccff}, {"darkturquoise", 0x00ced1ff},
        {"lightcyan", 0xe0ffffff}, {"paleturquoise", 0xafeeeeff},
        {"palegreen", 0x98fb98ff}, {"lightsteelblue", 0xb0c4deff},
        {"lightseagreen", 0x20b2aaff}, {"darkseagreen", 0x8fbc8fff},
        {"darkolivegreen", 0x556b2fff}, {"olivedrab", 0x6b8e23ff},
        {"darkslategray", 0x2f4f4fff}, {"darkslategrey", 0x2f4f4fff},
    };
    return m;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Color parse_color(const std::string& input, bool* ok) {
    std::string s = to_lower(trim(input));
    if (ok) *ok = false;
    if (s.empty()) return {};
    auto it = named_colors().find(s);
    if (it != named_colors().end()) {
        uint32_t v = it->second;
        if (ok) *ok = true;
        return Color{(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    }
    if (s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3 || h.size() == 4) {
            std::string e;
            for (char c : h) { e.push_back(c); e.push_back(c); }
            h = e;
        }
        if (h.size() != 6 && h.size() != 8) return {};
        int v[8];
        for (int i = 0; i < (int)h.size(); i++) {
            v[i] = hexval(h[i]);
            if (v[i] < 0) return {};
        }
        Color c;
        c.r = (uint8_t)(v[0] * 16 + v[1]);
        c.g = (uint8_t)(v[2] * 16 + v[3]);
        c.b = (uint8_t)(v[4] * 16 + v[5]);
        c.a = h.size() == 8 ? (uint8_t)(v[6] * 16 + v[7]) : 255;
        if (ok) *ok = true;
        return c;
    }
    if (starts_with(s, "rgb(") || starts_with(s, "rgba(")) {
        size_t lp = s.find('(');
        size_t rp = s.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp) return {};
        std::string body = s.substr(lp + 1, rp - lp - 1);
        if (body.find(',') == std::string::npos) {
            std::string alpha;
            size_t slash = body.find('/');
            if (slash != std::string::npos) {
                alpha = trim(body.substr(slash + 1));
                body = body.substr(0, slash);
            }
            std::string modern;
            for (char c : trim(body)) modern.push_back(is_ws(c) ? ',' : c);
            if (!alpha.empty()) {
                modern += ',';
                modern += alpha;
            }
            body = modern;
        }
        std::vector<std::string> parts;
        for (auto& p : split(body, ',')) parts.push_back(trim(p));
        if (parts.size() != 3 && parts.size() != 4) return {};
        int vals[4] = {0, 0, 0, 255};
        for (int i = 0; i < (int)parts.size(); i++) {
            std::string p = parts[i];
            double d = 0;
            if (ends_with(p, "%")) d = atof(p.substr(0, p.size() - 1).c_str()) * 255.0 / 100.0;
            else d = atof(p.c_str());
            if (i < 3) vals[i] = (int)std::lround(std::clamp(d, 0.0, 255.0));
            else {
                if (ends_with(p, "%")) d = atof(p.substr(0, p.size() - 1).c_str()) / 100.0;
                vals[3] = (int)std::lround(std::clamp(d, 0.0, 1.0) * 255.0);
            }
        }
        if (ok) *ok = true;
        return Color{(uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2], (uint8_t)vals[3]};
    }
    if (starts_with(s, "hsl(") || starts_with(s, "hsla(")) {
        size_t lp = s.find('(');
        size_t rp = s.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos) return {};
        std::string hbody = s.substr(lp + 1, rp - lp - 1);
        if (hbody.find(',') == std::string::npos) {
            std::string alpha;
            size_t slash = hbody.find('/');
            if (slash != std::string::npos) {
                alpha = trim(hbody.substr(slash + 1));
                hbody = hbody.substr(0, slash);
            }
            std::string modern;
            for (char c : trim(hbody)) modern.push_back(is_ws(c) ? ',' : c);
            if (!alpha.empty()) {
                modern += ',';
                modern += alpha;
            }
            hbody = modern;
        }
        auto parts = split(hbody, ',');
        if (parts.size() < 3) return {};
        double h = atof(trim(parts[0]).c_str()) / 360.0;
        double sat = atof(trim(parts[1]).c_str()) / 100.0;
        double light = atof(trim(parts[2]).c_str()) / 100.0;
        double alpha = parts.size() > 3 ? atof(trim(parts[3]).c_str()) : 1.0;
        auto hue = [](double p, double q, double t) {
            if (t < 0) t += 1;
            if (t > 1) t -= 1;
            if (t < 1.0 / 6) return p + (q - p) * 6 * t;
            if (t < 0.5) return q;
            if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
            return p;
        };
        double r, g, b;
        if (sat == 0) r = g = b = light;
        else {
            double q = light < 0.5 ? light * (1 + sat) : light + sat - light * sat;
            double p = 2 * light - q;
            r = hue(p, q, h + 1.0 / 3);
            g = hue(p, q, h);
            b = hue(p, q, h - 1.0 / 3);
        }
        if (ok) *ok = true;
        return Color{(uint8_t)std::lround(std::clamp(r, 0.0, 1.0) * 255),
                     (uint8_t)std::lround(std::clamp(g, 0.0, 1.0) * 255),
                     (uint8_t)std::lround(std::clamp(b, 0.0, 1.0) * 255),
                     (uint8_t)std::lround(std::clamp(alpha, 0.0, 1.0) * 255)};
    }
    if (starts_with(s, "color-mix(")) {
        size_t lp = s.find('('), rp = s.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos) return {};
        std::string body = s.substr(lp + 1, rp - lp - 1);
        auto split_commas = [](const std::string& in) {
            std::vector<std::string> out;
            std::string cur;
            int depth = 0;
            for (char c : in) {
                if (c == '(') depth++;
                if (c == ')') depth--;
                if (c == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); }
                else cur.push_back(c);
            }
            out.push_back(trim(cur));
            return out;
        };
        auto parts = split_commas(body);
        if (parts.size() < 3) return {};
        // parts[0] is "in <color-space>"; mixing is approximated in sRGB.
        struct MixCol { Color c; float pct; };
        std::vector<MixCol> cols;
        for (size_t i = 1; i < parts.size(); i++) {
            std::string spec = trim(parts[i]);
            float pct = -1;
            size_t sp = spec.rfind(' ');
            if (sp != std::string::npos) {
                std::string tail = trim(spec.substr(sp + 1));
                if (ends_with(tail, "%")) {
                    pct = (float)atof(tail.c_str()) / 100.0f;
                    spec = trim(spec.substr(0, sp));
                }
            }
            bool cok = false;
            Color c = parse_color(spec, &cok);
            if (!cok) return {};
            cols.push_back({c, pct});
        }
        if (cols.size() < 2) return {};
        float p0 = cols[0].pct, p1 = cols[1].pct;
        if (p0 < 0 && p1 < 0) { p0 = p1 = 0.5f; }
        else if (p0 < 0) p0 = 1.0f - p1;
        else if (p1 < 0) p1 = 1.0f - p0;
        float sum = p0 + p1;
        if (sum <= 0) return {};
        p0 /= sum;
        p1 /= sum;
        auto pm = [](const Color& c) {
            float a = c.a / 255.0f;
            return std::array<float, 4>{c.r * a, c.g * a, c.b * a, a};
        };
        auto a0 = pm(cols[0].c), a1 = pm(cols[1].c);
        float ar = a0[3] * p0 + a1[3] * p1;
        if (ar <= 0.0001f) { if (ok) *ok = true; return Color{0, 0, 0, 0}; }
        float rr = (a0[0] * p0 + a1[0] * p1) / ar;
        float gg = (a0[1] * p0 + a1[1] * p1) / ar;
        float bb = (a0[2] * p0 + a1[2] * p1) / ar;
        if (ok) *ok = true;
        return Color{(uint8_t)std::lround(std::clamp(rr, 0.0f, 255.0f)),
                     (uint8_t)std::lround(std::clamp(gg, 0.0f, 255.0f)),
                     (uint8_t)std::lround(std::clamp(bb, 0.0f, 255.0f)),
                     (uint8_t)std::lround(std::clamp(ar, 0.0f, 1.0f) * 255)};
    }
    bool is_lch = starts_with(s, "oklch(");
    if (is_lch || starts_with(s, "oklab(")) {
        size_t lp = s.find('(');
        size_t rp = s.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos) return {};
        std::string body = s.substr(lp + 1, rp - lp - 1);
        double alpha = 1.0;
        size_t slash = body.find('/');
        if (slash != std::string::npos) {
            std::string as = trim(body.substr(slash + 1));
            if (ends_with(as, "%")) alpha = atof(as.c_str()) / 100.0;
            else alpha = atof(as.c_str());
            body = body.substr(0, slash);
        }
        body = replace_all(body, ",", " ");
        auto parts = split_ws(body);
        if (parts.size() < 3) return {};
        auto num = [](const std::string& tok, double pct_scale, double plain_scale) -> double {
            std::string t = trim(tok);
            if (ends_with(t, "%")) return atof(t.c_str()) / 100.0 * pct_scale;
            return atof(t.c_str()) * plain_scale;
        };
        double L = num(parts[0], 1.0, 1.0);
        double a = 0, bb = 0;
        if (is_lch) {
            double C = num(parts[1], 0.4, 1.0);
            std::string hs = trim(parts[2]);
            double H = atof(hs.c_str());
            if (ends_with(hs, "rad")) H = atof(hs.c_str()) * 180.0 / 3.14159265358979323846;
            else if (ends_with(hs, "turn")) H = atof(hs.c_str()) * 360.0;
            a = C * std::cos(H * 3.14159265358979323846 / 180.0);
            bb = C * std::sin(H * 3.14159265358979323846 / 180.0);
        } else {
            a = num(parts[1], 0.4, 1.0);
            bb = num(parts[2], 0.4, 1.0);
        }
        double l_ = L + 0.3963377774 * a + 0.2158037573 * bb;
        double m_ = L - 0.1055613458 * a - 0.0638541728 * bb;
        double s_ = L - 0.0894841775 * a - 1.2914855480 * bb;
        double l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
        double rl = 4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3;
        double gl = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3;
        double bl = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3;
        auto gamma = [](double x) {
            x = std::clamp(x, 0.0, 1.0);
            return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
        };
        if (ok) *ok = true;
        return Color{(uint8_t)std::lround(gamma(rl) * 255), (uint8_t)std::lround(gamma(gl) * 255),
                     (uint8_t)std::lround(gamma(bl) * 255),
                     (uint8_t)std::lround(std::clamp(alpha, 0.0, 1.0) * 255)};
    }
    return {};
}

const char* color_name(uint32_t rgb) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "#%06x", rgb & 0xffffff);
    return buf;
}

std::string html_unescape(const std::string& s) {
    static const std::unordered_map<std::string, std::string> ents = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", "\xc2\xa0"}, {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"},
        {"hellip", "\xe2\x80\xa6"}, {"mdash", "\xe2\x80\x94"}, {"ndash", "\xe2\x80\x93"},
        {"ldquo", "\xe2\x80\x9c"}, {"rdquo", "\xe2\x80\x9d"}, {"lsquo", "\xe2\x80\x98"},
        {"rsquo", "\xe2\x80\x99"}, {"times", "\xc3\x97"}, {"divide", "\xc3\xb7"},
        {"middot", "\xc2\xb7"}, {"bull", "\xe2\x80\xa2"}, {"deg", "\xc2\xb0"},
        {"plusmn", "\xc2\xb1"}, {"euro", "\xe2\x82\xac"}, {"pound", "\xc2\xa3"},
        {"yen", "\xc2\xa5"}, {"cent", "\xc2\xa2"}, {"sect", "\xc2\xa7"},
        {"para", "\xc2\xb6"}, {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"},
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 40) { out.push_back(s[i++]); continue; }
        std::string body = s.substr(i + 1, semi - i - 1);
        if (!body.empty() && body[0] == '#') {
            uint32_t cp = 0;
            bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
            try { cp = hex ? (uint32_t)std::stoul(body.substr(2), nullptr, 16) : (uint32_t)std::stoul(body.substr(1)); }
            catch (...) { cp = 0; }
            if (cp) { out += utf8_encode(cp); i = semi + 1; continue; }
        } else {
            auto it = ents.find(to_lower(body));
            if (it != ents.end()) { out += it->second; i = semi + 1; continue; }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out.push_back((char)c);
        }
    }
    return out;
}

std::string url_decode(const std::string& s, bool plus_is_space) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int a = hexval(s[i + 1]), b = hexval(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back((char)(a * 16 + b));
                i += 2;
                continue;
            }
        }
        if (plus_is_space && s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

std::string url_encode_path(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c >= 0x80)
            out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string base64_decode(const std::string& input) {
    std::string s;
    size_t comma = input.find(',');
    if (starts_with(to_lower(input), "data:") && comma != std::string::npos) s = input.substr(comma + 1);
    else s = input;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xff));
        }
    }
    return out;
}

std::string dirname(const std::string& path) {
    fs::path p(path);
    auto parent = p.parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

std::string basename(const std::string& path) {
    return fs::u8path(path).filename().string();
}

std::string path_join(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    fs::path pb(b);
    if (pb.is_absolute()) return pb.lexically_normal().string();
    return (fs::path(a) / pb).lexically_normal().string();
}

std::string normalize_path(const std::string& path) {
    std::string s = path;
    std::replace(s.begin(), s.end(), '\\', '/');
    return fs::path(s).lexically_normal().string();
}

std::string absolute_path(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) return normalize_path(path);
    return p.lexically_normal().string();
}

std::string current_dir() {
    std::error_code ec;
    auto p = fs::current_path(ec);
    return ec ? std::string(".") : p.string();
}

std::string home_dir() {
    const char* h = getenv("HOME");
#ifdef _WIN32
    if (!h) h = getenv("USERPROFILE");
#endif
    return h ? std::string(h) : current_dir();
}

std::string url_scheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos) return {};
    if(colon==1&&std::isalpha(static_cast<unsigned char>(url[0]))&&url.size()>2&&(url[2]=='\\'||url[2]=='/'))return {};
    for (size_t i = 0; i < colon; i++) {
        char c = url[i];
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) return {};
        if (i == 0 && !std::isalpha((unsigned char)c)) return {};
    }
    return to_lower(url.substr(0, colon));
}

static std::string strip_query_fragment(const std::string& s) {
    size_t p = s.find_first_of("?#");
    return p == std::string::npos ? s : s.substr(0, p);
}

std::string path_to_file_url(const std::string& path) {
    std::string abs = absolute_path(path);
    std::replace(abs.begin(), abs.end(), '\\', '/');
    if (!abs.empty() && abs[0] != '/') abs = "/" + abs;
    return "file://" + url_encode_path(abs);
}

std::string file_url_to_path(const std::string& url) {
    if (!starts_with(to_lower(url), "file:")) return url;
    std::string rest = url.substr(5);
    if (starts_with(rest, "//")) {
        rest = rest.substr(2);
        size_t slash = rest.find('/');
        std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash);
        if (!host.empty() && to_lower(host) != "localhost") {
            // UNC-ish path; keep host as part of path
            rest = "//" + host + rest;
        }
    }
    rest = strip_query_fragment(rest);
    std::string p = url_decode(rest);
#ifdef _WIN32
    if (p.size() >= 3 && p[0] == '/' && std::isalpha((unsigned char)p[1]) && p[2] == ':')
        p = p.substr(1);
#endif
    return p;
}

bool is_file_url(const std::string& url) {
    return url_scheme(url) == "file";
}

bool is_loopback_url(const std::string& url) {
    std::string scheme = url_scheme(url);
    if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss") return false;
    size_t pos = url.find("://");
    if (pos == std::string::npos) return false;
    std::string authority = url.substr(pos + 3);
    size_t slash = authority.find_first_of("/?#");
    if (slash != std::string::npos) authority = authority.substr(0, slash);
    // strip userinfo
    size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    std::string host;
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = to_lower(authority.substr(1, close - 1));
    } else {
        size_t colon = authority.find(':');
        host = to_lower(colon == std::string::npos ? authority : authority.substr(0, colon));
    }
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "0:0:0:0:0:0:0:1";
}

bool url_allowed(const std::string& url) {
    std::string scheme = url_scheme(url);
    if (scheme.empty() || scheme == "file" || scheme == "data") return true;
    return scheme == "http" && is_loopback_url(url);
}

std::string resolve_url(const std::string& base, const std::string& rel) {
    std::string r = trim(rel);
    if (r.empty()) return base;
    if (r[0] == '#') return strip_query_fragment(base) + r;
    std::string scheme = url_scheme(r);
    if (!scheme.empty()) {
        if (r.size() > 1 && r[0] == '/' && r[1] == '/') {
            std::string bs = url_scheme(base);
            return bs.empty() ? "file:" + r : bs + ":" + r;
        }
        return r;
    }
    std::string bscheme = url_scheme(base);
    if (bscheme == "http" || bscheme == "https") {
        size_t pos = base.find("://");
        if (pos == std::string::npos) return r;
        size_t path_start = base.find('/', pos + 3);
        std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
        if (!r.empty() && r[0] == '/') return origin + r;
        std::string bpath = path_start == std::string::npos ? "/" : base.substr(path_start);
        bpath = strip_query_fragment(bpath);
        size_t last = bpath.rfind('/');
        std::string dir = last == std::string::npos ? "/" : bpath.substr(0, last + 1);
        std::string combined = dir + r;
        // normalize dot segments
        std::vector<std::string> parts;
        for (auto& part : split(combined, '/')) {
            if (part.empty() || part == ".") continue;
            if (part == "..") { if (!parts.empty()) parts.pop_back(); }
            else parts.push_back(part);
        }
        return origin + "/" + join(parts, "/");
    }
    if (bscheme == "file") {
        std::string base_path = file_url_to_path(strip_query_fragment(base));
        std::string p;
        if (!r.empty() && (r[0] == '/' || (r.size() > 1 && r[1] == ':'))) p = url_decode(strip_query_fragment(r));
        else p = path_join(dirname(base_path), url_decode(strip_query_fragment(r)));
        return path_to_file_url(p);
    }
    // base is a filesystem path
    std::string p;
    if (!r.empty() && (r[0] == '/' || (r.size() > 1 && r[1] == ':'))) p = url_decode(strip_query_fragment(r));
    else p = path_join(dirname(base), url_decode(strip_query_fragment(r)));
    return path_to_file_url(p);
}

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string format_float(float v, int decimals) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

size_t utf8_next(const std::string& s, size_t i, uint32_t* cp) {
    if (i >= s.size()) { *cp = 0; return i; }
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { *cp = c; return i + 1; }
    int len = 0;
    uint32_t v = 0;
    if ((c & 0xe0) == 0xc0) { len = 2; v = c & 0x1f; }
    else if ((c & 0xf0) == 0xe0) { len = 3; v = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { len = 4; v = c & 0x07; }
    else { *cp = 0xfffd; return i + 1; }
    if (i + len > s.size()) { *cp = 0xfffd; return s.size(); }
    for (int k = 1; k < len; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xc0) != 0x80) { *cp = 0xfffd; return i + 1; }
        v = (v << 6) | (cc & 0x3f);
    }
    *cp = v;
    return i + len;
}

std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        i = utf8_next(s, i, &cp);
        out.push_back(cp);
    }
    return out;
}

std::string utf8_encode(uint32_t cp) {
    std::string out;
    if (cp <= 0x7f) out.push_back((char)cp);
    else if (cp <= 0x7ff) {
        out.push_back((char)(0xc0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back((char)(0xe0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0x10ffff) {
        out.push_back((char)(0xf0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back((char)(0x80 | (cp & 0x3f)));
    }
    return out;
}

bool is_cjk(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x20000 && cp <= 0x2fa1f) ||
           (cp >= 0xff00 && cp <= 0xffef) || (cp >= 0x3000 && cp <= 0x303f);
}

bool is_space_cp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
}

static void vlog(const char* prefix, const char* fmt, va_list ap) {
    if (getenv("MINIWEB_QUIET")) return;
    fprintf(stderr, "%s", prefix);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog("[miniweb] ", fmt, ap); va_end(ap);
}
void warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog("[miniweb] warning: ", fmt, ap); va_end(ap);
}
void error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vlog("[miniweb] error: ", fmt, ap); va_end(ap);
}

} // namespace util
