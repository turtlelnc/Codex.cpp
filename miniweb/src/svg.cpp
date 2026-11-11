#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "svg.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

std::string prepare_svg(const std::string& input, Color color) {
    std::string svg = input;
    char col[48];
    snprintf(col, sizeof(col), "rgb(%d,%d,%d)", color.r, color.g, color.b);
    svg = util::replace_all(svg, "currentColor", col);
    svg = util::replace_all(svg, "currentcolor", col);
    svg = util::replace_all(svg, "viewbox=", "viewBox=");
    svg = util::replace_all(svg, "preserveaspectratio=", "preserveAspectRatio=");
    svg = util::replace_all(svg, "gradientunits=", "gradientUnits=");
    svg = util::replace_all(svg, "gradienttransform=", "gradientTransform=");
    svg = util::replace_all(svg, "patternunits=", "patternUnits=");
    svg = util::replace_all(svg, "markerunits=", "markerUnits=");
    svg = util::replace_all(svg, "refx=", "refX=");
    svg = util::replace_all(svg, "refy=", "refY=");
    return svg;
}

float attr_number(const std::string& tag, const char* name, bool* found = nullptr) {
    if (found) *found = false;
    std::string needle = std::string(name) + "=";
    size_t p = tag.find(needle);
    if (p == std::string::npos) return 0;
    p += needle.size();
    char q = 0;
    if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) q = tag[p++];
    size_t e = p;
    if (q) {
        while (e < tag.size() && tag[e] != q) e++;
    } else {
        while (e < tag.size() && !std::isspace((unsigned char)tag[e]) && tag[e] != '>' && tag[e] != '/') e++;
    }
    std::string v = tag.substr(p, e - p);
    if (v.empty()) return 0;
    if (found) *found = true;
    return (float)std::atof(v.c_str());
}

} // namespace

void svg_intrinsic_size(const std::string& svg, float* out_w, float* out_h) {
    std::string tag = svg.substr(0, std::min<size_t>(svg.size(), 4096));
    bool fw = false, fh = false;
    float w = attr_number(tag, "width", &fw);
    float h = attr_number(tag, "height", &fh);
    float vbw = 0, vbh = 0;
    size_t vb = svg.find("viewBox=");
    if (vb == std::string::npos) vb = svg.find("viewbox=");
    if (vb != std::string::npos) {
        size_t q = svg.find('"', vb);
        if (q == std::string::npos) q = svg.find('\'', vb);
        if (q != std::string::npos) {
            char quote = svg[q];
            size_t e = svg.find(quote, q + 1);
            if (e != std::string::npos) {
                std::string val = svg.substr(q + 1, e - q - 1);
                float a, b, c, d;
                if (sscanf(val.c_str(), "%f %f %f %f", &a, &b, &c, &d) == 4) {
                    vbw = c;
                    vbh = d;
                } else if (sscanf(val.c_str(), "%f,%f,%f,%f", &a, &b, &c, &d) == 4) {
                    vbw = c;
                    vbh = d;
                }
            }
        }
    }
    if (!fw && !fh) {
        if (vbw > 0 && vbh > 0) { w = vbw; h = vbh; }
        else { w = 300; h = 150; }
    } else if (!fh) {
        h = (vbw > 0 && vbh > 0) ? w * vbh / vbw : w;
    } else if (!fw) {
        w = (vbw > 0 && vbh > 0) ? h * vbw / vbh : h;
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

std::shared_ptr<Image> render_svg(const std::string& input, float target_w, float target_h, Color current_color) {
    float iw = 0, ih = 0;
    svg_intrinsic_size(input, &iw, &ih);
    if (target_w <= 0) target_w = iw;
    if (target_h <= 0) target_h = ih;
    int ss = target_w <= 64 ? 3 : target_w <= 256 ? 2 : 1;
    int pw = std::max(1, (int)std::lround(target_w * ss));
    int ph = std::max(1, (int)std::lround(target_h * ss));
    while ((long long)pw * ph > 16ll * 1024 * 1024 && ss > 1) {
        ss--;
        pw = std::max(1, (int)std::lround(target_w * ss));
        ph = std::max(1, (int)std::lround(target_h * ss));
    }
    pw = std::min(pw, 4096);
    ph = std::min(ph, 4096);
    char keybuf[128];
    snprintf(keybuf, sizeof(keybuf), "|%d|%d|%02x%02x%02x", pw, ph, current_color.r, current_color.g,
             current_color.b);
    static std::unordered_map<std::string, std::shared_ptr<Image>> cache;
    std::string key = input + keybuf;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    auto img = std::make_shared<Image>();
    img->url = "svg:inline";
    std::string prepared = prepare_svg(input, current_color);
    std::vector<char> buf(prepared.begin(), prepared.end());
    buf.push_back(0);
    NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);
    if (!image) {
        cache[key] = img;
        return img;
    }
    float sw = image->width > 0 ? image->width : iw;
    float sh = image->height > 0 ? image->height : ih;
    float scale = std::min((float)pw / sw, (float)ph / sh);
    if (scale <= 0) scale = 1;
    float tx = (pw - sw * scale) / 2.0f;
    float ty = (ph - sh * scale) / 2.0f;
    img->w = pw;
    img->h = ph;
    img->rgba.assign((size_t)pw * ph * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast) {
        nsvgRasterize(rast, image, tx, ty, scale, img->rgba.data(), pw, ph, pw * 4);
        nsvgDeleteRasterizer(rast);
        img->ok = true;
    }
    nsvgDelete(image);
    cache[key] = img;
    return img;
}
