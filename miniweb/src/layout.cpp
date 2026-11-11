#include "layout.h"
#include "canvas.h"
#include "svg.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace {

struct Token {
    enum Kind { Word, Space, Break, ImageTok, BoxTok } kind = Word;
    Node* node = nullptr;
    Node* link = nullptr;
    const ComputedStyle* style = nullptr;
    std::string text;
    std::shared_ptr<Image> image;
    Node* box = nullptr;
    float w = 0, h = 0, ascent = 0, descent = 0;
    bool preserve_space = false;
    bool wrap = true;
};

float resolve_l(const Length& l, float basis, float fs) {
    return resolve_length(l, basis, fs, 16.0f, true);
}

std::string transform_text(const std::string& in, TextTransform tt) {
    if (tt == TextTransform::None) return in;
    std::string out = in;
    if (tt == TextTransform::Uppercase) {
        for (char& c : out) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    } else if (tt == TextTransform::Lowercase) {
        for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    } else if (tt == TextTransform::Capitalize) {
        bool start = true;
        for (char& c : out) {
            if (std::isspace((unsigned char)c)) { start = true; continue; }
            if (start && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            start = false;
        }
    }
    return out;
}

std::string collapse_text(const std::string& in, const ComputedStyle& st) {
    if (st.white_space == WhiteSpace::Pre || st.white_space == WhiteSpace::PreWrap)
        return transform_text(in, st.text_transform);
    std::string s = transform_text(in, st.text_transform);
    std::string out;
    bool ws = false;
    for (char c : s) {
        bool isw = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if (isw) {
            if (!ws && !out.empty()) out.push_back(' ');
            ws = true;
        } else {
            out.push_back(c);
            ws = false;
        }
    }
    return util::trim(out);
}

} // namespace

class LayoutEngine {
public:
    LayoutEngine(Document& d, LayoutContext& c)
        : doc(d), ctx(c), fonts(*c.fonts), images(*c.images), vw(c.viewport_w), vh(c.viewport_h) {}

    void run();

private:
    Document& doc;
    LayoutContext& ctx;
    FontManager& fonts;
    ImageCache& images;
    float vw = 1280, vh = 800;
    std::unordered_map<Node*, std::vector<Node*>> flex_item_cache;
    const std::vector<Node*>& flex_items(Node* parent);

    void resolve_edges(Node* n, float cb_w);
    void apply_auto_margins(Node* n, float cb_w);
    void layout_node(Node* n, float cb_w, float cb_h, bool shrink, float forced_w = -1, float forced_h = -1);
    float layout_block_content(Node* n, float content_w, float hspec, float child_cb_h);
    float layout_flex(Node* n, float content_w, float hspec);
    float layout_grid(Node* n, float content_w, float hspec);
    float layout_inline_group(Node* parent, const std::vector<Node*>& group, float content_w, float start_y);

    void build_tokens(Node* group_node, std::vector<Token>& out, float content_w, Node* box_parent);
    void append_text_tokens(Node* text_node, std::vector<Token>& out);
    void image_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out = nullptr);
    void svg_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out = nullptr);
    void layout_image(Node* n, float cb_w, float forced_w, float forced_h);
    void layout_svg(Node* n, float cb_w, float forced_w, float forced_h);
    float intrinsic_outer_width(Node* n, float basis, int depth);
    float intrinsic_content_width(Node* n, float basis, int depth);
    void place_absolute(Node* parent, Node* child, float content_w, float child_cb_h, float cursor,
                        float parent_height);
    float pad_h(Node* n, float basis);
    float pad_v(Node* n, float basis);
    float border_h(Node* n);
    float border_v(Node* n);
    float margin_h(Node* n, float basis);
    float flat_relative_x(Node* n, float basis);
    float flat_relative_y(Node* n, float basis);
};

void LayoutEngine::resolve_edges(Node* n, float cb_w) {
    auto& st = n->style;
    auto L = [&](const Length& l) { return resolve_length(l, cb_w, st.font_size, 16.0f, true); };
    n->layout.margin_left = L(st.margin.left);
    n->layout.margin_right = L(st.margin.right);
    n->layout.margin_top = L(st.margin.top);
    n->layout.margin_bottom = L(st.margin.bottom);
    n->layout.padding_left = L(st.padding.left);
    n->layout.padding_right = L(st.padding.right);
    n->layout.padding_top = L(st.padding.top);
    n->layout.padding_bottom = L(st.padding.bottom);
    n->layout.border_left = border_left(st);
    n->layout.border_right = border_right(st);
    n->layout.border_top = border_top(st);
    n->layout.border_bottom = border_bottom(st);
}

void LayoutEngine::apply_auto_margins(Node* n, float cb_w) {
    auto& st = n->style;
    if (st.position == Position::Absolute || st.position == Position::Fixed) return;
    if (st.display == Display::Inline || st.display == Display::InlineBlock) return;
    bool la = st.margin.left.is_auto();
    bool ra = st.margin.right.is_auto();
    if (!la && !ra) return;
    float ml = n->layout.margin_left, mr = n->layout.margin_right;
    float remaining = cb_w - n->layout.border.w - (la ? 0 : ml) - (ra ? 0 : mr);
    if (remaining < 0) remaining = 0;
    if (la && ra) {
        ml = remaining / 2;
        mr = remaining - ml;
    } else if (la) ml = remaining;
    else mr = remaining;
    n->layout.margin_left = ml;
    n->layout.margin_right = mr;
}

float LayoutEngine::pad_h(Node* n, float basis) {
    return resolve_l(n->style.padding.left, basis, n->style.font_size) +
           resolve_l(n->style.padding.right, basis, n->style.font_size) +
           border_h(n);
}
float LayoutEngine::pad_v(Node* n, float basis) {
    return resolve_l(n->style.padding.top, basis, n->style.font_size) +
           resolve_l(n->style.padding.bottom, basis, n->style.font_size) +
           border_v(n);
}
float LayoutEngine::border_h(Node* n) { return border_left(n->style) + border_right(n->style); }
float LayoutEngine::border_v(Node* n) { return border_top(n->style) + border_bottom(n->style); }
float LayoutEngine::margin_h(Node* n, float basis) {
    return resolve_l(n->style.margin.left, basis, n->style.font_size) +
           resolve_l(n->style.margin.right, basis, n->style.font_size);
}

float LayoutEngine::flat_relative_x(Node* n, float basis) {
    float v = 0;
    if (!n->style.left.is_auto()) v += resolve_l(n->style.left, basis, n->style.font_size);
    else if (!n->style.right.is_auto()) v -= resolve_l(n->style.right, basis, n->style.font_size);
    return v;
}
float LayoutEngine::flat_relative_y(Node* n, float basis) {
    float v = 0;
    if (!n->style.top.is_auto()) v += resolve_l(n->style.top, basis, n->style.font_size);
    else if (!n->style.bottom.is_auto()) v -= resolve_l(n->style.bottom, basis, n->style.font_size);
    return v;
}

void LayoutEngine::image_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out) {
    std::string src = n->attr("src");
    std::string url = doc.resolve(src);
    auto img = images.get(url);
    if (img_out) *img_out = img;
    float iw = (img && img->ok) ? (float)(img->intrinsic_w > 0 ? img->intrinsic_w : img->w) : 0;
    float ih = (img && img->ok) ? (float)(img->intrinsic_h > 0 ? img->intrinsic_h : img->h) : 0;
    auto attr_dim = [&](const char* name) -> float {
        if (!n->has_attr(name)) return -1;
        std::string s = util::to_lower(util::trim(n->attr(name)));
        if (s.empty() || s == "auto") return -1;
        if (util::ends_with(s, "%")) return cb_w * (float)std::atof(s.c_str()) / 100.0f;
        return (float)std::atof(s.c_str());
    };
    float sw = -1, sh = -1;
    if (!n->style.width.is_auto()) sw = resolve_l(n->style.width, cb_w, n->style.font_size);
    else {
        float a = attr_dim("width");
        if (a > 0) sw = a;
    }
    if (!n->style.height.is_auto()) sh = resolve_l(n->style.height, cb_w, n->style.font_size);
    else {
        float a = attr_dim("height");
        if (a > 0) sh = a;
    }
    float ww = sw, hh = sh;
    if (ww <= 0 && hh <= 0) {
        if (iw > 0 && ih > 0) { ww = iw; hh = ih; }
        else { ww = 150; hh = 100; }
    } else if (ww <= 0) {
        ww = (ih > 0) ? iw * hh / ih : hh;
    } else if (hh <= 0) {
        hh = (iw > 0) ? ih * ww / iw : ww * 0.66f;
    }
    w = std::max(1.0f, ww);
    h = std::max(1.0f, hh);
}

void LayoutEngine::svg_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out) {
    std::string svg = n->outer_html();
    float iw = 0, ih = 0;
    svg_intrinsic_size(svg, &iw, &ih);
    float sw = -1, sh = -1;
    if (!n->style.width.is_auto()) sw = resolve_l(n->style.width, cb_w, n->style.font_size);
    if (!n->style.height.is_auto()) sh = resolve_l(n->style.height, cb_w, n->style.font_size);
    float ww = sw, hh = sh;
    if (ww <= 0 && hh <= 0) { ww = iw; hh = ih; }
    else if (ww <= 0) ww = ih > 0 ? iw * hh / ih : hh;
    else if (hh <= 0) hh = iw > 0 ? ih * ww / iw : ww;
    w = std::max(1.0f, ww);
    h = std::max(1.0f, hh);
    if (img_out) *img_out = render_svg(svg, w, h, n->style.color);
}

void LayoutEngine::layout_svg(Node* n, float cb_w, float forced_w, float forced_h) {
    resolve_edges(n, cb_w);
    float cw = 0, ch = 0;
    svg_content_size(n, cb_w, cw, ch, nullptr);
    if (forced_w >= 0) {
        float oldw = cw;
        cw = forced_w;
        if (forced_h < 0 && oldw > 0) ch = ch * cw / oldw;
    }
    if (forced_h >= 0) {
        float oldh = ch;
        ch = forced_h;
        if (forced_w < 0 && oldh > 0) cw = cw * ch / oldh;
    }
    cw = std::max(1.0f, cw);
    ch = std::max(1.0f, ch);
    std::shared_ptr<Image> img = render_svg(n->outer_html(), cw, ch, n->style.color);
    n->layout.content_w = cw;
    n->layout.content_h = ch;
    n->layout.content_x = n->layout.padding_left + n->layout.border_left;
    n->layout.content_y = n->layout.padding_top + n->layout.border_top;
    n->layout.border.w = cw + n->layout.padding_left + n->layout.padding_right + n->layout.border_left +
                         n->layout.border_right;
    n->layout.border.h = ch + n->layout.padding_top + n->layout.padding_bottom + n->layout.border_top +
                         n->layout.border_bottom;
    n->layout.is_image = true;
    n->layout.image_w = cw;
    n->layout.image_h = ch;
    n->layout.image = img.get();
    n->layout.has_box = true;
    apply_auto_margins(n, cb_w);
}

void LayoutEngine::layout_image(Node* n, float cb_w, float forced_w, float forced_h) {
    resolve_edges(n, cb_w);
    float cw = 0, ch = 0;
    std::shared_ptr<Image> img;
    image_content_size(n, cb_w, cw, ch, &img);
    if (forced_w >= 0) {
        float oldw = cw;
        cw = forced_w;
        if (forced_h < 0 && oldw > 0) ch = ch * cw / oldw;
    }
    if (forced_h >= 0) {
        float oldh = ch;
        ch = forced_h;
        if (forced_w < 0 && oldh > 0) cw = cw * ch / oldh;
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    n->layout.content_w = cw;
    n->layout.content_h = ch;
    n->layout.content_x = n->layout.padding_left + n->layout.border_left;
    n->layout.content_y = n->layout.padding_top + n->layout.border_top;
    n->layout.border.w = cw + n->layout.padding_left + n->layout.padding_right + n->layout.border_left +
                         n->layout.border_right;
    n->layout.border.h = ch + n->layout.padding_top + n->layout.padding_bottom + n->layout.border_top +
                         n->layout.border_bottom;
    n->layout.is_image = true;
    n->layout.image_w = cw;
    n->layout.image_h = ch;
    n->layout.image = img.get();
    n->layout.has_box = true;
    apply_auto_margins(n, cb_w);
}

void LayoutEngine::layout_node(Node* n, float cb_w, float cb_h, bool shrink, float forced_w, float forced_h) {
    if (!n || !n->is_element()) return;
    n->layout = LayoutInfo{};
    if (n->style.display == Display::None) {
        n->layout.none = true;
        return;
    }
    if (n->is_element("br")) return;
    if (n->is_element("svg")) {
        layout_svg(n, cb_w, forced_w, forced_h);
        return;
    }
    if (n->is_element("img")) {
        layout_image(n, cb_w, forced_w, forced_h);
        return;
    }
    resolve_edges(n, cb_w);
    auto& st = n->style;
    float ph = n->layout.padding_left + n->layout.padding_right + n->layout.border_left + n->layout.border_right;
    float pv = n->layout.padding_top + n->layout.padding_bottom + n->layout.border_top + n->layout.border_bottom;
    float avail = std::max(0.0f, cb_w - n->layout.margin_left - n->layout.margin_right - ph);
    float cw = 0;
    if (forced_w >= 0) {
        cw = forced_w;
    } else if (!st.width.is_auto()) {
        float spec = resolve_length(st.width, cb_w, st.font_size, 16.0f, false);
        cw = st.box_sizing_border ? spec - ph : spec;
        cw = std::max(0.0f, cw);
    } else if (shrink || st.display == Display::InlineBlock) {
        float intrinsic = intrinsic_content_width(n, avail, 0);
        cw = std::min(avail, std::max(0.0f, intrinsic));
    } else {
        cw = avail;
    }
    if (!st.min_width.is_auto()) cw = std::max(cw, resolve_length(st.min_width, cb_w, st.font_size, 16, false));
    if (!st.max_width.is_auto()) cw = std::min(cw, resolve_length(st.max_width, cb_w, st.font_size, 16, false));
    cw = std::max(0.0f, cw);

    float hspec = -1;
    if (forced_h >= 0) {
        hspec = forced_h;
    } else if (st.aspect_ratio > 0 && st.height.is_auto() && cw > 0) {
        hspec = cw / st.aspect_ratio;
    } else if (!st.height.is_auto()) {
        float spec = resolve_length(st.height, cb_h > 0 ? cb_h : 0, st.font_size, 16, false);
        hspec = st.box_sizing_border ? spec - pv : spec;
        hspec = std::max(0.0f, hspec);
    }

    float used = 0;
    float child_cb_h = hspec >= 0 ? hspec : (cb_h > 0 ? cb_h : 0);
    if (st.display == Display::Flex) used = layout_flex(n, cw, hspec);
    else if (st.display == Display::Grid) used = layout_grid(n, cw, hspec);
    else used = layout_block_content(n, cw, hspec, child_cb_h);
    float ch = hspec >= 0 ? hspec : used;
    if (!st.min_height.is_auto()) ch = std::max(ch, resolve_length(st.min_height, child_cb_h, st.font_size, 16, false));
    if (!st.max_height.is_auto()) ch = std::min(ch, resolve_length(st.max_height, child_cb_h, st.font_size, 16, false));
    ch = std::max(0.0f, ch);

    n->layout.content_w = cw;
    n->layout.content_h = ch;
    n->layout.content_x = n->layout.padding_left + n->layout.border_left;
    n->layout.content_y = n->layout.padding_top + n->layout.border_top;
    n->layout.border.w = cw + ph;
    n->layout.border.h = ch + pv;
    n->layout.has_box = true;
    apply_auto_margins(n, cb_w);
}

void LayoutEngine::place_absolute(Node* parent, Node* child, float content_w, float child_cb_h, float cursor,
                                  float parent_height) {
    layout_node(child, content_w, child_cb_h, true);
    float x = 0, y = cursor;
    if (!child->style.left.is_auto()) x = resolve_l(child->style.left, content_w, child->style.font_size);
    else if (!child->style.right.is_auto())
        x = content_w - child->layout.border.w - resolve_l(child->style.right, content_w, child->style.font_size);
    if (!child->style.top.is_auto()) y = resolve_l(child->style.top, child_cb_h, child->style.font_size);
    else if (!child->style.bottom.is_auto()) {
        float base = parent_height > 0 ? parent_height : child_cb_h;
        y = base - child->layout.border.h - resolve_l(child->style.bottom, child_cb_h, child->style.font_size);
    }
    child->layout.border.x = x;
    child->layout.border.y = y;
    parent->layout.box_children.push_back(child);
}

float LayoutEngine::layout_block_content(Node* n, float content_w, float hspec, float child_cb_h) {
    float cursor = 0;
    std::vector<Node*> group;
    auto flush = [&]() {
        if (group.empty()) return;
        float used = layout_inline_group(n, group, content_w, cursor);
        cursor += used;
        group.clear();
    };
    for (auto* child : n->children) {
        if (!child) continue;
        if (child->is_text() || child->is_comment()) {
            if (child->is_text()) group.push_back(child);
            continue;
        }
        if (!child->is_element()) continue;
        if (child->style.display == Display::None) continue;
        if (child->is_element("br")) { group.push_back(child); continue; }
        if (child->style.position == Position::Absolute || child->style.position == Position::Fixed) {
            flush();
            place_absolute(n, child, content_w, child_cb_h, cursor, hspec >= 0 ? hspec : cursor);
            continue;
        }
        if (child->style.display == Display::Inline || child->style.display == Display::InlineBlock) {
            group.push_back(child);
            continue;
        }
        flush();
        layout_node(child, content_w, child_cb_h, false);
        float x = child->layout.margin_left;
        float y = cursor + child->layout.margin_top;
        if (child->style.position == Position::Relative) {
            x += flat_relative_x(child, content_w);
            y += flat_relative_y(child, child_cb_h);
        }
        child->layout.border.x = x;
        child->layout.border.y = y;
        n->layout.box_children.push_back(child);
        cursor = y - flat_relative_y(child, child_cb_h) + child->layout.border.h + child->layout.margin_bottom;
    }
    flush();
    return cursor;
}

void LayoutEngine::append_text_tokens(Node* tn, std::vector<Token>& out) {
    const ComputedStyle& st = tn->style;
    std::string text = transform_text(tn->data, st.text_transform);
    if (text.empty()) return;
    bool pre = st.white_space == WhiteSpace::Pre || st.white_space == WhiteSpace::PreWrap;
    bool wrap = st.white_space != WhiteSpace::Nowrap && st.white_space != WhiteSpace::Pre;
    Node* link = tn->link_ancestor();
    auto make_word = [&](const std::string& w) {
        if (w.empty()) return;
        Token t;
        t.kind = Token::Word;
        t.node = tn;
        t.link = link;
        t.style = &st;
        t.text = w;
        t.w = fonts.text_width(w, st);
        t.ascent = fonts.ascent(st);
        t.descent = fonts.descent(st);
        t.h = t.ascent + t.descent;
        t.wrap = wrap;
        out.push_back(std::move(t));
    };
    auto make_space = [&](bool preserve) {
        Token t;
        t.kind = Token::Space;
        t.node = tn;
        t.link = link;
        t.style = &st;
        t.text = " ";
        t.w = fonts.text_width(" ", st);
        t.ascent = fonts.ascent(st);
        t.descent = fonts.descent(st);
        t.h = t.ascent + t.descent;
        t.wrap = wrap;
        t.preserve_space = preserve;
        out.push_back(std::move(t));
    };
    auto make_break = [&]() {
        Token t;
        t.kind = Token::Break;
        t.node = tn;
        t.style = &st;
        t.link = link;
        out.push_back(std::move(t));
    };
    std::string word;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        size_t ni = util::utf8_next(text, i, &cp);
        std::string ch = text.substr(i, ni - i);
        i = ni;
        if (cp == '\n') {
            make_word(word);
            word.clear();
            if (pre) make_break();
            else make_space(false);
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f') {
            make_word(word);
            word.clear();
            if (pre) make_space(true);
            else make_space(false);
            continue;
        }
        if (util::is_cjk(cp)) {
            make_word(word);
            word.clear();
            make_word(ch);
            continue;
        }
        word += ch;
    }
    make_word(word);
}

void LayoutEngine::build_tokens(Node* g, std::vector<Token>& out, float content_w, Node* box_parent) {
    if (!g) return;
    if (g->is_text()) {
        append_text_tokens(g, out);
        return;
    }
    if (!g->is_element()) return;
    if (g->style.display == Display::None) return;
    if (g->is_element("br")) {
        Token t;
        t.kind = Token::Break;
        t.node = g;
        t.style = &g->style;
        out.push_back(std::move(t));
        return;
    }
    if (g->is_element("img") || g->is_element("svg")) {
        float w = 0, h = 0;
        std::shared_ptr<Image> img;
        if (g->is_element("svg")) svg_content_size(g, content_w, w, h, &img);
        else image_content_size(g, content_w, w, h, &img);
        Token t;
        t.kind = Token::ImageTok;
        t.node = g;
        t.link = g->link_ancestor();
        t.style = &g->style;
        t.image = img;
        t.w = w;
        t.h = h;
        t.ascent = h;
        t.descent = 0;
        t.wrap = true;
        out.push_back(std::move(t));
        return;
    }
    if (g->style.display == Display::InlineBlock || g->style.display == Display::Block ||
        g->style.display == Display::Flex || g->style.display == Display::Grid) {
        layout_node(g, content_w, 0, true);
        Token t;
        t.kind = Token::BoxTok;
        t.node = g;
        t.link = g->link_ancestor();
        t.style = &g->style;
        t.box = g;
        t.w = g->layout.border.w;
        t.h = g->layout.border.h;
        t.ascent = t.h;
        t.descent = 0;
        t.wrap = true;
        if (box_parent && std::find(box_parent->layout.box_children.begin(), box_parent->layout.box_children.end(), g) ==
                               box_parent->layout.box_children.end())
            box_parent->layout.box_children.push_back(g);
        out.push_back(std::move(t));
        return;
    }
    for (auto* c : g->children) build_tokens(c, out, content_w, box_parent);
}

float LayoutEngine::layout_inline_group(Node* parent, const std::vector<Node*>& group, float content_w,
                                        float start_y) {
    std::vector<Token> tokens;
    for (auto* g : group) build_tokens(g, tokens, content_w, parent);
    if (tokens.empty()) return 0;

    std::vector<std::pair<Token, float>> line;
    float line_w = 0;
    float line_y = 0;
    bool pending = false;
    Token pending_tok;
    bool wrap_allowed = parent->style.white_space != WhiteSpace::Nowrap && parent->style.white_space != WhiteSpace::Pre;

    auto finish_line = [&](bool force) {
        if (line.empty() && !force) return;
        float ascent = fonts.ascent(parent->style);
        float descent = fonts.descent(parent->style);
        float lh = style_line_height(parent->style);
        for (auto& p : line) {
            const Token& t = p.first;
            ascent = std::max(ascent, t.ascent);
            descent = std::max(descent, t.descent);
            if (t.style) lh = std::max(lh, style_line_height(*t.style));
            lh = std::max(lh, t.ascent + t.descent);
        }
        if (ascent + descent > lh) lh = ascent + descent;
        float leading = std::max(0.0f, lh - (ascent + descent));
        float baseline = leading / 2 + ascent; // relative to the top of this line
        float leftover = content_w - line_w;
        float offset = 0;
        if (parent->style.text_align == TextAlign::Center) offset = leftover / 2;
        else if (parent->style.text_align == TextAlign::Right) offset = leftover;
        LineBox lb;
        lb.y = start_y + line_y;
        lb.height = lh;
        lb.baseline = baseline;
        lb.width = line_w;
        for (auto& p : line) {
            const Token& t = p.first;
            float x = offset + p.second;
            LineItem item;
            item.kind = t.kind == Token::ImageTok ? LineItem::ImageItem
                                                  : t.kind == Token::BoxTok ? LineItem::BoxItem : LineItem::Text;
            item.node = t.node;
            item.link = t.link;
            item.style = t.style;
            item.text = t.text;
            item.image = t.image.get();
            item.box = t.box;
            item.x = x;
            item.y = lb.y + (baseline - t.ascent);
            item.w = t.w;
            item.h = t.h;
            item.baseline = t.ascent;
            item.font_size = t.style ? t.style->font_size : parent->style.font_size;
            item.color = t.style ? t.style->color : parent->style.color;
            item.underline = t.style ? t.style->underline : false;
            item.line_through = t.style ? t.style->line_through : false;
            lb.items.push_back(std::move(item));
            if (t.kind == Token::BoxTok && t.box) {
                t.box->layout.border.x = x;
                t.box->layout.border.y = lb.y + (baseline - t.h);
            }
        }
        parent->layout.lines.push_back(std::move(lb));
        parent->layout.has_lines = true;
        line_y += lh;
        line.clear();
        line_w = 0;
    };

    auto add_pending_space = [&]() {
        if (!pending) return;
        line.push_back({pending_tok, line_w});
        line_w += pending_tok.w;
        pending = false;
    };

    for (auto& t : tokens) {
        if (t.kind == Token::Break) {
            pending = false;
            finish_line(true);
            continue;
        }
        if (t.kind == Token::Space) {
            if (t.preserve_space) {
                line.push_back({t, line_w});
                line_w += t.w;
            } else {
                if (!line.empty()) { pending = true; pending_tok = t; }
            }
            continue;
        }
        float extra = pending ? pending_tok.w : 0;
        if (wrap_allowed && t.wrap && !line.empty() && line_w + extra + t.w > content_w + 0.5f) {
            pending = false;
            finish_line(false);
            extra = 0;
        }
        if (pending && !line.empty()) add_pending_space();
        else if (line.empty()) pending = false;
        line.push_back({t, line_w});
        line_w += t.w;
    }
    finish_line(false);
    return line_y;
}

const std::vector<Node*>& LayoutEngine::flex_items(Node* parent) {
    auto found = flex_item_cache.find(parent);
    if (found != flex_item_cache.end()) return found->second;
    auto& items = flex_item_cache[parent];
    std::vector<Node*> text;
    auto flush = [&]() {
        bool visible_text = false;
        for (auto* child : text)
            if (child->data.find_first_not_of(" \t\n\r\f") != std::string::npos) visible_text = true;
        if (visible_text) {
            doc.layout_nodes.emplace_back();
            auto* box = &doc.layout_nodes.back();
            box->anonymous = true;
            box->parent = parent;
            box->style = inherited_style(parent->style);
            box->style.display = Display::Block;
            doc.layout_nodes.emplace_back();
            auto* run = &doc.layout_nodes.back();
            run->type = NodeType::Text;
            run->anonymous = true;
            // One text run for both measurement and tokenization. DOM node
            // boundaries are not word breaks; clicks/links still target the
            // original flex container rather than a synthetic DOM element.
            run->parent = parent;
            run->style = parent->style;
            for (auto* child : text) run->data += child->data;
            box->children.push_back(run);
            items.push_back(box);
        }
        text.clear();
    };
    for (auto* child : parent->children) {
        if (!child) continue;
        if (child->is_text()) text.push_back(child);
        else if (child->is_element()) {
            flush();
            if (child->style.display != Display::None) items.push_back(child);
        }
    }
    flush();
    return items;
}

float LayoutEngine::layout_flex(Node* n, float content_w, float hspec) {
    auto& st = n->style;
    std::vector<Node*> items;
    std::vector<Node*> absolutes;
    for (auto* c : flex_items(n)) {
        if (c->style.position == Position::Absolute || c->style.position == Position::Fixed) {
            absolutes.push_back(c);
            continue;
        }
        items.push_back(c);
    }
    std::stable_sort(items.begin(), items.end(), [](Node* a, Node* b) { return a->style.order < b->style.order; });

    bool column = st.flex_direction == FlexDirection::Column || st.flex_direction == FlexDirection::ColumnReverse;
    if (column) {
        float total = 0;
        std::vector<float> outer_heights(items.size(), 0);
        for (size_t i = 0; i < items.size(); i++) {
            auto* it = items[i];
            float ml = resolve_l(it->style.margin.left, content_w, it->style.font_size);
            float mr = resolve_l(it->style.margin.right, content_w, it->style.font_size);
            float mt = resolve_l(it->style.margin.top, content_w, it->style.font_size);
            float mb = resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
            bool stretch = st.align_items == AlignItems::Stretch && it->style.width.is_auto();
            float forced = -1;
            if (stretch) forced = std::max(0.0f, content_w - ml - mr - pad_h(it, content_w));
            layout_node(it, content_w, hspec > 0 ? hspec : 0, !stretch, forced, -1);
            outer_heights[i] = it->layout.border.h + mt + mb;
            total += outer_heights[i];
            if (i + 1 < items.size()) total += st.row_gap;
        }
        float container_h = hspec >= 0 ? hspec : total;
        float free = container_h - total;
        if (free > 0) {
            float grow_sum = 0;
            for (auto* it : items) grow_sum += std::max(0.0f, it->style.flex_grow);
            if (grow_sum > 0) {
                for (size_t i = 0; i < items.size(); i++) {
                    float add = free * std::max(0.0f, items[i]->style.flex_grow) / grow_sum;
                    items[i]->layout.content_h += add;
                    items[i]->layout.border.h += add;
                    outer_heights[i] += add;
                }
            }
        } else if (free < 0) {
            float shrink_sum = 0;
            for (auto* it : items)
                shrink_sum += std::max(0.0f, it->style.flex_shrink) * std::max(1.0f, it->layout.content_h);
            if (shrink_sum > 0) {
                for (size_t i = 0; i < items.size(); i++) {
                    float sh = std::max(0.0f, items[i]->style.flex_shrink) * std::max(1.0f, items[i]->layout.content_h);
                    float cut = (-free) * sh / shrink_sum;
                    float nh = std::max(0.0f, items[i]->layout.content_h - cut);
                    items[i]->layout.border.h -= (items[i]->layout.content_h - nh);
                    items[i]->layout.content_h = nh;
                }
            }
        }
        float used_main = 0;
        for (size_t i = 0; i < items.size(); i++) used_main += items[i]->layout.border.h +
            resolve_l(items[i]->style.margin.top, content_w, items[i]->style.font_size) +
            resolve_l(items[i]->style.margin.bottom, content_w, items[i]->style.font_size);
        used_main += (items.size() > 1 ? (items.size() - 1) * st.row_gap : 0);
        float extra = std::max(0.0f, container_h - used_main);
        float start = 0, between = 0;
        size_t count = items.size();
        if (st.justify == Justify::FlexEnd) start = extra;
        else if (st.justify == Justify::Center) start = extra / 2;
        else if (st.justify == Justify::SpaceBetween && count > 1) between = extra / (count - 1);
        else if (st.justify == Justify::SpaceAround && count > 0) { between = extra / count; start = between / 2; }
        else if (st.justify == Justify::SpaceEvenly && count > 0) { between = extra / (count + 1); start = between; }
        float y = start;
        for (size_t i = 0; i < items.size(); i++) {
            auto* it = items[i];
            float ml = resolve_l(it->style.margin.left, content_w, it->style.font_size);
            float mr = resolve_l(it->style.margin.right, content_w, it->style.font_size);
            float mt = resolve_l(it->style.margin.top, content_w, it->style.font_size);
            float mb = resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
            float outer_w = it->layout.border.w + ml + mr;
            float x = ml;
            if (st.align_items == AlignItems::Center) x = (content_w - outer_w) / 2 + ml;
            else if (st.align_items == AlignItems::FlexEnd) x = content_w - outer_w + ml;
            it->layout.border.x = x;
            it->layout.border.y = y + mt;
            n->layout.box_children.push_back(it);
            y += it->layout.border.h + mt + mb + st.row_gap + between;
        }
        for (auto* a : absolutes) place_absolute(n, a, content_w, container_h, 0, container_h);
        return container_h;
    }

    // Row layout, optionally wrapped.
    float gap = st.column_gap;
    auto base_content = [&](Node* it) -> float {
        auto& s = it->style;
        if (!s.flex_basis.is_auto()) return std::max(0.0f, resolve_l(s.flex_basis, content_w, s.font_size));
        if (!s.width.is_auto()) {
            float w = resolve_l(s.width, content_w, s.font_size);
            if (s.box_sizing_border) w -= pad_h(it, content_w);
            return std::max(0.0f, w);
        }
        return std::max(0.0f, std::min(content_w, intrinsic_content_width(it, content_w, 0)));
    };
    auto outer_of = [&](Node* it, float content) {
        return content + pad_h(it, content_w) + margin_h(it, content_w);
    };
    std::vector<float> base(items.size(), 0);
    std::vector<float> base_outer(items.size(), 0);
    for (size_t i = 0; i < items.size(); i++) {
        base[i] = base_content(items[i]);
        base_outer[i] = outer_of(items[i], base[i]);
    }

    bool wrap = st.flex_wrap != FlexWrap::NoWrap;
    std::vector<std::vector<size_t>> lines;
    if (wrap) {
        std::vector<size_t> line;
        float w = 0;
        for (size_t i = 0; i < items.size(); i++) {
            float add = base_outer[i] + (line.empty() ? 0 : gap);
            if (!line.empty() && w + add > content_w) {
                lines.push_back(line);
                line.clear();
                w = 0;
                add = base_outer[i];
            }
            line.push_back(i);
            w += add;
        }
        if (!line.empty()) lines.push_back(line);
    } else {
        std::vector<size_t> line;
        for (size_t i = 0; i < items.size(); i++) line.push_back(i);
        lines.push_back(line);
    }

    float line_top = 0;
    for (auto& line : lines) {
        float sum_outer = 0, sum_grow = 0, sum_shrink = 0;
        for (size_t i : line) {
            sum_outer += base_outer[i];
            sum_grow += std::max(0.0f, items[i]->style.flex_grow);
            sum_shrink += std::max(0.0f, items[i]->style.flex_shrink) * std::max(1.0f, base[i]);
        }
        sum_outer += (line.size() > 1 ? (line.size() - 1) * gap : 0);
        float free = content_w - sum_outer;
        std::vector<float> final_content(line.size(), 0);
        for (size_t k = 0; k < line.size(); k++) {
            size_t i = line[k];
            float c = base[i];
            if (free > 0 && sum_grow > 0) c += free * std::max(0.0f, items[i]->style.flex_grow) / sum_grow;
            else if (free < 0 && sum_shrink > 0) {
                float sh = std::max(0.0f, items[i]->style.flex_shrink) * std::max(1.0f, base[i]);
                c = std::max(0.0f, c + free * sh / sum_shrink);
            }
            final_content[k] = c;
        }
        std::vector<float> outer_h(line.size(), 0);
        for (size_t k = 0; k < line.size(); k++) {
            size_t i = line[k];
            Node* it = items[i];
            layout_node(it, content_w, hspec > 0 ? hspec : 0, true, final_content[k], -1);
            outer_h[k] = it->layout.border.h + resolve_l(it->style.margin.top, content_w, it->style.font_size) +
                         resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
        }
        float line_cross = 0;
        for (float h : outer_h) line_cross = std::max(line_cross, h);
        float cross_size = line_cross;
        if (hspec >= 0 && lines.size() == 1) cross_size = std::max(cross_size, hspec);
        for (size_t k = 0; k < line.size(); k++) {
            Node* it = items[line[k]];
            if (st.align_items == AlignItems::Stretch && it->style.height.is_auto() &&
                it->style.display != Display::Inline) {
                float mt = resolve_l(it->style.margin.top, content_w, it->style.font_size);
                float mb = resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
                float target = cross_size - mt - mb - pad_v(it, content_w);
                if (target > it->layout.content_h) {
                    it->layout.content_h = target;
                    it->layout.border.h = target + pad_v(it, content_w);
                    outer_h[k] = it->layout.border.h + mt + mb;
                }
            }
        }
        float used_w = 0;
        for (size_t k = 0; k < line.size(); k++) {
            Node* it = items[line[k]];
            used_w += it->layout.border.w + margin_h(it, content_w);
            if (k + 1 < line.size()) used_w += gap;
        }
        float extra = std::max(0.0f, content_w - used_w);
        float start = 0, between = 0;
        size_t count = line.size();
        if (st.justify == Justify::FlexEnd) start = extra;
        else if (st.justify == Justify::Center) start = extra / 2;
        else if (st.justify == Justify::SpaceBetween && count > 1) between = extra / (count - 1);
        else if (st.justify == Justify::SpaceAround && count > 0) { between = extra / count; start = between / 2; }
        else if (st.justify == Justify::SpaceEvenly && count > 0) { between = extra / (count + 1); start = between; }
        float x = start;
        for (size_t k = 0; k < line.size(); k++) {
            Node* it = items[line[k]];
            float ml = resolve_l(it->style.margin.left, content_w, it->style.font_size);
            float mt = resolve_l(it->style.margin.top, content_w, it->style.font_size);
            float mb = resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
            float outer = it->layout.border.h + mt + mb;
            float yoff = 0;
            if (st.align_items == AlignItems::Center) yoff = (cross_size - outer) / 2;
            else if (st.align_items == AlignItems::FlexEnd) yoff = cross_size - outer;
            it->layout.border.x = x + ml;
            it->layout.border.y = line_top + mt + yoff;
            n->layout.box_children.push_back(it);
            x += it->layout.border.w + ml + resolve_l(it->style.margin.right, content_w, it->style.font_size) + gap +
                 between;
        }
        line_top += cross_size;
        if (&line != &lines.back()) line_top += st.row_gap;
    }
    float used_h = hspec >= 0 ? hspec : line_top;
    for (auto* a : absolutes) place_absolute(n, a, content_w, used_h, 0, used_h);
    return used_h;
}

float LayoutEngine::layout_grid(Node* n, float content_w, float hspec) {
    const auto& st = n->style;
    std::vector<Node*> items, absolutes;
    for (auto* c : n->children) {
        if (!c || !c->is_element() || c->style.display == Display::None) continue;
        if (c->style.position == Position::Absolute || c->style.position == Position::Fixed) {
            absolutes.push_back(c);
            continue;
        }
        items.push_back(c);
    }
    std::vector<float> cols;
    float col_gap = st.column_gap;
    if (st.grid_columns.empty()) {
        cols.push_back(content_w);
    } else if (st.grid_columns.size() == 1 && st.grid_columns[0].kind == TrackKind::AutoFit) {
        float minw = st.grid_columns[0].value;
        if (minw < 40) minw = 240;
        int count = std::max(1, (int)std::floor((content_w + col_gap) / (minw + col_gap)));
        if (count > 32) count = 32;
        float each = (content_w - (count - 1) * col_gap) / count;
        if (each < 0) each = 0;
        cols.assign(count, each);
    } else {
        float fixed = 0, flex = 0;
        int autos = 0;
        for (auto& t : st.grid_columns) {
            if (t.kind == TrackKind::Px) fixed += t.value;
            else if (t.kind == TrackKind::Percent) fixed += content_w * t.value / 100.0f;
            else if (t.kind == TrackKind::Fr) flex += std::max(0.0f, t.value);
            else autos++;
        }
        float gaps = std::max(0, (int)st.grid_columns.size() - 1) * col_gap;
        float free = std::max(0.0f, content_w - fixed - gaps);
        float unit = (flex + autos > 0) ? free / (flex + autos) : 0;
        for (auto& t : st.grid_columns) {
            if (t.kind == TrackKind::Px) cols.push_back(std::max(0.0f, t.value));
            else if (t.kind == TrackKind::Percent) cols.push_back(std::max(0.0f, content_w * t.value / 100.0f));
            else if (t.kind == TrackKind::Fr) cols.push_back(std::max(0.0f, unit * t.value));
            else cols.push_back(std::max(0.0f, unit));
        }
    }
    if (cols.empty()) cols.push_back(content_w);
    int cn = (int)cols.size();
    struct Placement {Node* node;int row,col,span;float width;};
    std::vector<Placement> placed;int row=0,col=0;
    for(auto* item:items){const int span=std::min(cn,std::max(1,item->style.grid_column_span));if(col+span>cn){++row;col=0;}float w=col_gap*(span-1);for(int c=col;c<col+span;++c)w+=cols[c];placed.push_back({item,row,col,span,w});col+=span;if(col==cn){++row;col=0;}}
    const int rows=placed.empty()?0:placed.back().row+1;std::vector<float> row_h(rows,0);
    for(auto& p:placed){auto* it=p.node;const float ml=resolve_l(it->style.margin.left,content_w,it->style.font_size),mr=resolve_l(it->style.margin.right,content_w,it->style.font_size),mt=resolve_l(it->style.margin.top,content_w,it->style.font_size),mb=resolve_l(it->style.margin.bottom,content_w,it->style.font_size);
        float fw=it->style.width.is_auto()?std::max(0.0f,p.width-ml-mr-pad_h(it,content_w)):-1;
        layout_node(it,p.width,hspec>0?hspec:0,true,fw,-1);row_h[p.row]=std::max(row_h[p.row],it->layout.border.h+mt+mb);
    }
    for(int r=0;r<rows&&r<(int)st.grid_rows.size();++r){const auto t=st.grid_rows[r];if(t.kind==TrackKind::Px)row_h[r]=std::max(row_h[r],t.value);else if(t.kind==TrackKind::Percent)row_h[r]=std::max(row_h[r],t.value*std::max(0.0f,hspec)/100);}
    float y=0;for(int r=0;r<rows;++r){for(auto& p:placed){if(p.row!=r)continue;auto* it=p.node;float x=0;for(int c=0;c<p.col;++c)x+=cols[c]+col_gap;
        const float mt=resolve_l(it->style.margin.top,content_w,it->style.font_size),mb=resolve_l(it->style.margin.bottom,content_w,it->style.font_size),ml=resolve_l(it->style.margin.left,content_w,it->style.font_size);
        if(it->style.height.is_auto()&&st.align_items==AlignItems::Stretch){const float target=row_h[r]-mt-mb-pad_v(it,content_w);if(target>it->layout.content_h){it->layout.content_h=target;it->layout.border.h=target+pad_v(it,content_w);}}
        it->layout.border.x=x+ml;it->layout.border.y=y+mt;n->layout.box_children.push_back(it);
    }y+=row_h[r]+st.row_gap;}
    float used = hspec >= 0 ? hspec : std::max(0.0f, y - (rows ? st.row_gap : 0));
    for (auto* a : absolutes) place_absolute(n, a, content_w, used, 0, used);
    return used;
}

float LayoutEngine::intrinsic_content_width(Node* n, float basis, int depth) {
    if (!n || !n->is_element() || depth > 30) return 0;
    if (n->style.display == Display::None) return 0;
    if (n->is_element("img")) {
        float w = 0, h = 0;
        image_content_size(n, basis, w, h, nullptr);
        return w;
    }
    if (n->is_element("svg")) {
        float w = 0, h = 0;
        svg_content_size(n, basis, w, h, nullptr);
        return w;
    }
    if (n->is_element("br")) return 0;
    if (n->style.display == Display::Grid) {
        int cols = (int)n->style.grid_columns.size();
        if (cols <= 0 || n->style.grid_columns[0].kind == TrackKind::AutoFit) cols = 1;
        float maxw = 0;
        int count = 0;
        for (auto* c : n->children) {
            if (!c || !c->is_element() || c->style.display == Display::None) continue;
            maxw = std::max(maxw, intrinsic_outer_width(c, basis, depth + 1));
            count++;
        }
        int visible = std::min(cols, std::max(1, count));
        return maxw * visible + (visible > 1 ? (visible - 1) * n->style.column_gap : 0);
    }
    if (n->style.display == Display::Flex) {
        bool column = n->style.flex_direction == FlexDirection::Column ||
                      n->style.flex_direction == FlexDirection::ColumnReverse;
        float total = 0, maxw = 0;
        int count = 0;
        for (auto* c : flex_items(n)) {
            if (c->style.position == Position::Absolute || c->style.position == Position::Fixed) continue;
            float w = intrinsic_outer_width(c, basis, depth + 1);
            if (column) maxw = std::max(maxw, w);
            else total += w + (count ? n->style.column_gap : 0);
            count++;
        }
        return column ? maxw : total;
    }
    float line = 0, maxw = 0;
    std::function<void(Node*)> walk = [&](Node* c) {
        if (!c || depth > 30) return;
        if (c->is_text()) {
            line += fonts.text_width(collapse_text(c->data, c->style), c->style);
            return;
        }
        if (!c->is_element() || c->style.display == Display::None) return;
        if (c->is_element("br")) { maxw = std::max(maxw, line); line = 0; return; }
        if (c->is_element("img")) {
            float w = 0, h = 0;
            image_content_size(c, basis, w, h, nullptr);
            line += w + margin_h(c, basis);
            return;
        }
        if (c->style.display == Display::InlineBlock) {
            line += intrinsic_outer_width(c, basis, depth + 1);
            return;
        }
        if (c->style.display == Display::Inline) {
            for (auto* cc : c->children) walk(cc);
            return;
        }
        maxw = std::max(maxw, line);
        line = 0;
        maxw = std::max(maxw, intrinsic_outer_width(c, basis, depth + 1));
    };
    for (auto* c : n->children) walk(c);
    maxw = std::max(maxw, line);
    return maxw;
}

float LayoutEngine::intrinsic_outer_width(Node* n, float basis, int depth) {
    if (!n || !n->is_element()) return 0;
    float content = intrinsic_content_width(n, basis, depth + 1);
    if (n->is_element("img")) return content + pad_h(n, basis) + margin_h(n, basis);
    if (n->style.display == Display::Inline) return content;
    return content + pad_h(n, basis) + margin_h(n, basis);
}

void LayoutEngine::run() {
    Node* root = doc.html;
    if (!root) root = doc.body;
    if (!root) {
        for (auto* c : doc.root->children) if (c && c->is_element()) { root = c; break; }
    }
    if (!root) return;
    layout_node(root, vw, vh, false, -1, -1);
    root->layout.border.x = 0;
    root->layout.border.y = 0;
}

void layout_document(Document& doc, LayoutContext& ctx) {
    doc.layout_nodes.clear();
    // Hidden subtrees are skipped below, so invalidate their previous boxes too.
    for(auto& node:doc.arena)node.layout=LayoutInfo{};
    LayoutEngine e(doc, ctx);
    e.run();
}

float document_content_height(const Document& doc) {
    const Node* root = doc.html ? doc.html : doc.body;
    if (!root || !root->layout.has_box) return 0;
    return root->layout.border.h;
}
