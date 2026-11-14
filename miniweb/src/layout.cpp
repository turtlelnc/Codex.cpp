#include "layout.h"
#include "canvas.h"
#include "diagnostics.h"
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

std::string node_label(const Node* node) {
    std::string label = node->tag.empty() ? "element" : node->tag;
    if (node->has_attr("id")) label += "#" + node->attr("id");
    return label;
}

float resolve_l(const Length& l, float basis, float fs) {
    return resolve_length(l, basis, fs, 16.0f, true);
}

bool percentage_dependent(const Length& length) {
    if (length.unit == Length::Percent) return true;
    return std::any_of(length.arguments.begin(), length.arguments.end(), percentage_dependent);
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
    void layout_node(Node* n, float cb_w, float cb_h, bool shrink, float forced_w = -1, float forced_h = -1,
                     bool clamp_width = true, float avail_w = -1);
    float layout_block_content(Node* n, float content_w, float hspec, float child_cb_h);
    float layout_flex(Node* n, float content_w, float hspec);
    float layout_grid(Node* n, float content_w, float hspec);
    struct TableRow { Node* node; Node* group; std::vector<Node*> cells; };
    std::vector<TableRow> table_rows(Node* table);
    std::vector<float> table_columns(const std::vector<TableRow>& rows, float basis, bool minimum, int depth);
    float minimum_width(Node* node, float basis, int depth);
    float layout_table(Node* n, float& content_w, float hspec);
    float layout_inline_group(Node* parent, const std::vector<Node*>& group, float content_w, float start_y);

    void build_tokens(Node* group_node, std::vector<Token>& out, float content_w, Node* box_parent);
    void append_text_tokens(Node* text_node, std::vector<Token>& out);
    void image_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out = nullptr);
    void svg_content_size(Node* n, float cb_w, float& w, float& h, std::shared_ptr<Image>* img_out = nullptr);
    void layout_image(Node* n, float cb_w, float forced_w, float forced_h);
    void layout_svg(Node* n, float cb_w, float forced_w, float forced_h);
    float intrinsic_outer_width(Node* n, float basis, int depth);
    float intrinsic_content_width(Node* n, float basis, int depth);
    void place_absolute(Node* parent, Node* child, float content_w, float cursor,
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
    const auto& top=n->style.top;const auto& bottom=n->style.bottom;
    if (!top.is_auto()&&(basis>=0||!percentage_dependent(top))) v += resolve_l(top, std::max(0.0f,basis), n->style.font_size);
    else if (!bottom.is_auto()&&(basis>=0||!percentage_dependent(bottom))) v -= resolve_l(bottom, std::max(0.0f,basis), n->style.font_size);
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

void LayoutEngine::layout_node(Node* n, float cb_w, float cb_h, bool shrink, float forced_w, float forced_h,
                               bool clamp_width, float avail_w) {
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
    float avail = std::max(0.0f, (avail_w >= 0 ? avail_w : cb_w) - n->layout.margin_left -
                                     n->layout.margin_right - ph);
    float cw = 0;
    if (forced_w >= 0) {
        cw = forced_w;
    } else if (!st.width.is_auto()) {
        float spec = resolve_length(st.width, cb_w, st.font_size, 16.0f, false);
        cw = st.box_sizing_border ? spec - ph : spec;
        cw = std::max(0.0f, cw);
    } else if (shrink || st.display == Display::InlineBlock || st.display == Display::Table) {
        float intrinsic = intrinsic_content_width(n, avail, 0);
        cw = std::min(avail, std::max(0.0f, intrinsic));
    } else {
        cw = avail;
    }
    if (clamp_width) {
        // Min/max constrain the same sizing box as width, not always content.
        auto width_bound = [&](const Length& value) {
            const float resolved = resolve_length(value, cb_w, st.font_size, 16, false);
            return std::max(0.0f, resolved - (st.box_sizing_border ? ph : 0.0f));
        };
        if (!st.max_width.is_auto()) cw = std::min(cw, width_bound(st.max_width));
        if (!st.min_width.is_auto()) cw = std::max(cw, width_bound(st.min_width));
    }
    cw = std::max(0.0f, cw);

    float hspec = -1;
    if (forced_h >= 0) {
        hspec = forced_h;
    } else if (st.aspect_ratio > 0 && st.height.is_auto() && cw > 0) {
        hspec = cw / st.aspect_ratio;
    } else if (!st.height.is_auto() && (cb_h >= 0 || !percentage_dependent(st.height))) {
        float spec = resolve_length(st.height, std::max(0.0f,cb_h), st.font_size, 16, false);
        hspec = st.box_sizing_border ? spec - pv : spec;
        hspec = std::max(0.0f, hspec);
    }

    auto constrain_height = [&](float height) {
        auto bound = [&](const Length& value) {
            return std::max(0.0f,resolve_length(value,std::max(0.0f,cb_h),st.font_size,16,false)-(st.box_sizing_border?pv:0.0f));
        };
        if (!st.max_height.is_auto() && (cb_h>=0 || !percentage_dependent(st.max_height))) height=std::min(height,bound(st.max_height));
        if (!st.min_height.is_auto() && (cb_h>=0 || !percentage_dependent(st.min_height))) height=std::max(height,bound(st.min_height));
        return std::max(0.0f,height);
    };
    // Descendant percentages use the constrained definite size, including a
    // stretched flex size, rather than the unconstrained requested height.
    if(hspec>=0)hspec=constrain_height(hspec);
    float used = 0;
    // Indefinite is different from a definite zero. An auto-height block must
    // not lend its ancestor's height to percentage-height descendants.
    float child_cb_h = hspec;
    if (st.display == Display::Flex) used = layout_flex(n, cw, hspec);
    else if (st.display == Display::Grid) used = layout_grid(n, cw, hspec);
    else if (st.display == Display::Table) used = layout_table(n, cw, hspec);
    else used = layout_block_content(n, cw, hspec, child_cb_h);
    float ch = hspec >= 0 ? hspec : used;
    if (st.display == Display::Table) ch = std::max(ch, used);
    ch = constrain_height(ch);

    n->layout.content_w = cw;
    n->layout.content_h = ch;
    n->layout.content_x = n->layout.padding_left + n->layout.border_left;
    n->layout.content_y = n->layout.padding_top + n->layout.border_top;
    n->layout.border.w = cw + ph;
    n->layout.border.h = ch + pv;
    n->layout.has_box = true;
    apply_auto_margins(n, cb_w);
}

void LayoutEngine::place_absolute(Node* parent, Node* child, float content_w, float cursor,
                                  float parent_height) {
    if(child->style.position==Position::Absolute&&parent->style.position==Position::Static&&parent!=doc.html)
        diag::approximate("absolute containing block through static ancestors",node_label(parent));
    // Positioned containing blocks use the padding box. Stored child positions
    // are relative to the content origin, so explicit insets need that offset.
    const float padding_x=parent->layout.padding_left+parent->layout.padding_right;
    const float padding_y=parent->layout.padding_top+parent->layout.padding_bottom;
    const float containing_w=content_w+padding_x;
    const float containing_h=std::max(0.0f,parent_height)+padding_y;
    const float left = child->style.left.is_auto()
                           ? 0
                           : resolve_l(child->style.left, containing_w, child->style.font_size);
    const float right = child->style.right.is_auto()
                            ? 0
                            : resolve_l(child->style.right, containing_w, child->style.font_size);
    // Shrink-to-fit into the space the inset properties leave, not the whole
    // containing block.
    layout_node(child, containing_w, containing_h, true, -1, -1, true,
                std::max(0.0f, containing_w - left - right));
    float x = 0, y = cursor;
    if (!child->style.left.is_auto()) x = left-parent->layout.padding_left;
    else if (!child->style.right.is_auto()) x = containing_w - child->layout.border.w - right-parent->layout.padding_left;
    if (!child->style.top.is_auto()) y = resolve_l(child->style.top, containing_h, child->style.font_size)-parent->layout.padding_top;
    else if (!child->style.bottom.is_auto()) {
        y = containing_h - child->layout.border.h - resolve_l(child->style.bottom, containing_h, child->style.font_size)-parent->layout.padding_top;
    }
    child->layout.border.x = x;
    child->layout.border.y = y;
    parent->layout.box_children.push_back(child);
}

float LayoutEngine::layout_block_content(Node* n, float content_w, float hspec, float child_cb_h) {
    float cursor = 0;
    bool flow_content=false;
    const bool escaping_margins=n!=doc.html&&n->style.display==Display::Block&&n->style.overflow==Overflow::Visible&&
        n->style.position!=Position::Absolute&&n->style.position!=Position::Fixed&&
        (!n->parent||(n->parent->style.display!=Display::Flex&&n->parent->style.display!=Display::Grid));
    // Adjoining sibling margins form one strut: greatest positive + least
    // negative. Empty blocks can join the same strut; inline content breaks it.
    float positive_margin=0,negative_margin=0;
    auto add_margin=[&](float margin){positive_margin=std::max(positive_margin,margin);negative_margin=std::min(negative_margin,margin);};
    auto margin=[&](){return positive_margin+negative_margin;};
    auto clear_margin=[&](){positive_margin=negative_margin=0;};
    std::vector<Node*> group;
    std::vector<std::pair<Node*,float>> absolutes;
    auto flush = [&]() {
        if (group.empty()) return;
        float used = layout_inline_group(n, group, content_w, cursor+margin());
        if(used>0){cursor+=margin()+used;clear_margin();flow_content=true;}
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
            absolutes.push_back({child,cursor+margin()});
            n->layout.box_children.push_back(child); // Preserve DOM paint order.
            continue;
        }
        if (child->style.display == Display::Inline || child->style.display == Display::InlineBlock) {
            group.push_back(child);
            continue;
        }
        flush();
        layout_node(child, content_w, child_cb_h, false);
        if(!flow_content&&escaping_margins&&n->layout.padding_top==0&&n->layout.border_top==0&&child->layout.margin_top!=0)
            diag::approximate("parent-child margin collapse",node_label(n));
        float x = child->layout.margin_left;
        add_margin(child->layout.margin_top);
        float y = cursor + margin();
        const bool relative = child->style.position == Position::Relative;
        if (relative) {
            x += flat_relative_x(child, content_w);
            y += flat_relative_y(child, child_cb_h);
        }
        child->layout.border.x = x;
        child->layout.border.y = y;
        n->layout.box_children.push_back(child);
        // Flow advances by the un-offset box; top/left only move a relatively
        // positioned box visually. Static boxes ignore them entirely.
        const bool empty=child->style.display==Display::Block&&child->style.overflow==Overflow::Visible&&
            child->layout.border.h==0&&child->layout.box_children.empty()&&!child->layout.has_lines;
        if(!empty){cursor+=margin()+child->layout.border.h;clear_margin();flow_content=true;}
        add_margin(child->layout.margin_bottom);
    }
    flush();
    if(escaping_margins&&hspec<0&&n->layout.padding_bottom==0&&n->layout.border_bottom==0&&margin()!=0&&
       (n->style.min_height.is_auto()||n->style.min_height.is_zero()))
        diag::approximate("parent-child margin collapse",node_label(n));
    for(const auto& entry:absolutes){
        place_absolute(n,entry.first,content_w,entry.second,hspec>=0?hspec:cursor+margin());
        n->layout.box_children.pop_back(); // Already inserted at its DOM position.
    }
    return cursor+margin();
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
        layout_node(g, content_w, -1, true);
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
        if (g->style.display == Display::InlineBlock && g->style.overflow == Overflow::Visible && !g->layout.lines.empty()) {
            const auto& last = g->layout.lines.back();
            t.ascent = std::min(t.h, g->layout.content_y + last.y + last.baseline);
            t.descent = t.h - t.ascent;
        }
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
            // Text glyphs may overflow a deliberately small line-height; replaced and
            // inline-block boxes still occupy their full height in the line box.
            if (t.kind == Token::ImageTok || t.kind == Token::BoxTok) lh = std::max(lh, t.h);
        }
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
                t.box->layout.border.y = lb.y + (baseline - t.ascent);
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
            layout_node(it, content_w, hspec, !stretch, forced, -1);
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
        // A definite column container makes allocated main sizes definite.
        // Reflow percentage descendants after flexing their parent's box.
        if(hspec>=0)for(auto* it:items){
            const float width=it->layout.content_w,height=it->layout.content_h;
            layout_node(it,content_w,hspec,true,width,height);
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
        for (auto* a : absolutes) place_absolute(n, a, content_w, 0, container_h);
        return container_h;
    }

    // Row layout, optionally wrapped.
    float gap = st.column_gap;
    auto width_limits=[&](Node* it){const auto& s=it->style;
        const float padding=s.box_sizing_border?pad_h(it,content_w):0;
        const float minimum=s.min_width.is_auto()?0:std::max(0.0f,resolve_l(s.min_width,content_w,s.font_size)-padding);
        const float maximum=s.max_width.is_auto()?INFINITY:std::max(minimum,resolve_l(s.max_width,content_w,s.font_size)-padding);
        return std::make_pair(minimum,maximum);
    };
    auto base_content = [&](Node* it) -> float {
        auto& s = it->style;
        if (!s.flex_basis.is_auto()) return std::max(0.0f, resolve_l(s.flex_basis, content_w, s.font_size)-(s.box_sizing_border?pad_h(it,content_w):0));
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
            const auto limits=width_limits(items[i]);
            const float hypothetical_outer=outer_of(items[i],std::clamp(base[i],limits.first,limits.second));
            float add = hypothetical_outer + (line.empty() ? 0 : gap);
            if (!line.empty() && w + add > content_w) {
                lines.push_back(line);
                line.clear();
                w = 0;
                add = hypothetical_outer;
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
        float sum_outer = 0;
        for (size_t i : line) {
            sum_outer += base_outer[i];
        }
        sum_outer += (line.size() > 1 ? (line.size() - 1) * gap : 0);
        const float raw_free = content_w - sum_outer;
        std::vector<float> final_content(line.size(),0),minimum(line.size(),0),maximum(line.size(),INFINITY);
        std::vector<bool> frozen(line.size(),false);
        float hypothetical=0;
        for (size_t k = 0; k < line.size(); k++) {
            auto* it=items[line[k]];const auto limits=width_limits(it);
            minimum[k]=limits.first;maximum[k]=limits.second;
            final_content[k]=std::clamp(base[line[k]],minimum[k],maximum[k]);
            hypothetical+=outer_of(it,final_content[k]);
        }
        hypothetical+=line.size()>1?(line.size()-1)*gap:0;
        const bool growing=hypothetical<content_w;
        for(size_t k=0;k<line.size();++k){const auto& s=items[line[k]]->style;
            frozen[k]=(growing?s.flex_grow:s.flex_shrink)<=0||
                (growing?base[line[k]]>final_content[k]:base[line[k]]<final_content[k]);
        }
        float initial_free=raw_free;
        for(size_t k=0;k<line.size();++k)if(frozen[k])initial_free+=base[line[k]]-final_content[k];
        // Freeze constrained items and redistribute their unused share, rather
        // than clamping once and leaving gaps or overflowing the flex line.
        for(size_t pass=0;pass<=line.size();++pass){
            float free=raw_free,factors=0,weights=0;
            for(size_t k=0;k<line.size();++k){const auto& s=items[line[k]]->style;
                if(frozen[k])free+=base[line[k]]-final_content[k];
                else {const float factor=std::max(0.0f,growing?s.flex_grow:s.flex_shrink);factors+=factor;weights+=growing?factor:factor*base[line[k]];}
            }
            if(factors<1&&std::abs(initial_free*factors)<std::abs(free))free=initial_free*factors;
            float violation=0;std::vector<float> violations(line.size(),0);
            for(size_t k=0;k<line.size();++k)if(!frozen[k]){
                const auto& s=items[line[k]]->style;
                const float weight=growing?std::max(0.0f,s.flex_grow):std::max(0.0f,s.flex_shrink)*base[line[k]];
                const float target=base[line[k]]+(weights>0?free*weight/weights:0);
                final_content[k]=std::clamp(target,minimum[k],maximum[k]);
                violations[k]=final_content[k]-target;violation+=violations[k];
            }
            if(std::abs(violation)<.0001f)break;
            bool progress=false;
            for(size_t k=0;k<line.size();++k)if(!frozen[k]&&(violation>0?violations[k]>0:violations[k]<0)){frozen[k]=true;progress=true;}
            if(!progress)break;
        }
        std::vector<float> outer_h(line.size(), 0);
        for (size_t k = 0; k < line.size(); k++) {
            size_t i = line[k];
            Node* it = items[i];
            if(it->style.min_width.is_auto()&&it->style.overflow==Overflow::Visible&&final_content[k]+.01f<minimum_width(it,content_w,0))
                diag::approximate("flex automatic minimum size",node_label(it));
            const float mt=resolve_l(it->style.margin.top,content_w,it->style.font_size),mb=resolve_l(it->style.margin.bottom,content_w,it->style.font_size);
            const bool definite_stretch=hspec>=0&&lines.size()==1&&st.align_items==AlignItems::Stretch&&it->style.height.is_auto();
            const float stretched_height=definite_stretch?std::max(0.0f,hspec-mt-mb-pad_v(it,content_w)):-1;
            layout_node(it, content_w, hspec, true, final_content[k], stretched_height);
            outer_h[k] = it->layout.border.h + resolve_l(it->style.margin.top, content_w, it->style.font_size) +
                         resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
        }
        float line_cross = 0;
        for (float h : outer_h) line_cross = std::max(line_cross, h);
        float cross_size = line_cross;
        if (hspec >= 0 && lines.size() == 1) cross_size = hspec;
        for (size_t k = 0; k < line.size(); k++) {
            Node* it = items[line[k]];
            if (st.align_items == AlignItems::Stretch && it->style.height.is_auto() &&
                it->style.display != Display::Inline) {
                float mt = resolve_l(it->style.margin.top, content_w, it->style.font_size);
                float mb = resolve_l(it->style.margin.bottom, content_w, it->style.font_size);
                const float target=std::max(0.0f,cross_size-mt-mb-pad_v(it,content_w));
                if (!(hspec>=0&&lines.size()==1)) {
                    // Auto-sized lines need a second layout with a definite
                    // used cross size; resizing just the box leaves children stale.
                    layout_node(it,content_w,hspec,true,final_content[k],target);
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
    for (auto* a : absolutes) place_absolute(n, a, content_w, 0, used_h);
    return used_h;
}

std::vector<LayoutEngine::TableRow> LayoutEngine::table_rows(Node* table) {
    std::vector<TableRow> headers, body, footers;
    auto append = [&](Node* row, Node* group, std::vector<TableRow>& output) {
        if (!row || row->style.display != Display::TableRow) return;
        TableRow result{row,group,{}};
        for (auto* cell : row->children)
            if (cell && cell->is_element() && cell->style.display == Display::TableCell) result.cells.push_back(cell);
        output.push_back(std::move(result));
    };
    for (auto* child : table->children) {
        if (!child || !child->is_element()) continue;
        auto display=child->style.display;
        if (display == Display::TableRow) append(child,nullptr,body);
        else if (display==Display::TableHeader || display==Display::TableGroup || display==Display::TableFooter) {
            auto& output=display==Display::TableHeader?headers:display==Display::TableFooter?footers:body;
            for (auto* row : child->children) append(row,child,output);
        }
    }
    headers.insert(headers.end(),body.begin(),body.end());
    headers.insert(headers.end(),footers.begin(),footers.end());
    return headers;
}

float LayoutEngine::minimum_width(Node* node,float basis,int depth) {
    if (!node || depth>30 || node->style.display==Display::None) return 0;
    if (node->is_text()) {
        if (node->style.white_space==WhiteSpace::Nowrap || node->style.white_space==WhiteSpace::Pre)
            return fonts.text_width(collapse_text(node->data,node->style),node->style);
        std::vector<Token> tokens;append_text_tokens(node,tokens);float width=0;
        for (const auto& token:tokens) if(token.kind==Token::Word) width=std::max(width,token.w);
        return width;
    }
    if (!node->is_element()) return 0;
    if (node->is_element("img") || node->is_element("svg")) return intrinsic_content_width(node,basis,depth+1);
    if (node->style.display==Display::Table) {
        const auto columns=table_columns(table_rows(node),basis,true,depth+1);
        float width=columns.empty()?0:node->style.border_spacing_x*(columns.size()+1);
        for(float value:columns)width+=value;
        return width;
    }
    float width=0;
    for(auto* child:node->children)width=std::max(width,minimum_width(child,basis,depth+1));
    return width;
}

std::vector<float> LayoutEngine::table_columns(const std::vector<TableRow>& rows,float basis,bool minimum,int depth) {
    std::vector<float> columns;
    if(depth>30)return columns;
    for(const auto& row:rows){
        columns.resize(std::max(columns.size(),row.cells.size()),0);
        for(size_t i=0;i<row.cells.size();++i){auto* cell=row.cells[i];
            float content=minimum?minimum_width(cell,basis,depth+1):intrinsic_content_width(cell,basis,depth+1);
            // A specified cell width is a preference, not a minimum: the automatic
            // algorithm may shrink the column down to its minimum content width.
            if(!minimum&&!cell->style.width.is_auto()){
                float requested=resolve_l(cell->style.width,basis,cell->style.font_size);
                if(cell->style.box_sizing_border)requested-=pad_h(cell,basis);
                content=std::max(content,requested);
            }
            if(!cell->style.min_width.is_auto())content=std::max(content,resolve_l(cell->style.min_width,basis,cell->style.font_size));
            columns[i]=std::max(columns[i],content+pad_h(cell,basis));
        }
    }
    return columns;
}

float LayoutEngine::layout_table(Node* table,float& width,float hspec) {
    const std::string label=node_label(table);
    for(auto* child:table->children){
        if(!child||!child->is_element())continue;
        if(child->is_element("caption"))diag::structural("table caption is ignored",label);
        else if(child->is_element("colgroup")||child->is_element("col"))diag::structural("table col/colgroup sizing is ignored",label);
    }
    auto rows=table_rows(table);if(rows.empty())return 0;
    bool percent_cells=false,max_width_cells=false;
    for(const auto& row:rows)for(auto* cell:row.cells){
        if(cell->has_attr("rowspan")||cell->has_attr("colspan")){
            const std::string rowspan=util::trim(cell->attr("rowspan")),colspan=util::trim(cell->attr("colspan"));
            if((!rowspan.empty()&&rowspan!="1")||(!colspan.empty()&&colspan!="1"))
                diag::structural("table rowspan/colspan is ignored",label);
        }
        if(!cell->style.width.is_auto()&&cell->style.width.unit==Length::Percent)percent_cells=true;
        if(!cell->style.max_width.is_auto())max_width_cells=true;
    }
    auto preferred=table_columns(rows,width,false,0), minimum=table_columns(rows,width,true,0);
    const size_t count=preferred.size();if(!count)return 0;
    const float sx=table->style.border_spacing_x,sy=table->style.border_spacing_y;
    const float gutters=sx*(count+1);
    std::vector<float> columns(count,0);
    const bool fixed=table->style.table_fixed&&!table->style.width.is_auto();
    if(!fixed&&(percent_cells||max_width_cells))diag::approximate("automatic table column distribution",label);
    if(table->style.table_fixed&&table->style.width.is_auto())
        diag::approximate("table-layout:fixed with auto width uses automatic layout",label);
    if(table->parent&&table->parent->style.display==Display::TableCell&&table->style.width.is_auto())
        diag::approximate("auto-width table nested in a cell",label);
    if(fixed){
        std::vector<char> is_percent(count,0),is_auto(count,0);
        size_t unspecified=0,length_columns=0;
        float fixed_sum=0,percent_sum=0;
        for(size_t i=0;i<count;++i){
            Node* cell=i<rows.front().cells.size()?rows.front().cells[i]:nullptr;
            if(cell&&!cell->style.width.is_auto()){
                const bool percent=cell->style.width.unit==Length::Percent;
                float value=resolve_l(cell->style.width,width,cell->style.font_size);
                if(!cell->style.box_sizing_border)value+=pad_h(cell,width);
                columns[i]=value;is_percent[i]=percent;
                if(percent){percent_sum+=value;}
                else{fixed_sum+=value;++length_columns;}
            }else{is_auto[i]=1;++unspecified;}
        }
        // Only fixed lengths enlarge the table; percentage columns are fitted into
        // the space left after them and the border spacing.
        width=std::max(width,fixed_sum+gutters);
        const float column_area=std::max(0.0f,width-gutters);
        if(fixed_sum+percent_sum>column_area){
            const float scale=percent_sum>0?std::max(0.0f,column_area-fixed_sum)/percent_sum:0;
            for(size_t i=0;i<count;++i)if(is_percent[i])columns[i]*=scale;
        }else{
            const float extra=column_area-fixed_sum-percent_sum;
            if(unspecified){
                const float each=extra/std::max<size_t>(1,unspecified);
                for(size_t i=0;i<count;++i)if(is_auto[i])columns[i]=each;
            }else if(length_columns){
                const float each=extra/length_columns;
                for(size_t i=0;i<count;++i)if(!is_percent[i])columns[i]+=each;
            }else{
                const float each=extra/count;
                for(auto& column:columns)column+=each;
            }
        }
    }else{
        float min_sum=0,pref_sum=0;for(size_t i=0;i<count;++i){preferred[i]=std::max(preferred[i],minimum[i]);min_sum+=minimum[i];pref_sum+=preferred[i];}
        width=std::max(width,min_sum+gutters);
        const float available=std::max(0.0f,width-gutters);
        for(size_t i=0;i<count;++i){
            if(available>=pref_sum)columns[i]=preferred[i]+(available-pref_sum)*(pref_sum>0?preferred[i]/pref_sum:1.f/count);
            else columns[i]=minimum[i]+(available-min_sum)*(pref_sum>min_sum?(preferred[i]-minimum[i])/(pref_sum-min_sum):1.f/count);
        }
    }
    float row_width=std::max(0.0f,width-2*sx);
    std::vector<float> heights(rows.size(),0);
    for(size_t i=0;i<rows.size();++i){auto& row=rows[i];float baseline=0,descent=0;
        for(size_t j=0;j<row.cells.size();++j){auto* cell=row.cells[j];
            // The column width already accounts for the cell's own preferred and
            // minimum widths; re-applying min/max here would detach the cell box
            // from its shared column.
            layout_node(cell,width,hspec,false,std::max(0.0f,columns[j]-pad_h(cell,width)),-1,false);
            heights[i]=std::max(heights[i],cell->layout.border.h);
            if(cell->style.vertical_align==VerticalAlign::Baseline){
                float b=cell->layout.content_y+(cell->layout.lines.empty()?cell->layout.content_h:cell->layout.lines.front().baseline);
                baseline=std::max(baseline,b);descent=std::max(descent,cell->layout.border.h-b);
            }
        }
        heights[i]=std::max(heights[i],baseline+descent);
        if(!row.node->style.height.is_auto())heights[i]=std::max(heights[i],resolve_l(row.node->style.height,hspec>0?hspec:0,row.node->style.font_size));
    }
    float total=sy*(rows.size()+1);for(float height:heights)total+=height;
    if(hspec>total){float extra=(hspec-total)/rows.size();for(auto& height:heights)height+=extra;total=hspec;}
    auto box=[](Node* node,float x,float y,float w,float h){node->layout=LayoutInfo{};auto& l=node->layout;l.has_box=true;l.border={x,y,w,h};l.content_w=w;l.content_h=h;};
    float y=sy;
    for(size_t i=0;i<rows.size();++i){auto& row=rows[i];const float height=heights[i];
        if(row.group){auto& group=row.group->layout;
            if(!group.has_box){box(row.group,sx,y,row_width,0);table->layout.box_children.push_back(row.group);}
            box(row.node,0,y-group.border.y,row_width,height);group.box_children.push_back(row.node);
            group.border.h=y+height-group.border.y;group.content_h=group.border.h;
        }else{box(row.node,sx,y,row_width,height);table->layout.box_children.push_back(row.node);}
        float row_baseline=0;for(auto* cell:row.cells)if(cell->style.vertical_align==VerticalAlign::Baseline)
            row_baseline=std::max(row_baseline,cell->layout.content_y+(cell->layout.lines.empty()?cell->layout.content_h:cell->layout.lines.front().baseline));
        float x=0;
        for(size_t j=0;j<row.cells.size();++j){auto* cell=row.cells[j];auto& l=cell->layout;
            const float extra=std::max(0.0f,height-l.border.h);float shift=0;
            if(cell->style.vertical_align==VerticalAlign::Middle)shift=extra/2;
            else if(cell->style.vertical_align==VerticalAlign::Bottom)shift=extra;
            else if(cell->style.vertical_align==VerticalAlign::Baseline)shift=row_baseline-l.content_y-(l.lines.empty()?l.content_h:l.lines.front().baseline);
            for(auto& line:l.lines){line.y+=shift;for(auto& item:line.items)item.y+=shift;}
            for(auto* child:l.box_children){
                if(child->style.position==Position::Absolute||child->style.position==Position::Fixed){if(child->style.top.is_auto()&&!child->style.bottom.is_auto())child->layout.border.y+=extra;}
                else child->layout.border.y+=shift;
            }
            l.border.x=x;l.border.y=0;l.content_h+=extra;l.border.h=height;
            row.node->layout.box_children.push_back(cell);x+=columns[j]+sx;
        }
        y+=height+sy;
    }
    return total;
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
        layout_node(it,p.width,hspec,true,fw,-1);row_h[p.row]=std::max(row_h[p.row],it->layout.border.h+mt+mb);
    }
    for(int r=0;r<rows&&r<(int)st.grid_rows.size();++r){const auto t=st.grid_rows[r];if(t.kind==TrackKind::Px)row_h[r]=std::max(row_h[r],t.value);else if(t.kind==TrackKind::Percent)row_h[r]=std::max(row_h[r],t.value*std::max(0.0f,hspec)/100);}
    float y=0;for(int r=0;r<rows;++r){for(auto& p:placed){if(p.row!=r)continue;auto* it=p.node;float x=0;for(int c=0;c<p.col;++c)x+=cols[c]+col_gap;
        const float mt=resolve_l(it->style.margin.top,content_w,it->style.font_size),mb=resolve_l(it->style.margin.bottom,content_w,it->style.font_size),ml=resolve_l(it->style.margin.left,content_w,it->style.font_size);
        if(it->style.height.is_auto()&&st.align_items==AlignItems::Stretch){
            const float target=std::max(0.0f,row_h[r]-mt-mb-pad_v(it,content_w));
            const float width=it->layout.content_w;
            layout_node(it,p.width,row_h[r],true,width,target);
        }
        it->layout.border.x=x+ml;it->layout.border.y=y+mt;n->layout.box_children.push_back(it);
    }y+=row_h[r]+st.row_gap;}
    float used = hspec >= 0 ? hspec : std::max(0.0f, y - (rows ? st.row_gap : 0));
    for (auto* a : absolutes) place_absolute(n, a, content_w, 0, used);
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
    if (n->style.display == Display::Table) {
        const auto columns=table_columns(table_rows(n),basis,false,depth+1);
        float width=columns.empty()?0:n->style.border_spacing_x*(columns.size()+1);
        for(float column:columns)width+=column;return width;
    }
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
