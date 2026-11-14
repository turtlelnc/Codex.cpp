#pragma once
#include "style.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Canvas;

struct GlyphBitmap {
    bool ok = false;
    int w = 0, h = 0;
    int xoff = 0, yoff = 0;
    float advance = 0;
    std::vector<uint8_t> bitmap;
};

struct FontFace;

class FontManager {
public:
    FontManager();
    ~FontManager();
    bool init(const std::vector<std::string>& extra_fonts = {});
    bool any_font() const { return !faces_.empty(); }

    float text_width(const std::string& utf8, const ComputedStyle& style);
    float ascent(const ComputedStyle& style);
    float descent(const ComputedStyle& style);
    float normal_line_height(const ComputedStyle& style);
    void draw_text(Canvas& canvas, const std::string& utf8, float x, float baseline,
                   const ComputedStyle& style, Color color, float opacity = 1.0f);

    const GlyphBitmap* get_glyph(uint32_t cp, float px_size, const ComputedStyle& style);

private:
    std::vector<std::unique_ptr<FontFace>> faces_;
    // A page can invent arbitrarily many family lists; insertion is bounded.
    std::unordered_map<std::string,int> primary_cache_;
    // Outer key: face and exact float size; inner key: Unicode codepoint.
    std::unordered_map<uint64_t, std::unordered_map<uint32_t, GlyphBitmap>> glyphs_;
    int default_regular_ = -1, default_bold_ = -1, default_italic_ = -1, default_bold_italic_ = -1;
    int mono_regular_ = -1, mono_bold_ = -1;
    int serif_regular_ = -1, serif_bold_ = -1;
    int cjk_regular_ = -1, cjk_bold_ = -1;

    int add_face(const std::string& path, bool bold, bool italic, bool mono, bool cjk, bool serif);
    int primary_face(const ComputedStyle& style, bool for_cjk = false);
    int select_primary_face(const ComputedStyle& style, bool for_cjk);
    int face_for(uint32_t cp, const ComputedStyle& style);
    void pick_defaults();
};
