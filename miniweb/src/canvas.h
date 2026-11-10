#pragma once
#include "image.h"
#include "style.h"
#include "util.h"
#include <cstdint>
#include <string>
#include <vector>

class Canvas {
public:
    Canvas(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }

    void clear(Color c);
    void fill_rect(const RectF& r, Color c, float opacity = 1.0f);
    void fill_rounded_rect(const RectF& r, float radius, Color c, float opacity = 1.0f);
    void fill_linear_gradient(const RectF& r, const Gradient& g, float opacity = 1.0f);
    void draw_border(const RectF& r, const ComputedStyle& st, float opacity = 1.0f);
    void draw_image(const Image& img, const RectF& dest, ObjectFit fit, float opacity = 1.0f,
                    float radius = 0.0f);
    void blend_pixel(int x, int y, Color c, float alpha);
    void push_clip(const RectF& r);
    void pop_clip();
    void reset_clip();
    bool save_png(const std::string& path) const;

private:
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> pixels_;
    std::vector<RectF> clips_;

    bool clip_contains(int x, int y) const;
    void blend_at(int x, int y, Color c, float alpha);
};
