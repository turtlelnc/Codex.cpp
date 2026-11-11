#include "canvas.h"
#include "stb_image_write.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

Canvas::Canvas(int w, int h) : w_(std::max(1, w)), h_(std::max(1, h)) {
    if(w_>4096||h_>4096||(size_t)w_*h_>8*1024*1024)throw std::runtime_error("Preview canvas exceeds limits");
    pixels_.assign((size_t)w_ * h_ * 4, 255);
    clips_.push_back(RectF{0, 0, (float)w_, (float)h_});
}

void Canvas::clear(Color c) {
    for (int y = 0; y < h_; y++) {
        for (int x = 0; x < w_; x++) {
            size_t i = ((size_t)y * w_ + x) * 4;
            if (!clip_contains(x, y)) continue;
            pixels_[i] = c.r; pixels_[i + 1] = c.g; pixels_[i + 2] = c.b; pixels_[i + 3] = c.a;
        }
    }
}

void Canvas::blend_at(int x, int y, Color c, float alpha) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    if (!clip_contains(x, y)) return;
    float a = (c.a / 255.0f) * std::clamp(alpha, 0.0f, 1.0f);
    if (a <= 0.0001f) return;
    size_t i = ((size_t)y * w_ + x) * 4;
    if (a >= 0.999f) {
        pixels_[i] = c.r; pixels_[i + 1] = c.g; pixels_[i + 2] = c.b; pixels_[i + 3] = 255;
        return;
    }
    float da = pixels_[i + 3] / 255.0f;
    float oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto mix = [&](uint8_t s, uint8_t d) {
        return (uint8_t)std::lround((s * a + d * da * (1 - a)) / oa);
    };
    pixels_[i] = mix(c.r, pixels_[i]);
    pixels_[i + 1] = mix(c.g, pixels_[i + 1]);
    pixels_[i + 2] = mix(c.b, pixels_[i + 2]);
    pixels_[i + 3] = (uint8_t)std::lround(oa * 255);
}

void Canvas::blend_pixel(int x, int y, Color c, float alpha) { blend_at(x, y, c, alpha); }

bool Canvas::clip_contains(int x, int y) const {
    if (clips_.empty()) return true;
    const RectF& r = clips_.back();
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void Canvas::push_clip(const RectF& r) {
    RectF c = r;
    if (!clips_.empty()) {
        const RectF& p = clips_.back();
        float x0 = std::max(p.x, c.x), y0 = std::max(p.y, c.y);
        float x1 = std::min(p.x + p.w, c.x + c.w), y1 = std::min(p.y + p.h, c.y + c.h);
        c = RectF{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    clips_.push_back(c);
}

void Canvas::pop_clip() {
    if (clips_.size() > 1) clips_.pop_back();
}

void Canvas::reset_clip() {
    clips_.clear();
    clips_.push_back(RectF{0, 0, (float)w_, (float)h_});
}

void Canvas::fill_rect(const RectF& r, Color c, float opacity) {
    if (r.w <= 0 || r.h <= 0 || c.a == 0 || opacity <= 0) return;
    int x0 = (int)std::floor(r.x);
    int y0 = (int)std::floor(r.y);
    int x1 = (int)std::ceil(r.x + r.w);
    int y1 = (int)std::ceil(r.y + r.h);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, w_); y1 = std::min(y1, h_);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) blend_at(x, y, c, opacity);
}

void Canvas::fill_rounded_rect(const RectF& r, float radius, Color c, float opacity) {
    if (radius <= 0.5f) { fill_rect(r, c, opacity); return; }
    float rad = std::min({radius, r.w / 2, r.h / 2});
    int x0 = (int)std::floor(r.x), y0 = (int)std::floor(r.y);
    int x1 = (int)std::ceil(r.x + r.w), y1 = (int)std::ceil(r.y + r.h);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, w_); y1 = std::min(y1, h_);
    auto inside = [&](float px, float py) {
        if (px < r.x || py < r.y || px >= r.x + r.w || py >= r.y + r.h) return false;
        float cx = std::clamp(px, r.x + rad, r.x + r.w - rad);
        float cy = std::clamp(py, r.y + rad, r.y + r.h - rad);
        float dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy <= rad * rad;
    };
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++)
                    if (inside(x + 0.25f + sx * 0.5f, y + 0.25f + sy * 0.5f)) hits++;
            if (hits) blend_at(x, y, c, opacity * (hits / 4.0f));
        }
    }
}

void Canvas::fill_linear_gradient(const RectF& r, const Gradient& g, float opacity) {
    if (!g.enabled || g.stops.size() < 2 || r.w <= 0 || r.h <= 0) return;
    const float pi = 3.14159265358979323846f;
    float rad = g.angle * pi / 180.0f;
    float dx = std::sin(rad), dy = -std::cos(rad);
    float half = (std::fabs(dx) * r.w + std::fabs(dy) * r.h) / 2.0f;
    if (half <= 0.0001f) return;
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    float sx = cx - dx * half, sy = cy - dy * half;
    float denom = 2 * half;
    int x0 = (int)std::floor(r.x), y0 = (int)std::floor(r.y);
    int x1 = (int)std::ceil(r.x + r.w), y1 = (int)std::ceil(r.y + r.h);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, w_); y1 = std::min(y1, h_);
    auto color_at = [&](float t) -> Color {
        t = std::clamp(t, 0.0f, 1.0f);
        const auto& st = g.stops;
        if (t <= st.front().pos) return st.front().color;
        if (t >= st.back().pos) return st.back().color;
        for (size_t i = 1; i < st.size(); i++) {
            if (t <= st[i].pos) {
                float span = st[i].pos - st[i - 1].pos;
                float u = span <= 0.00001f ? 0 : (t - st[i - 1].pos) / span;
                Color a = st[i - 1].color, b = st[i].color;
                auto mix = [&](uint8_t x, uint8_t y) { return (uint8_t)std::lround(x + (y - x) * u); };
                return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
            }
        }
        return st.back().color;
    };
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float t = ((x + 0.5f - sx) * dx + (y + 0.5f - sy) * dy) / denom;
            Color c = color_at(t);
            if (c.a) blend_at(x, y, c, opacity);
        }
    }
}

void Canvas::draw_border(const RectF& r, const ComputedStyle& st, float opacity) {
    float l = border_left(st), rr = border_right(st), t = border_top(st), b = border_bottom(st);
    if (l > 0) fill_rect(RectF{r.x, r.y, l, r.h}, st.border[3].color, opacity);
    if (rr > 0) fill_rect(RectF{r.x + r.w - rr, r.y, rr, r.h}, st.border[1].color, opacity);
    if (t > 0) fill_rect(RectF{r.x + l, r.y, r.w - l - rr, t}, st.border[0].color, opacity);
    if (b > 0) fill_rect(RectF{r.x + l, r.y + r.h - b, r.w - l - rr, b}, st.border[2].color, opacity);
}

void Canvas::draw_image(const Image& img, const RectF& dest, ObjectFit fit, float opacity, float radius) {
    if (!img.ok || dest.w <= 0 || dest.h <= 0) return;
    float rad = std::min({std::max(0.0f, radius), dest.w / 2, dest.h / 2});
    auto rounded_inside = [&](float px, float py) {
        if (rad <= 0.5f) return true;
        if (px < dest.x || py < dest.y || px >= dest.x + dest.w || py >= dest.y + dest.h) return false;
        float qx = std::clamp(px, dest.x + rad, dest.x + dest.w - rad);
        float qy = std::clamp(py, dest.y + rad, dest.y + dest.h - rad);
        float dx = px - qx, dy = py - qy;
        return dx * dx + dy * dy <= rad * rad;
    };
    float dw = dest.w, dh = dest.h;
    float dx = dest.x, dy = dest.y;
    float sx = 0, sy = 0, sw = (float)img.w, sh = (float)img.h;
    if (fit == ObjectFit::Contain || fit == ObjectFit::ScaleDown) {
        float scale = std::min(dw / img.w, dh / img.h);
        if (fit == ObjectFit::ScaleDown) scale = std::min(scale, 1.0f);
        float nw = img.w * scale, nh = img.h * scale;
        dx += (dw - nw) / 2;
        dy += (dh - nh) / 2;
        dw = nw; dh = nh;
    } else if (fit == ObjectFit::Cover) {
        float scale = std::max(dw / img.w, dh / img.h);
        float nw = img.w * scale, nh = img.h * scale;
        sx += (nw - dw) / (2 * scale);
        sy += (nh - dh) / (2 * scale);
        sw = dw / scale;
        sh = dh / scale;
    } else if (fit == ObjectFit::None) {
        float nw = (float)img.w, nh = (float)img.h;
        dx += (dw - nw) / 2;
        dy += (dh - nh) / 2;
        dw = nw; dh = nh;
    }
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = (int)std::floor(dx), y0 = (int)std::floor(dy);
    int x1 = (int)std::ceil(dx + dw), y1 = (int)std::ceil(dy + dh);
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, w_); y1 = std::min(y1, h_);
    auto sample = [&](float u, float v, int c) -> float {
        u = std::clamp(u, 0.0f, (float)img.w - 1.001f);
        v = std::clamp(v, 0.0f, (float)img.h - 1.001f);
        int iu = (int)u, iv = (int)v;
        float fu = u - iu, fv = v - iv;
        auto px = [&](int x, int y) -> uint8_t {
            x = std::clamp(x, 0, img.w - 1);
            y = std::clamp(y, 0, img.h - 1);
            return img.rgba[((size_t)y * img.w + x) * 4 + c];
        };
        float a = px(iu, iv) * (1 - fu) * (1 - fv) + px(iu + 1, iv) * fu * (1 - fv) +
                  px(iu, iv + 1) * (1 - fu) * fv + px(iu + 1, iv + 1) * fu * fv;
        return a;
    };
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float u = sx + ((x + 0.5f) - dx) / dw * sw;
            float v = sy + ((y + 0.5f) - dy) / dh * sh;
            Color c;
            c.r = (uint8_t)std::lround(sample(u, v, 0));
            c.g = (uint8_t)std::lround(sample(u, v, 1));
            c.b = (uint8_t)std::lround(sample(u, v, 2));
            c.a = (uint8_t)std::lround(sample(u, v, 3));
            if (c.a && rounded_inside(x + 0.5f, y + 0.5f)) blend_at(x, y, c, opacity);
        }
    }
}

bool Canvas::save_png(const std::string& path) const {
    return stbi_write_png(path.c_str(), w_, h_, 4, pixels_.data(), w_ * 4) != 0;
}
