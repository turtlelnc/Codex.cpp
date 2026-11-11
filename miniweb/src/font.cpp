#include "font.h"
#include "canvas.h"
#include "stb_truetype.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

struct FontFace {
    std::string path;
    std::string family;
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    bool bold = false, italic = false, mono = false, cjk = false, serif = false;
    int ascent = 0, descent = 0, line_gap = 0;
};

namespace {

std::string normalize_family(const std::string& s) {
    std::string out;
    bool last_space = false;
    for (char c : util::to_lower(s)) {
        if (c == '-' || c == '_' || std::isspace((unsigned char)c)) {
            if (!out.empty() && !last_space) out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(c);
            last_space = false;
        }
    }
    return util::trim(out);
}

std::string face_family_from_path(const std::string& path) {
    std::string name = std::filesystem::path(path).stem().string();
    std::string low = util::to_lower(name);
    static const char* suffixes[] = {" regular", " bold italic", " bold", " italic", " oblique", " medium",
                                     " light", " semibold", " black", " narrow", " condensed", " mt", " ms"};
    for (auto* s : suffixes) {
        size_t p = low.rfind(s);
        if (p != std::string::npos && p + strlen(s) == low.size()) {
            name = name.substr(0, p);
            low = low.substr(0, p);
        }
    }
    return normalize_family(name);
}

bool family_in(const std::string& family, const char* needle) {
    return family.find(needle) != std::string::npos;
}

uint32_t normalize_cp(uint32_t cp) {
    if (cp == 0) return ' ';
    if (cp < 32) return ' ';
    if (cp == 0xa0) return ' ';
    return cp;
}

} // namespace

FontManager::FontManager() = default;
FontManager::~FontManager() = default;

int FontManager::add_face(const std::string& path, bool bold, bool italic, bool mono, bool cjk, bool serif) {
    for (size_t i = 0; i < faces_.size(); i++) {
        if (faces_[i]->path == path) return (int)i;
    }
    bool ok = false;
    std::string data = util::read_file(path, &ok);
    if (!ok || data.empty()) return -1;
    auto face = std::make_unique<FontFace>();
    face->path = path;
    face->data.assign(data.begin(), data.end());
    int offset = stbtt_GetFontOffsetForIndex(face->data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&face->info, face->data.data(), offset)) {
        util::warn("cannot load font: %s", path.c_str());
        return -1;
    }
    face->family = face_family_from_path(path);
    face->bold = bold;
    face->italic = italic;
    face->mono = mono;
    face->serif = serif;
    face->cjk = cjk;
    stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->line_gap);
    int gi = stbtt_FindGlyphIndex(&face->info, 0x4e2d); // 中
    if (gi > 0) face->cjk = true;
    int idx = (int)faces_.size();
    faces_.push_back(std::move(face));
    return idx;
}

bool FontManager::init(const std::vector<std::string>& extra_fonts) {
    auto add = [&](const std::string& path, bool bold, bool italic, bool mono, bool cjk, bool serif) {
        if (util::file_exists(path)) add_face(path, bold, italic, mono, cjk, serif);
    };
#ifdef __APPLE__
    const std::string sf = "/System/Library/Fonts/";
    const std::string supp = "/System/Library/Fonts/Supplemental/";
    add(supp + "Arial.ttf", false, false, false, false, false);
    add(supp + "Arial Bold.ttf", true, false, false, false, false);
    add(supp + "Arial Italic.ttf", false, true, false, false, false);
    add(supp + "Arial Bold Italic.ttf", true, true, false, false, false);
    add(sf + "Helvetica.ttc", false, false, false, false, false);
    add(supp + "Times New Roman.ttf", false, false, false, false, true);
    add(supp + "Times New Roman Bold.ttf", true, false, false, false, true);
    add(supp + "Georgia.ttf", false, false, false, false, true);
    add(sf + "Menlo.ttc", false, false, true, false, false);
    add(supp + "Courier New.ttf", false, false, true, false, false);
    add(sf + "Hiragino Sans GB.ttc", false, false, false, true, false);
    add(sf + "STHeiti Light.ttc", false, false, false, true, false);
    add(sf + "STHeiti Medium.ttc", true, false, false, true, false);
    add(sf + "PingFang.ttc", false, false, false, true, false);
    add(supp + "Arial Unicode.ttf", false, false, false, true, false);
#elif defined(_WIN32)
    const std::string wf = "C:/Windows/Fonts/";
    add(wf + "arial.ttf", false, false, false, false, false);
    add(wf + "arialbd.ttf", true, false, false, false, false);
    add(wf + "ariali.ttf", false, true, false, false, false);
    add(wf + "arialbi.ttf", true, true, false, false, false);
    add(wf + "times.ttf", false, false, false, false, true);
    add(wf + "timesbd.ttf", true, false, false, false, true);
    add(wf + "georgia.ttf", false, false, false, false, true);
    add(wf + "consola.ttf", false, false, true, false, false);
    add(wf + "consolab.ttf", true, false, true, false, false);
    add(wf + "cour.ttf", false, false, true, false, false);
    add(wf + "msyh.ttc", false, false, false, true, false);
    add(wf + "msyhbd.ttc", true, false, false, true, false);
    add(wf + "simhei.ttf", false, false, false, true, false);
    add(wf + "simsun.ttc", false, false, false, true, true);
#else
    const char* regs[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    const char* bolds[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    };
    const char* italics[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
    };
    for (auto* p : regs) add(p, false, false, false, false, false);
    for (auto* p : bolds) add(p, true, false, false, false, false);
    for (auto* p : italics) add(p, false, true, false, false, false);
    add("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", false, false, true, false, false);
    add("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", true, false, true, false, false);
    add("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", false, false, true, false, false);
    add("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", false, false, false, true, false);
    add("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", false, false, false, true, false);
    add("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", true, false, false, true, false);
    add("/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc", true, false, false, true, false);
    add("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", false, false, false, true, false);
    add("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", false, false, false, true, false);
#endif
    for (auto& p : extra_fonts) {
        std::string low = util::to_lower(p);
        bool bold = low.find("bold") != std::string::npos || low.find("bd") != std::string::npos;
        bool italic = low.find("italic") != std::string::npos || low.find("oblique") != std::string::npos;
        bool mono = low.find("mono") != std::string::npos || low.find("courier") != std::string::npos ||
                    low.find("menlo") != std::string::npos || low.find("consol") != std::string::npos;
        bool serif = low.find("serif") != std::string::npos || low.find("times") != std::string::npos ||
                     low.find("georgia") != std::string::npos || low.find("song") != std::string::npos;
        bool cjk = low.find("cjk") != std::string::npos || low.find("hiragino") != std::string::npos ||
                   low.find("pingfang") != std::string::npos || low.find("heiti") != std::string::npos ||
                   low.find("songti") != std::string::npos || low.find("msyh") != std::string::npos ||
                   low.find("simhei") != std::string::npos || low.find("wqy") != std::string::npos;
        add(p, bold, italic, mono, cjk, serif);
    }
    pick_defaults();
    util::log("fonts loaded: %d", (int)faces_.size());
    return !faces_.empty();
}

void FontManager::pick_defaults() {
    auto set = [&](int i, bool bold, bool italic, bool mono, bool cjk, bool serif) {
        if (cjk) {
            if (bold && cjk_bold_ < 0) cjk_bold_ = i;
            if (!bold && !italic && cjk_regular_ < 0) cjk_regular_ = i;
        } else if (mono) {
            if (bold && !italic && mono_bold_ < 0) mono_bold_ = i;
            if (!bold && !italic && mono_regular_ < 0) mono_regular_ = i;
        } else if (serif) {
            if (bold && !italic && serif_bold_ < 0) serif_bold_ = i;
            if (!bold && !italic && serif_regular_ < 0) serif_regular_ = i;
        } else {
            if (bold && italic && default_bold_italic_ < 0) default_bold_italic_ = i;
            else if (bold && !italic && default_bold_ < 0) default_bold_ = i;
            else if (!bold && italic && default_italic_ < 0) default_italic_ = i;
            else if (!bold && !italic && default_regular_ < 0) default_regular_ = i;
        }
    };
    for (size_t i = 0; i < faces_.size(); i++)
        set((int)i, faces_[i]->bold, faces_[i]->italic, faces_[i]->mono, faces_[i]->cjk, faces_[i]->serif);
    if (default_regular_ < 0) {
        for (size_t i = 0; i < faces_.size(); i++) if (!faces_[i]->cjk && !faces_[i]->mono) { default_regular_ = (int)i; break; }
    }
    if (default_regular_ < 0 && !faces_.empty()) default_regular_ = 0;
    if (default_bold_ < 0) default_bold_ = default_regular_;
    if (default_italic_ < 0) default_italic_ = default_regular_;
    if (default_bold_italic_ < 0) default_bold_italic_ = default_bold_;
    if (mono_regular_ < 0) mono_regular_ = default_regular_;
    if (mono_bold_ < 0) mono_bold_ = mono_regular_;
    if (serif_regular_ < 0) serif_regular_ = default_regular_;
    if (serif_bold_ < 0) serif_bold_ = serif_regular_;
    if (cjk_regular_ < 0) cjk_regular_ = default_regular_;
    if (cjk_bold_ < 0) cjk_bold_ = cjk_regular_;
}

int FontManager::primary_face(const ComputedStyle& style, bool for_cjk) {
    if (faces_.empty()) return -1;
    if (for_cjk) {
        if (style.font_weight >= 600) return cjk_bold_ >= 0 ? cjk_bold_ : cjk_regular_;
        return cjk_regular_;
    }
    std::string fam = normalize_family(style.font_family);
    bool mono = family_in(fam, "mono") || family_in(fam, "courier") || family_in(fam, "menlo") ||
                family_in(fam, "consol");
    bool serif = (family_in(fam, "serif") && !family_in(fam, "sans")) || family_in(fam, "times") ||
                 family_in(fam, "georgia") || family_in(fam, "song");
    bool bold = style.font_weight >= 600;
    bool italic = style.font_style != FontStyle::Normal;
    if (!fam.empty() && fam != "sans-serif" && fam != "system-ui" && !mono && !serif) {
        int best = -1, best_score = -1;
        for (size_t i = 0; i < faces_.size(); i++) {
            auto& f = faces_[i];
            if (f->cjk) continue;
            if (f->family != fam) continue;
            int score = 0;
            if (f->bold == bold) score += 2;
            if (f->italic == italic) score += 1;
            if (score > best_score) { best_score = score; best = (int)i; }
        }
        if (best >= 0) return best;
    }
    if (mono) return bold && mono_bold_ >= 0 ? mono_bold_ : mono_regular_;
    if (serif) return bold && serif_bold_ >= 0 ? serif_bold_ : serif_regular_;
    if (bold && italic) return default_bold_italic_;
    if (bold) return default_bold_;
    if (italic) return default_italic_;
    return default_regular_;
}

int FontManager::face_for(uint32_t cp, const ComputedStyle& style) {
    if (faces_.empty()) return -1;
    cp = normalize_cp(cp);
    bool cjk_char = util::is_cjk(cp);
    int first = primary_face(style, cjk_char);
    if (first >= 0 && stbtt_FindGlyphIndex(&faces_[first]->info, (int)cp) > 0) return first;
    if (cjk_char) {
        int order[2] = {cjk_regular_, cjk_bold_};
        for (int i : order) {
            if (i >= 0 && stbtt_FindGlyphIndex(&faces_[i]->info, (int)cp) > 0) return i;
        }
    }
    bool bold = style.font_weight >= 600;
    bool italic = style.font_style != FontStyle::Normal;
    for (size_t k = 0; k < faces_.size(); k++) {
        auto& f = faces_[k];
        if (f->cjk && !cjk_char) continue;
        if (f->bold != bold && f->bold) continue; // prefer requested weight but allow fallback below
        if (stbtt_FindGlyphIndex(&f->info, (int)cp) > 0) return (int)k;
    }
    for (size_t k = 0; k < faces_.size(); k++) {
        if (stbtt_FindGlyphIndex(&faces_[k]->info, (int)cp) > 0) return (int)k;
    }
    (void)italic;
    return first;
}

const GlyphBitmap* FontManager::get_glyph(uint32_t cp, float px_size, const ComputedStyle& style) {
    if (faces_.empty()) return nullptr;
    cp = normalize_cp(cp);
    int face_idx = face_for(cp, style);
    if (face_idx < 0) return nullptr;
    int px = std::max(1, (int)std::lround(px_size));
    uint64_t key = ((uint64_t)(face_idx & 0xffff) << 40) | ((uint64_t)(px & 0xffff) << 24) |
                   (uint64_t)(cp & 0xffffff);
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return &it->second;
    FontFace& face = *faces_[face_idx];
    float scale = stbtt_ScaleForPixelHeight(&face.info, (float)px);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&face.info, (int)cp, &adv, &lsb);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&face.info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
    GlyphBitmap g;
    g.ok = true;
    g.w = std::max(0, x1 - x0);
    g.h = std::max(0, y1 - y0);
    g.xoff = x0;
    g.yoff = y0;
    g.advance = adv * scale;
    if (g.w > 0 && g.h > 0) {
        g.bitmap.resize((size_t)g.w * g.h);
        stbtt_MakeCodepointBitmap(&face.info, g.bitmap.data(), g.w, g.h, g.w, scale, scale, (int)cp);
    }
    auto res = glyphs_.emplace(key, std::move(g));
    return &res.first->second;
}

float FontManager::text_width(const std::string& utf8, const ComputedStyle& style) {
    if (utf8.empty()) return 0;
    float x = 0;
    int prev_face = -1;
    uint32_t prev_cp = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        i = util::utf8_next(utf8, i, &cp);
        cp = normalize_cp(cp);
        const GlyphBitmap* g = get_glyph(cp, style.font_size, style);
        if (!g) {
            x += style.font_size * 0.55f;
            prev_face = -1;
            continue;
        }
        int face_idx = face_for(cp, style);
        if (prev_face >= 0 && face_idx == prev_face) {
            x += stbtt_GetCodepointKernAdvance(&faces_[face_idx]->info, (int)prev_cp, (int)cp) *
                 stbtt_ScaleForPixelHeight(&faces_[face_idx]->info, style.font_size);
        }
        x += g->advance;
        x += style.letter_spacing;
        if (style.font_weight >= 600 && face_idx >= 0 && !faces_[face_idx]->bold) x += 0.4f;
        prev_face = face_idx;
        prev_cp = cp;
    }
    return x;
}

float FontManager::ascent(const ComputedStyle& style) {
    int f = primary_face(style, false);
    if (f < 0) return style.font_size * 0.8f;
    float scale = stbtt_ScaleForPixelHeight(&faces_[f]->info, style.font_size);
    return faces_[f]->ascent * scale;
}

float FontManager::descent(const ComputedStyle& style) {
    int f = primary_face(style, false);
    if (f < 0) return style.font_size * 0.2f;
    float scale = stbtt_ScaleForPixelHeight(&faces_[f]->info, style.font_size);
    return -faces_[f]->descent * scale;
}

float FontManager::normal_line_height(const ComputedStyle& style) {
    int f = primary_face(style, false);
    if (f < 0) return style.font_size * 1.2f;
    float scale = stbtt_ScaleForPixelHeight(&faces_[f]->info, style.font_size);
    return (faces_[f]->ascent - faces_[f]->descent + faces_[f]->line_gap) * scale;
}

void FontManager::draw_text(Canvas& canvas, const std::string& utf8, float x, float baseline,
                            const ComputedStyle& style, Color color, float opacity) {
    if (utf8.empty() || faces_.empty()) return;
    float pen = x;
    int prev_face = -1;
    uint32_t prev_cp = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        i = util::utf8_next(utf8, i, &cp);
        cp = normalize_cp(cp);
        const GlyphBitmap* g = get_glyph(cp, style.font_size, style);
        int face_idx = face_for(cp, style);
        if (!g) {
            pen += style.font_size * 0.55f;
            prev_face = -1;
            continue;
        }
        if (prev_face >= 0 && face_idx == prev_face) {
            pen += stbtt_GetCodepointKernAdvance(&faces_[face_idx]->info, (int)prev_cp, (int)cp) *
                   stbtt_ScaleForPixelHeight(&faces_[face_idx]->info, style.font_size);
        }
        if (g->w > 0 && g->h > 0) {
            int bx = (int)std::lround(pen) + g->xoff;
            int by = (int)std::lround(baseline) + g->yoff;
            for (int yy = 0; yy < g->h; yy++) {
                for (int xx = 0; xx < g->w; xx++) {
                    uint8_t a = g->bitmap[(size_t)yy * g->w + xx];
                    if (!a) continue;
                    canvas.blend_pixel(bx + xx, by + yy, color, (a / 255.0f) * opacity);
                    if (style.font_weight >= 600 && face_idx >= 0 && !faces_[face_idx]->bold)
                        canvas.blend_pixel(bx + xx + 1, by + yy, color, (a / 255.0f) * opacity * 0.8f);
                }
            }
        }
        pen += g->advance;
        pen += style.letter_spacing;
        if (style.font_weight >= 600 && face_idx >= 0 && !faces_[face_idx]->bold) pen += 0.4f;
        prev_face = face_idx;
        prev_cp = cp;
    }
}
