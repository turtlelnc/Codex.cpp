#include "layout.h"
#include "canvas.h"
#include "font.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {

bool is_whitespace_text(const std::string& s) {
    for (char c : s) if (!std::isspace((unsigned char)c)) return false;
    return !s.empty();
}

Color with_opacity(Color c, float op) {
    c.a = (uint8_t)std::lround(std::clamp(c.a * op, 0.0f, 255.0f));
    return c;
}

void paint_node(Canvas& canvas, Node* n, FontManager& fonts, ImageCache* images, float ox, float oy,
                float opacity);

void paint_text_item(Canvas& canvas, FontManager& fonts, const LineItem& item, float cx, float cy, float opacity) {
    if (item.kind != LineItem::Text) return;
    if (item.text.empty() || is_whitespace_text(item.text)) return;
    const ComputedStyle* st = item.style;
    if (!st || st->visibility != Visibility::Visible) return;
    float x = cx + item.x;
    float baseline = cy + item.y + item.baseline;
    Color c = with_opacity(item.color, opacity);
    fonts.draw_text(canvas, item.text, x, baseline, *st, c, 1.0f);
    if (item.underline || item.line_through) {
        float th = std::max(1.0f, item.font_size / 14.0f);
        if (item.underline) canvas.fill_rect(RectF{x, baseline + 2, item.w, th}, c, 1.0f);
        if (item.line_through)
            canvas.fill_rect(RectF{x, baseline - item.font_size * 0.32f, item.w, th}, c, 1.0f);
    }
}

void paint_input_value(Canvas& canvas, Node* n, FontManager& fonts, float cx, float cy, float opacity) {
    const ComputedStyle& st = n->style;
    std::string type = util::to_lower(n->attr_or("type", "text"));
    if (type == "hidden") return;
    if (type == "checkbox" || type == "radio") {
        float s = std::min(14.0f, std::max(8.0f, st.font_size));
        RectF box{cx + 2, cy + (n->layout.content_h - s) / 2, s, s};
        canvas.fill_rect(box, Color{255, 255, 255, 255}, opacity);
        canvas.fill_rect(RectF{box.x, box.y, box.w, 1}, Color{120, 120, 120, 255}, opacity);
        canvas.fill_rect(RectF{box.x, box.y + box.h - 1, box.w, 1}, Color{120, 120, 120, 255}, opacity);
        canvas.fill_rect(RectF{box.x, box.y, 1, box.h}, Color{120, 120, 120, 255}, opacity);
        canvas.fill_rect(RectF{box.x + box.w - 1, box.y, 1, box.h}, Color{120, 120, 120, 255}, opacity);
        if (n->has_attr("checked")) {
            if (type == "checkbox") {
                canvas.fill_rect(RectF{box.x + 3, box.y + 6, 3, 2}, Color{30, 30, 30, 255}, opacity);
                canvas.fill_rect(RectF{box.x + 5, box.y + 8, 3, 2}, Color{30, 30, 30, 255}, opacity);
                canvas.fill_rect(RectF{box.x + 7, box.y + 4, 3, 2}, Color{30, 30, 30, 255}, opacity);
            } else {
                canvas.fill_rounded_rect(RectF{box.x + 4, box.y + 4, s - 8, s - 8}, 4, Color{30, 30, 30, 255}, opacity);
            }
        }
        return;
    }
    std::string value = n->attr("value");
    std::string placeholder = n->attr("placeholder");
    bool use_placeholder = value.empty() && !placeholder.empty();
    std::string text = use_placeholder ? placeholder : value;
    if (text.empty()) return;
    Color color = use_placeholder ? Color{150, 150, 150, 255} : st.color;
    float pad_top = resolve_length(st.padding.top, n->layout.content_w, st.font_size, 16.0f, true);
    float baseline = cy + pad_top + fonts.ascent(st) + 1;
    fonts.draw_text(canvas, text, cx + 4, baseline, st, with_opacity(color, opacity), 1.0f);
}

void paint_select_value(Canvas& canvas, Node* n, FontManager& fonts, float cx, float cy, float opacity) {
    Node* selected = nullptr;
    Node* first = nullptr;
    for (auto* c : n->children) {
        if (!c->is_element("option")) continue;
        if (!first) first = c;
        if (c->has_attr("selected")) { selected = c; break; }
    }
    Node* opt = selected ? selected : first;
    if (!opt) return;
    std::string text = util::trim(opt->text_content());
    if (text.empty()) text = opt->attr("label");
    if (text.empty()) return;
    float pad_top = resolve_length(n->style.padding.top, n->layout.content_w, n->style.font_size, 16.0f, true);
    float baseline = cy + pad_top + fonts.ascent(n->style) + 1;
    fonts.draw_text(canvas, text, cx + 4, baseline, n->style, with_opacity(n->style.color, opacity), 1.0f);
}

void paint_node(Canvas& canvas, Node* n, FontManager& fonts, ImageCache* images, float ox, float oy,
                float opacity) {
    if (!n || !n->is_element()) return;
    if (n->style.display == Display::None) return;
    const bool visible = n->style.visibility == Visibility::Visible;
    const LayoutInfo& L = n->layout;
    if (!L.has_box) return;
    float op = opacity * std::clamp(n->style.opacity, 0.0f, 1.0f);
    if (op <= 0.001f) return;
    RectF box=visual_box(n,ox,oy);
    if (visible) {
        for (auto& sh : n->style.shadows) {
            if (sh.color.a == 0) continue;
            RectF r{box.x + sh.x - sh.spread, box.y + sh.y - sh.spread, box.w + 2 * sh.spread,
                    box.h + 2 * sh.spread};
            if (r.w <= 0 || r.h <= 0) continue;
            int layers = std::min(12, std::max(0, (int)std::lround(sh.blur / 2.0f)));
            for (int i = layers; i >= 0; i--) {
                float grow = layers > 0 ? sh.blur * (float)i / layers : 0;
                Color c = sh.color;
                c.a = (uint8_t)std::lround(sh.color.a / (layers + 1.0f));
                canvas.fill_rounded_rect(RectF{r.x - grow / 2, r.y - grow / 2, r.w + grow, r.h + grow},
                                         n->style.border_radius + grow / 2, c, op);
            }
        }
        if (n->style.background.a > 0) {
            canvas.fill_rounded_rect(box, n->style.border_radius, n->style.background, op);
        }
        if (n->style.background_gradient.enabled) {
            canvas.fill_linear_gradient(box, n->style.background_gradient, op);
        }
        if (images && !n->style.background_image.empty()) {
            std::string url = n->doc ? n->doc->resolve(n->style.background_image) : n->style.background_image;
            auto img = images->get(url);
            if (img && img->ok) {
                ObjectFit fit = n->style.background_size == BackgroundSize::Cover ? ObjectFit::Cover
                                : n->style.background_size == BackgroundSize::Contain ? ObjectFit::Contain
                                : n->style.background_size == BackgroundSize::Stretch ? ObjectFit::Fill
                                                                                     : ObjectFit::None;
                canvas.push_clip(box);
                canvas.draw_image(*img, box, fit, op, n->style.border_radius);
                canvas.pop_clip();
            }
        }
        bool has_border = border_left(n->style) > 0 || border_right(n->style) > 0 || border_top(n->style) > 0 ||
                          border_bottom(n->style) > 0;
        if (has_border) canvas.draw_border(box, n->style, op);
    }

    bool clip = n->style.overflow == Overflow::Hidden;
    if (clip) canvas.push_clip(box);

    float cx = box.x + L.content_x;
    float cy = box.y + L.content_y;
    RectF content{cx, cy, L.content_w, L.content_h};

    if (visible && L.is_image) {
        if (L.image && L.image->ok) {
            canvas.draw_image(*L.image, content, n->style.object_fit, op, n->style.border_radius);
        } else {
            canvas.fill_rect(content, Color{232, 232, 232, 255}, op);
            canvas.fill_rect(RectF{content.x, content.y, content.w, 1}, Color{200, 200, 200, 255}, op);
            canvas.fill_rect(RectF{content.x, content.y + content.h - 1, content.w, 1}, Color{200, 200, 200, 255}, op);
            if (content.w > 40 && content.h > 20)
                fonts.draw_text(canvas, "image", content.x + 6, content.y + fonts.ascent(n->style) + 4,
                                n->style, Color{140, 140, 140, 255}, op);
        }
    }

    for (auto& line : L.lines) {
        for (auto& item : line.items) {
            if (item.kind == LineItem::Text) {
                paint_text_item(canvas, fonts, item, cx, cy, op);
            } else if (item.kind == LineItem::ImageItem && item.style && item.style->visibility == Visibility::Visible) {
                RectF dest{cx + item.x, cy + item.y, item.w, item.h};
                if (item.image && item.image->ok) {
                    ObjectFit fit = item.style ? item.style->object_fit : ObjectFit::Fill;
                    float rad = item.style ? item.style->border_radius : 0;
                    canvas.draw_image(*item.image, dest, fit, op, rad);
                } else {
                    canvas.fill_rect(dest, Color{232, 232, 232, 255}, op);
                }
            }
        }
    }

    for (auto* child : paint_order(n)) paint_node(canvas, child, fonts, images, cx, cy, op);

    if (visible && n->is_element("li")) {
        Node* p = n->parent;
        bool unordered = p && p->is_element("ul");
        if (unordered) {
            float baseline = cy;
            if (!L.lines.empty()) baseline = cy + L.lines[0].y + L.lines[0].baseline;
            else baseline = cy + fonts.ascent(n->style);
            fonts.draw_text(canvas, "\xe2\x80\xa2", box.x - 15, baseline, n->style,
                            with_opacity(n->style.color, op), 1.0f);
        }
    }

    if (visible && n->is_element("input")) {
        paint_input_value(canvas, n, fonts, cx, cy, op);
    } else if (visible && n->is_element("select")) {
        paint_select_value(canvas, n, fonts, cx, cy, op);
    } else if (visible && n->is_element("textarea") && n->children.empty()) {
        std::string value = n->attr("value");
        if (!value.empty()) {
            float pad_top = resolve_length(n->style.padding.top, n->layout.content_w, n->style.font_size, 16.0f, true);
            float baseline = cy + pad_top + fonts.ascent(n->style);
            fonts.draw_text(canvas, value, cx + 3, baseline, n->style, with_opacity(n->style.color, op), 1.0f);
        }
    }

    if (clip) canvas.pop_clip();
}

} // namespace

void paint_document(Canvas& canvas, Document& doc, FontManager& fonts, ImageCache* images) {
    canvas.clear(Color{255, 255, 255, 255});
    Node* bg = doc.body ? doc.body : doc.html;
    if (bg && bg->style.background.a > 0) {
        canvas.fill_rect(RectF{0, 0, (float)canvas.width(), (float)canvas.height()}, bg->style.background, 1.0f);
    }
    if (bg && bg->style.background_gradient.enabled) {
        canvas.fill_linear_gradient(RectF{0, 0, (float)canvas.width(), (float)canvas.height()},
                                    bg->style.background_gradient, 1.0f);
    }
    if (images && bg && !bg->style.background_image.empty()) {
        std::string url = bg->doc ? bg->doc->resolve(bg->style.background_image) : bg->style.background_image;
        auto img = images->get(url);
        if (img && img->ok)
            canvas.draw_image(*img, RectF{0, 0, (float)canvas.width(), (float)canvas.height()},
                              ObjectFit::Cover, 1.0f);
    }
    if (!doc.root) return;
    for (auto* c : doc.root->children) {
        if (c && c->is_element()) paint_node(canvas, c, fonts, images, 0, 0, 1.0f);
    }
}
