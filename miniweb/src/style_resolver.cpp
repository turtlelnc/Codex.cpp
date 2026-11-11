#include "css.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>

const char* kUserAgentCss = R"CSS(
html, body, div, p, section, article, aside, header, footer, main, nav, figure, figcaption,
address, blockquote, pre, form, fieldset, legend, details, summary, ul, ol, li, dl, dt, dd,
table, thead, tbody, tfoot, tr, td, th, h1, h2, h3, h4, h5, h6, hr { display: block; }
head, title, meta, link, style, script, base, template, noscript, source, track { display: none; }
body { margin: 8px; }
p { margin: 1em 0; }
h1 { font-size: 2em; font-weight: bold; margin: 0.67em 0; }
h2 { font-size: 1.5em; font-weight: bold; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }
h4 { font-size: 1em; font-weight: bold; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }
ul, ol { margin: 1em 0; padding-left: 40px; }
li { display: block; }
a { color: #0000ee; text-decoration: underline; }
strong, b { font-weight: bold; }
em, i { font-style: italic; }
code, pre, kbd, samp { font-family: monospace; }
pre { white-space: pre; margin: 1em 0; }
small { font-size: 0.83em; }
big { font-size: 1.17em; }
hr { border: 0; border-top: 1px solid #999999; margin: 0.5em 0; }
button { display: inline-block; padding: 4px 10px; border: 1px solid #cccccc; background: #f0f0f0; color: #111111; text-align: center; }
input, select, textarea { display: inline-block; font-size: 14px; }
input[type=text], input[type=search], input[type=email], input[type=password], input[type=number], input:not([type]) {
  width: 150px; height: 24px; border: 1px solid #999999; padding: 2px 4px; background: #ffffff;
}
textarea { width: 220px; height: 60px; border: 1px solid #999999; padding: 3px; background: #ffffff; }
select { height: 24px; border: 1px solid #999999; background: #ffffff; padding: 1px 4px; }
img { display: inline-block; }
svg, canvas { display: inline-block; }
[hidden] { display: none !important; }
)CSS";

namespace {

float g_viewport_w = 1280;
float g_viewport_h = 800;

struct DeclItem {
    const CssDeclaration* decl = nullptr;
    std::string name;
    std::string value;
    bool important = false;
    bool inline_style = false;
    int spec_id = 0, spec_class = 0, spec_tag = 0;
    int order = 0;
    int index = 0;
};

std::vector<std::string> split_ws_paren(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int par = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < s.size()) { cur.push_back(s[i + 1]); i++; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '(') par++;
        if (c == ')') par--;
        if (std::isspace((unsigned char)c) && par == 0) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string strip_quotes(const std::string& s) {
    std::string t = util::trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\'')))
        return t.substr(1, t.size() - 2);
    return t;
}

std::vector<std::string> split_top_commas(const std::string& s);
Length parse_length_value(const std::string& input, float font_size, float root_size, bool allow_auto,
                          bool* ok = nullptr) {
    std::string s = util::to_lower(util::trim(input));
    if (ok) *ok = false;
    if (s.empty()) return {};
    if((util::starts_with(s,"min(")||util::starts_with(s,"max(")||util::starts_with(s,"clamp("))&&s.back()==')'){
        const auto open=s.find('(');auto parts=split_top_commas(s.substr(open+1,s.size()-open-2));Length result;result.unit=s.substr(0,open)=="min"?Length::Min:s.substr(0,open)=="max"?Length::Max:Length::Clamp;
        if(parts.empty()||parts.size()>32||(result.unit==Length::Clamp&&parts.size()!=3))return {};
        for(const auto& part:parts){bool valid=false;auto l=parse_length_value(part,font_size,root_size,false,&valid);if(!valid)return {};result.arguments.push_back(std::move(l));}if(ok)*ok=true;return result;
    }
    if (util::starts_with(s, "calc(") && util::ends_with(s, ")")) {
        std::string expr = util::trim(s.substr(5, s.size() - 6));
        // Support the common subset: sum/difference of lengths, e.g.
        // calc(100% - 20px), calc(100vh - 64px), calc(50% + 1rem).
        std::vector<std::pair<char, std::string>> terms;
        char sign = '+';
        std::string cur;
        int par = 0;
        char quote = 0;
        for (size_t i = 0; i < expr.size(); i++) {
            char c = expr[i];
            if (quote) {
                cur.push_back(c);
                if (c == quote) quote = 0;
                continue;
            }
            if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
            if (c == '(') par++;
            if (c == ')') par--;
            if (par == 0 && (c == '+' || c == '-') && i > 0) {
                terms.emplace_back(sign, util::trim(cur));
                sign = c;
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        terms.emplace_back(sign, util::trim(cur));
        float pct = 0, px = 0;
        bool any_pct = false;
        bool good = true;
        for (auto& t : terms) {
            if (t.second.empty()) { good = false; break; }
            bool term_ok = false;
            Length l = parse_length_value(t.second, font_size, root_size, false, &term_ok);
            if (!term_ok) { good = false; break; }
            float sgn = t.first == '-' ? -1.0f : 1.0f;
            if (l.unit == Length::Percent) {
                pct += sgn * l.value;
                px += sgn * l.offset;
                any_pct = true;
            } else {
                px += sgn * resolve_length(l, 0, font_size, root_size, true);
            }
        }
        if (!good) return {};
        Length out;
        out.unit = any_pct ? Length::Percent : Length::Px;
        out.value = any_pct ? pct : px;
        out.offset = any_pct ? px : 0;
        if (ok) *ok = true;
        return out;
    }
    if (s == "auto") {
        if (allow_auto) { if (ok) *ok = true; return Length{}; }
        return {};
    }
    if (s == "none" || s == "fit-content" || s == "min-content" || s == "max-content") {
        if (ok) *ok = true;
        return Length{};
    }
    float mul = 1;
    Length::Unit unit = Length::Px;
    if (util::ends_with(s, "px")) { s = s.substr(0, s.size() - 2); }
    else if (util::ends_with(s, "rem")) { s = s.substr(0, s.size() - 3); unit = Length::Rem; }
    else if (util::ends_with(s, "em")) { s = s.substr(0, s.size() - 2); unit = Length::Em; }
    else if (util::ends_with(s, "%")) { s = s.substr(0, s.size() - 1); unit = Length::Percent; }
    else if (util::ends_with(s, "pt")) { s = s.substr(0, s.size() - 2); mul = 96.0f / 72.0f; }
    else if (util::ends_with(s, "vh")) {
        s = s.substr(0, s.size() - 2);
        float vh = std::strtof(util::trim(s).c_str(), nullptr);
        if (ok) *ok = true;
        return Length::px(vh * g_viewport_h / 100.0f);
    } else if (util::ends_with(s, "vw")) {
        s = s.substr(0, s.size() - 2);
        float vw = std::strtof(util::trim(s).c_str(), nullptr);
        if (ok) *ok = true;
        return Length::px(vw * g_viewport_w / 100.0f);
    } else if (util::ends_with(s, "vmin") || util::ends_with(s, "vmax")) {
        bool is_max = util::ends_with(s, "vmax");
        s = s.substr(0, s.size() - 4);
        float v = std::strtof(util::trim(s).c_str(), nullptr);
        if (ok) *ok = true;
        float base = is_max ? std::max(g_viewport_w, g_viewport_h) : std::min(g_viewport_w, g_viewport_h);
        return Length::px(v * base / 100.0f);
    }
    std::string num = util::trim(s);
    const char* cstr = num.c_str();
    char* endp = nullptr;
    float v = std::strtof(cstr, &endp);
    if (endp == cstr) return {};
    Length l;
    l.unit = unit;
    l.value = v * mul;
    if (unit == Length::Percent) l.value = v; // vw approximated below? keep percentage
    if (ok) *ok = true;
    return l;
}

float parse_float_value(const std::string& s, float def = 0) {
    char* e = nullptr;
    float v = std::strtof(util::trim(s).c_str(), &e);
    if (e == s.c_str()) return def;
    return v;
}

bool is_inherited_prop(const std::string& p) {
    return p == "color" || p == "font-family" || p == "font-size" || p == "font-weight" ||
           p == "font-style" || p == "line-height" || p == "text-align" || p == "white-space" ||
           p == "visibility" || p == "letter-spacing" || p == "text-transform" ||
           util::starts_with(p, "font") || util::starts_with(p, "text-");
}

void reset_display(ComputedStyle& st) { st.display = Display::Inline; }

void reset_prop(ComputedStyle& st, const std::string& p) {
    if (p == "display") st.display = Display::Inline;
    else if (p == "position") st.position = Position::Static;
    else if (p == "color") st.color = Color{0, 0, 0, 255};
    else if (p == "background-color" || p == "background" || p == "background-image") {
        st.background = Color{0, 0, 0, 0};
        st.background_gradient = Gradient{};
        st.background_image.clear();
        st.background_size = BackgroundSize::Auto;
    } else if (p == "background-size") {
        st.background_size = BackgroundSize::Auto;
    } else if (p == "box-shadow") {
        st.shadows.clear();
    }
    else if (p == "font-size") st.font_size = 16;
    else if (p == "font-family") st.font_family.clear();
    else if (p == "font-weight") st.font_weight = 400;
    else if (p == "font-style") st.font_style = FontStyle::Normal;
    else if (p == "line-height") st.line_height = Length{};
    else if (p == "text-align") st.text_align = TextAlign::Left;
    else if (p == "white-space") st.white_space = WhiteSpace::Normal;
    else if (p == "visibility") st.visibility = Visibility::Visible;
    else if (p == "opacity") st.opacity = 1;
    else if (p == "width") st.width = Length{};
    else if (p == "height") st.height = Length{};
    else if (p == "min-width") st.min_width = Length{};
    else if (p == "min-height") st.min_height = Length{};
    else if (p == "max-width") st.max_width = Length{};
    else if (p == "max-height") st.max_height = Length{};
    else if (p == "margin") {
        st.margin = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)};
    } else if (p == "padding") {
        st.padding = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)};
    }
    else if (util::starts_with(p, "border")) {
        for (auto& b : st.border) { b.width = 0; b.style = BorderStyle::None; b.color = Color{0,0,0,255}; }
    } else if (p == "border-radius") st.border_radius = 0;
    else if (p == "overflow") st.overflow = Overflow::Visible;
    else if (p == "box-sizing") st.box_sizing_border = false;
    else if (p == "flex-grow") st.flex_grow = 0;
    else if (p == "flex-shrink") st.flex_shrink = 1;
    else if (p == "flex-basis") st.flex_basis = Length{};
    else if (p == "aspect-ratio") st.aspect_ratio = 0;
    else if (p == "flex") { st.flex_grow = 0; st.flex_shrink = 1; st.flex_basis = Length{}; }
    else if (p == "flex-direction") st.flex_direction = FlexDirection::Row;
    else if (p == "flex-wrap") st.flex_wrap = FlexWrap::NoWrap;
    else if (p == "justify-content") st.justify = Justify::FlexStart;
    else if (p == "align-items") st.align_items = AlignItems::Stretch;
    else if (p == "gap") { st.row_gap = st.column_gap = 0; }
    else if (p == "row-gap") st.row_gap = 0;
    else if (p == "column-gap") st.column_gap = 0;
    else if (p == "order") st.order = 0;
    else if (p == "grid-column") st.grid_column_span=1;
    else if(p=="transform") {st.translate_x=Length::px(0);st.translate_y=Length::px(0);}
    else if (p == "z-index") st.z_index = 0;
    else if (p == "top") st.top = Length{};
    else if (p == "right") st.right = Length{};
    else if (p == "bottom") st.bottom = Length{};
    else if (p == "left") st.left = Length{};
    else if (p == "text-decoration") { st.underline = st.line_through = false; }
    else if (p == "letter-spacing") st.letter_spacing = 0;
    else if (p == "text-transform") st.text_transform = TextTransform::None;
    else if (p == "object-fit") st.object_fit = ObjectFit::Fill;
}

bool inherit_prop(ComputedStyle& st, const std::string& p, const ComputedStyle* parent) {
    if (!parent) { reset_prop(st, p); return true; }
    if (p == "color") st.color = parent->color;
    else if (p == "background-color" || p == "background" || p == "background-image") {
        st.background = parent->background;
        st.background_gradient = parent->background_gradient;
        st.background_image = parent->background_image;
        st.background_size = parent->background_size;
    }
    else if (p == "font-size") st.font_size = parent->font_size;
    else if (p == "font-family") st.font_family = parent->font_family;
    else if (p == "font-weight") st.font_weight = parent->font_weight;
    else if (p == "font-style") st.font_style = parent->font_style;
    else if (p == "line-height") st.line_height = parent->line_height;
    else if (p == "text-align") st.text_align = parent->text_align;
    else if (p == "white-space") st.white_space = parent->white_space;
    else if (p == "visibility") st.visibility = parent->visibility;
    else if (p == "letter-spacing") st.letter_spacing = parent->letter_spacing;
    else if (p == "text-transform") st.text_transform = parent->text_transform;
    else if (p == "display") st.display = parent->display;
    else return false;
    return true;
}

bool apply_global_keyword(ComputedStyle& st, const std::string& p, const std::string& value,
                          const ComputedStyle* parent) {
    std::string v = util::to_lower(util::trim(value));
    if (v == "inherit") return inherit_prop(st, p, parent);
    if (v == "initial") { reset_prop(st, p); return true; }
    if (v == "unset" || v == "revert") {
        if (is_inherited_prop(p)) return inherit_prop(st, p, parent);
        reset_prop(st, p);
        return true;
    }
    return false;
}

void apply_four(Edges<Length>& e, const std::string& value, float font_size, float root_size, bool auto_ok) {
    auto toks = split_ws_paren(value);
    if (toks.empty()) return;
    if (toks.size() > 4) toks.resize(4);
    Length vals[4];
    for (size_t i = 0; i < toks.size(); i++) {
        bool ok = false;
        vals[i] = parse_length_value(toks[i], font_size, root_size, auto_ok, &ok);
        if (!ok) vals[i] = Length::px(0);
    }
    if (toks.size() == 1) e = {vals[0], vals[0], vals[0], vals[0]};
    else if (toks.size() == 2) e = {vals[0], vals[1], vals[0], vals[1]};
    else if (toks.size() == 3) e = {vals[0], vals[1], vals[2], vals[1]};
    else e = {vals[0], vals[1], vals[2], vals[3]};
}

struct BorderParse {
    bool valid = true;
    bool has_width = false, has_style = false, has_color = false;
    bool current_color = true;
    float width = 0;
    BorderStyle style = BorderStyle::None;
    Color color{0, 0, 0, 255};
};

BorderParse parse_border_tokens(const std::string& value, const ComputedStyle& st) {
    BorderParse b;
    auto toks = split_ws_paren(value);
    if (toks.empty()) b.valid = false;
    for (auto& tok : toks) {
        const int before = b.has_width | (b.has_style << 1) | (b.has_color << 2);
        std::string t = util::to_lower(tok);
        if (t == "thin") { b.width = 1; b.has_width = true; }
        else if (t == "medium") { b.width = 3; b.has_width = true; }
        else if (t == "thick") { b.width = 5; b.has_width = true; }
        else if (t == "none") { b.style = BorderStyle::None; b.has_style = true; }
        else if (t == "solid") { b.style = BorderStyle::Solid; b.has_style = true; }
        else if (t == "dashed") { b.style = BorderStyle::Dashed; b.has_style = true; }
        else if (t == "dotted") { b.style = BorderStyle::Dotted; b.has_style = true; }
        else if (t == "double") { b.style = BorderStyle::Double; b.has_style = true; }
        else if (t == "hidden") { b.style = BorderStyle::None; b.has_style = true; }
        else if (t == "currentcolor") { b.color = st.color; b.has_color = true; }
        else {
            bool ok = false;
            Color c = util::parse_color(t, &ok);
            if (ok) { b.color = c; b.has_color = true; b.current_color = false; }
            else {
                bool len_ok = false;
                Length l = parse_length_value(t, st.font_size, 16, false, &len_ok);
                if (len_ok && l.unit != Length::Percent) {
                    const float width = resolve_length(l, 0, st.font_size);
                    if (std::isfinite(width) && width >= 0) { b.width = width; b.has_width = true; }
                }
            }
        }
        if ((b.has_width | (b.has_style << 1) | (b.has_color << 2)) == before) b.valid = false;
    }
    return b;
}

void apply_border_side(BorderSide& side, const BorderParse& b, bool set_width, bool set_style, bool set_color) {
    if (!b.valid) return;
    if (set_width && b.has_width) side.width = b.width;
    if (set_style && b.has_style) side.style = b.style;
    if (set_color && b.has_color) { side.color = b.color; side.current_color = b.current_color; }
}



Display parse_display(const std::string& v) {
    if (v == "block") return Display::Block;
    if (v == "inline") return Display::Inline;
    if (v == "inline-block") return Display::InlineBlock;
    if (v == "flex" || v == "inline-flex") return Display::Flex;
    if (v == "grid" || v == "inline-grid") return Display::Grid;
    if (v == "none") return Display::None;
    if (v == "list-item" || v == "table" || v == "table-row" || v == "table-cell" ||
        v == "flow-root" || v == "contents") return Display::Block;
    return Display::Inline;
}

Justify parse_justify(const std::string& v) {
    if (v == "flex-end" || v == "end" || v == "right") return Justify::FlexEnd;
    if (v == "center") return Justify::Center;
    if (v == "space-between") return Justify::SpaceBetween;
    if (v == "space-around") return Justify::SpaceAround;
    if (v == "space-evenly") return Justify::SpaceEvenly;
    return Justify::FlexStart;
}

AlignItems parse_align(const std::string& v) {
    if (v == "flex-end" || v == "end") return AlignItems::FlexEnd;
    if (v == "center") return AlignItems::Center;
    if (v == "baseline") return AlignItems::Baseline;
    if (v == "stretch") return AlignItems::Stretch;
    return AlignItems::FlexStart;
}

GridTrack parse_grid_track(const std::string& raw, float fs, bool auto_fit_context = false,
                           std::string* minmax_min = nullptr) {
    std::string t = util::to_lower(util::trim(raw));
    if (util::starts_with(t, "minmax(") && util::ends_with(t, ")")) {
        std::string inner = t.substr(7, t.size() - 8);
        size_t comma = inner.find(',');
        std::string lo = util::trim(comma == std::string::npos ? inner : inner.substr(0, comma));
        std::string hi = util::trim(comma == std::string::npos ? inner : inner.substr(comma + 1));
        if (minmax_min) *minmax_min = lo;
        if (util::ends_with(hi, "fr")) {
            GridTrack tr;
            tr.kind = TrackKind::Fr;
            tr.value = (float)std::atof(hi.substr(0, hi.size() - 2).c_str());
            if (tr.value <= 0) tr.value = 1;
            return tr;
        }
        return parse_grid_track(lo, fs);
    }
    if (util::ends_with(t, "fr")) {
        GridTrack tr;
        tr.kind = TrackKind::Fr;
        tr.value = (float)std::atof(t.substr(0, t.size() - 2).c_str());
        if (tr.value <= 0) tr.value = 1;
        return tr;
    }
    if (t == "auto" || t == "min-content" || t == "max-content" || util::starts_with(t, "fit-content(")) {
        return GridTrack{TrackKind::Auto, 0};
    }
    bool ok = false;
    Length l = parse_length_value(t, fs, 16, false, &ok);
    if (ok) {
        GridTrack tr;
        if (l.unit == Length::Percent) {
            tr.kind = TrackKind::Percent;
            tr.value = l.value;
        } else {
            tr.kind = TrackKind::Px;
            tr.value = resolve_length(l, 0, fs, 16, true);
        }
        return tr;
    }
    (void)auto_fit_context;
    return GridTrack{TrackKind::Auto, 0};
}

std::vector<GridTrack> parse_grid_tracks(const std::string& value, float fs);

std::vector<GridTrack> parse_grid_track_list(const std::string& value, float fs) {
    std::vector<GridTrack> out;
    auto toks = split_ws_paren(value);
    for (auto& tok : toks) {
        std::string t = util::to_lower(util::trim(tok));
        if (util::starts_with(t, "repeat(") && util::ends_with(t, ")")) {
            std::string inner = util::trim(tok.substr(7, tok.size() - 8));
            size_t comma = std::string::npos;
            int par = 0;
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '(') par++;
                else if (inner[i] == ')') par--;
                else if (inner[i] == ',' && par == 0) { comma = i; break; }
            }
            if (comma == std::string::npos) continue;
            std::string count = util::to_lower(util::trim(inner.substr(0, comma)));
            std::string tracks = util::trim(inner.substr(comma + 1));
            if (count == "auto-fit" || count == "auto-fill") {
                float minw = 0;
                std::string mm;
                parse_grid_track(tracks, fs, true, &mm);
                if (!mm.empty()) {
                    bool ok = false;
                    Length l = parse_length_value(mm, fs, 16, false, &ok);
                    if (ok) minw = resolve_length(l, 0, fs, 16, true);
                }
                out.push_back(GridTrack{TrackKind::AutoFit, minw});
            } else {
                int n = (int)std::atof(count.c_str());
                if (n < 1) n = 1;
                if (n > 32) n = 32;
                auto inner_tracks = parse_grid_track_list(tracks, fs);
                for (int k = 0; k < n; k++) out.insert(out.end(), inner_tracks.begin(), inner_tracks.end());
            }
        } else {
            out.push_back(parse_grid_track(tok, fs));
        }
    }
    return out;
}

std::vector<GridTrack> parse_grid_tracks(const std::string& value, float fs) {
    std::string v = util::to_lower(util::trim(value));
    if (v.empty() || v == "none") return {};
    return parse_grid_track_list(value, fs);
}

std::string extract_css_url(const std::string& value) {
    std::string low = util::to_lower(value);
    size_t p = low.find("url(");
    if (p == std::string::npos) return {};
    size_t start = p + 4;
    int par = 1;
    char quote = 0;
    size_t i = start;
    for (; i < value.size(); i++) {
        char c = value[i];
        if (quote) {
            if (c == '\\' && i + 1 < value.size()) { i++; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') par++;
        else if (c == ')') {
            par--;
            if (par == 0) break;
        }
    }
    if (i <= start || i > value.size()) return {};
    std::string u = util::trim(value.substr(start, i - start));
    return strip_quotes(u);
}

std::vector<std::string> split_top_commas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int par = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < s.size()) cur.push_back(s[++i]);
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '(') par++;
        else if (c == ')') par--;
        if (c == ',' && par == 0) { out.push_back(util::trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(util::trim(cur));
    return out;
}

std::vector<BoxShadow> parse_shadows(const std::string& value, float fs) {
    std::vector<BoxShadow> out;
    std::string low = util::to_lower(util::trim(value));
    if (low == "none" || low.empty()) return out;
    for (auto& part : split_top_commas(value)) {
        auto toks = split_ws_paren(part);
        BoxShadow sh;
        std::vector<float> nums;
        bool have_color = false;
        for (auto& tok : toks) {
            std::string t = util::to_lower(tok);
            if (t == "inset") continue;
            bool ok = false;
            Color c = util::parse_color(t, &ok);
            if (ok) { sh.color = c; have_color = true; continue; }
            bool lok = false;
            Length l = parse_length_value(t, fs, 16, false, &lok);
            if (lok) nums.push_back(resolve_length(l, 0, fs, 16, true));
        }
        if (!have_color) sh.color = Color{0, 0, 0, 50};
        if (!nums.empty()) sh.x = nums[0];
        if (nums.size() > 1) sh.y = nums[1];
        if (nums.size() > 2) sh.blur = std::max(0.0f, nums[2]);
        if (nums.size() > 3) sh.spread = nums[3];
        out.push_back(sh);
    }
    return out;
}

Gradient parse_gradient(const std::string& value) {
    Gradient g;
    std::string low = util::to_lower(value);
    size_t p = low.find("linear-gradient(");
    if (p == std::string::npos) return g;
    size_t open = p + std::string("linear-gradient").size();
    int par = 0;
    size_t close = std::string::npos;
    for (size_t i = open; i < value.size(); i++) {
        if (value[i] == '(') par++;
        else if (value[i] == ')') {
            par--;
            if (par == 0) { close = i; break; }
        }
    }
    if (close == std::string::npos) return g;
    std::string inner = value.substr(open + 1, close - open - 1);
    auto parts = split_top_commas(inner);
    if (parts.empty()) return g;
    size_t idx = 0;
    std::string first = util::to_lower(util::trim(parts[0]));
    bool first_is_color = false;
    {
        bool ok = false;
        util::parse_color(first, &ok);
        first_is_color = ok;
    }
    if (!first_is_color) {
        idx = 1;
        if (util::starts_with(first, "to ")) {
            std::string dir = util::trim(first.substr(3));
            if (dir == "top") g.angle = 0;
            else if (dir == "right") g.angle = 90;
            else if (dir == "bottom") g.angle = 180;
            else if (dir == "left") g.angle = 270;
            else if (dir == "top right" || dir == "right top") g.angle = 45;
            else if (dir == "bottom right" || dir == "right bottom") g.angle = 135;
            else if (dir == "bottom left" || dir == "left bottom") g.angle = 225;
            else if (dir == "top left" || dir == "left top") g.angle = 315;
        } else if (util::ends_with(first, "deg")) {
            g.angle = (float)std::atof(first.c_str());
        } else if (util::ends_with(first, "rad")) {
            g.angle = (float)(std::atof(first.c_str()) * 180.0 / 3.14159265358979323846);
        } else if (util::ends_with(first, "turn")) {
            g.angle = (float)(std::atof(first.c_str()) * 360.0);
        }
    }
    bool any_pos = false;
    for (size_t i = idx; i < parts.size(); i++) {
        auto toks = split_ws_paren(parts[i]);
        Color col;
        bool have_color = false;
        float pos = -1;
        for (auto& tok : toks) {
            bool ok = false;
            Color c = util::parse_color(tok, &ok);
            if (ok && !have_color) { col = c; have_color = true; continue; }
            if (util::ends_with(tok, "%")) {
                pos = (float)std::atof(tok.c_str()) / 100.0f;
                any_pos = true;
            }
        }
        if (have_color) g.stops.push_back(GradientStop{pos, col});
    }
    if (g.stops.size() < 2) return Gradient{};
    if (any_pos) {
        if (g.stops.front().pos < 0) g.stops.front().pos = 0;
        if (g.stops.back().pos < 0) g.stops.back().pos = 1;
        for (size_t i = 1; i + 1 < g.stops.size(); i++) {
            if (g.stops[i].pos >= 0) continue;
            size_t prev = i - 1, next = i + 1;
            while (next < g.stops.size() && g.stops[next].pos < 0) next++;
            if (next < g.stops.size()) {
                float a = g.stops[prev].pos, b = g.stops[next].pos;
                g.stops[i].pos = a + (b - a) * (float)(i - prev) / (float)(next - prev);
            } else g.stops[i].pos = g.stops[prev].pos;
        }
    } else {
        for (size_t i = 0; i < g.stops.size(); i++)
            g.stops[i].pos = g.stops.size() == 1 ? 0 : (float)i / (float)(g.stops.size() - 1);
    }
    std::sort(g.stops.begin(), g.stops.end(), [](const GradientStop& a, const GradientStop& b) {
        return a.pos < b.pos;
    });
    g.stops.front().pos = std::max(0.0f, g.stops.front().pos);
    g.stops.back().pos = std::min(1.0f, g.stops.back().pos);
    g.enabled = true;
    return g;
}

void apply_font_shorthand(ComputedStyle& st, const std::string& value, const ComputedStyle* parent) {
    auto toks = split_ws_paren(value);
    if (toks.empty()) return;
    int i = 0;
    int weight = -1;
    FontStyle fs = FontStyle::Normal;
    bool has_style = false;
    while (i < (int)toks.size()) {
        std::string t = util::to_lower(toks[i]);
        if (t == "italic" || t == "oblique") { fs = FontStyle::Italic; has_style = true; i++; }
        else if (t == "normal") { i++; }
        else if (t == "bold" || t == "bolder" || t == "lighter") {
            weight = t == "bold" ? 700 : t == "lighter" ? 300 : 700;
            i++;
        } else if (!t.empty() &&
                   std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
            int w = (int)parse_float_value(t, 400);
            if (w >= 100 && w <= 900) { weight = w; i++; }
            else break;
        } else break;
    }
    if (i >= (int)toks.size()) return;
    std::string size_tok = toks[i];
    std::string line_h;
    size_t slash = size_tok.find('/');
    if (slash != std::string::npos) {
        std::string lh = size_tok.substr(slash + 1);
        size_tok = size_tok.substr(0, slash);
        line_h = util::to_lower(lh);
    }
    float psize = parent ? parent->font_size : 16;
    if (util::ends_with(size_tok, "%")) st.font_size = psize * parse_float_value(size_tok) / 100.0f;
    else if (util::ends_with(size_tok, "rem")) st.font_size = 16 * parse_float_value(size_tok);
    else if (util::ends_with(size_tok, "em")) st.font_size = psize * parse_float_value(size_tok);
    else if (util::ends_with(size_tok, "px")) st.font_size = parse_float_value(size_tok);
    else st.font_size = parse_float_value(size_tok, psize);
    i++;
    st.line_height = Length{};
    if (!line_h.empty() && line_h != "normal") {
        if (util::ends_with(line_h, "%")) st.line_height.unit = Length::Percent;
        else if (util::ends_with(line_h, "px")) st.line_height.unit = Length::Px;
        else if (util::ends_with(line_h, "rem")) { st.line_height.unit = Length::Px; }
        else st.line_height.unit = Length::Em;
        st.line_height.value = parse_float_value(line_h);
        if (util::ends_with(line_h, "rem")) st.line_height.value *= 16;
    }
    if (weight > 0) st.font_weight = weight;
    if (has_style) st.font_style = fs;
    std::string fam;
    for (; i < (int)toks.size(); i++) {
        if (!fam.empty()) fam += " ";
        fam += toks[i];
    }
    size_t comma = fam.find(',');
    if (comma != std::string::npos) fam = fam.substr(0, comma);
    st.font_family = util::to_lower(strip_quotes(util::trim(fam)));
}

void apply_property(ComputedStyle& st, const std::string& prop, const std::string& value,
                    const ComputedStyle* parent, Node* node) {
    std::string p = prop;
    std::string v = util::trim(value);
    std::string vl = util::to_lower(v);
    if (apply_global_keyword(st, p, v, parent)) return;

    auto len = [&](float basis_font, bool auto_ok) { return parse_length_value(v, basis_font, 16, auto_ok); };

    if (p == "display") st.display = parse_display(vl);
    else if (p == "position") {
        if (vl == "relative" || vl == "sticky") st.position = Position::Relative;
        else if (vl == "absolute") st.position = Position::Absolute;
        else if (vl == "fixed") st.position = Position::Fixed;
        else st.position = Position::Static;
    } else if (p == "width") st.width = len(st.font_size, true);
    else if (p == "height") st.height = len(st.font_size, true);
    else if (p == "min-width") st.min_width = len(st.font_size, false);
    else if (p == "min-height") st.min_height = len(st.font_size, false);
    else if (p == "max-width") {
        if (vl == "none") st.max_width = Length{};
        else st.max_width = len(st.font_size, false);
    } else if (p == "max-height") {
        if (vl == "none") st.max_height = Length{};
        else st.max_height = len(st.font_size, false);
    } else if (p == "margin") apply_four(st.margin, v, st.font_size, 16, true);
    else if (p == "padding") apply_four(st.padding, v, st.font_size, 16, false);
    else if (p == "margin-top") st.margin.top = len(st.font_size, true);
    else if (p == "margin-right") st.margin.right = len(st.font_size, true);
    else if (p == "margin-bottom") st.margin.bottom = len(st.font_size, true);
    else if (p == "margin-left") st.margin.left = len(st.font_size, true);
    else if (p == "padding-top") st.padding.top = len(st.font_size, false);
    else if (p == "padding-right") st.padding.right = len(st.font_size, false);
    else if (p == "padding-bottom") st.padding.bottom = len(st.font_size, false);
    else if (p == "padding-left") st.padding.left = len(st.font_size, false);
    else if (p == "color") {
        bool ok = false;
        Color c = vl == "currentcolor" ? st.color : util::parse_color(vl, &ok);
        if (ok || vl == "currentcolor") st.color = c;
    } else if (p == "background-color") {
        bool ok = false;
        Color c = util::parse_color(vl, &ok);
        if (ok) st.background = c;
    } else if (p == "background") {
        if (vl == "none") {
            st.background = Color{0, 0, 0, 0};
            st.background_gradient = Gradient{};
            st.background_image.clear();
            st.background_size = BackgroundSize::Auto;
        } else {
            if (vl.find("gradient(") != std::string::npos) {
                Gradient g = parse_gradient(v);
                if (g.enabled) st.background_gradient = std::move(g);
            }
            std::string url = extract_css_url(v);
            if (!url.empty()) st.background_image = url;
            if (vl.find("cover") != std::string::npos) st.background_size = BackgroundSize::Cover;
            else if (vl.find("contain") != std::string::npos) st.background_size = BackgroundSize::Contain;
            else if (vl.find("100% 100%") != std::string::npos) st.background_size = BackgroundSize::Stretch;
            bool ok = false;
            Color c = util::parse_color(vl, &ok);
            if (ok) st.background = c;
            else {
                for (auto& tok : split_ws_paren(vl)) {
                    Color tc = util::parse_color(tok, &ok);
                    if (ok) { st.background = tc; break; }
                }
            }
        }
    } else if (p == "background-image") {
        if (vl == "none") {
            st.background_gradient = Gradient{};
            st.background_image.clear();
        } else {
            Gradient g = parse_gradient(v);
            if (g.enabled) {
                st.background_gradient = std::move(g);
                st.background_image.clear();
            } else {
                std::string url = extract_css_url(v);
                if (!url.empty()) st.background_image = url;
            }
        }
    } else if (p == "background-size") {
        if (vl.find("cover") != std::string::npos) st.background_size = BackgroundSize::Cover;
        else if (vl.find("contain") != std::string::npos) st.background_size = BackgroundSize::Contain;
        else if (vl.find("100% 100%") != std::string::npos) st.background_size = BackgroundSize::Stretch;
        else st.background_size = BackgroundSize::Auto;
    } else if (p == "box-shadow") {
        st.shadows = parse_shadows(v, st.font_size);
    } else if (p == "font-size") {
        if (vl == "xx-small") st.font_size = 9;
        else if (vl == "x-small") st.font_size = 10;
        else if (vl == "small") st.font_size = 13;
        else if (vl == "medium") st.font_size = 16;
        else if (vl == "large") st.font_size = 18;
        else if (vl == "x-large") st.font_size = 24;
        else if (vl == "xx-large") st.font_size = 32;
        else if (vl == "xxx-large") st.font_size = 48;
        else if (vl == "smaller") st.font_size = (parent ? parent->font_size : 16) * 0.833f;
        else if (vl == "larger") st.font_size = (parent ? parent->font_size : 16) * 1.2f;
        else {
            float base = parent ? parent->font_size : 16;
            if (util::ends_with(vl, "%")) st.font_size = base * parse_float_value(vl) / 100.0f;
            else if (util::ends_with(vl, "em")) st.font_size = base * parse_float_value(vl);
            else if (util::ends_with(vl, "rem")) st.font_size = 16 * parse_float_value(vl);
            else if (util::ends_with(vl, "px")) st.font_size = parse_float_value(vl);
            else {
                float x = parse_float_value(vl, st.font_size);
                if (x > 0) st.font_size = x;
            }
        }
    } else if (p == "font-family") {
        size_t comma = v.find(',');
        std::string fam = comma == std::string::npos ? v : v.substr(0, comma);
        fam = util::to_lower(strip_quotes(util::trim(fam)));
        if (!fam.empty()) st.font_family = fam;
    } else if (p == "font-weight") {
        if (vl == "normal") st.font_weight = 400;
        else if (vl == "bold") st.font_weight = 700;
        else if (vl == "bolder") st.font_weight = std::min(900, (parent ? parent->font_weight : 400) + 300);
        else if (vl == "lighter") st.font_weight = std::max(100, (parent ? parent->font_weight : 400) - 300);
        else {
            int w = (int)parse_float_value(vl, 400);
            if (w >= 100 && w <= 900) st.font_weight = w;
        }
    } else if (p == "font-style") {
        st.font_style = (vl == "italic" || vl == "oblique") ? FontStyle::Italic : FontStyle::Normal;
    } else if (p == "line-height") {
        if (vl == "normal") st.line_height = Length{};
        else if (util::ends_with(vl, "%")) { st.line_height.unit = Length::Percent; st.line_height.value = parse_float_value(vl); }
        else if (util::ends_with(vl, "px")) { st.line_height.unit = Length::Px; st.line_height.value = parse_float_value(vl); }
        else if (util::ends_with(vl, "em")) { st.line_height.unit = Length::Em; st.line_height.value = parse_float_value(vl); }
        else { st.line_height.unit = Length::Em; st.line_height.value = parse_float_value(vl, 1.2f); }
    } else if (p == "font") {
        std::string fv = util::to_lower(util::trim(v));
        if (fv == "inherit" && parent) {
            st.font_family = parent->font_family;
            st.font_size = parent->font_size;
            st.font_weight = parent->font_weight;
            st.font_style = parent->font_style;
            st.line_height = parent->line_height;
        } else if (fv == "initial" || fv == "unset" || fv == "revert") {
            st.font_family.clear();
            st.font_size = 16;
            st.font_weight = 400;
            st.font_style = FontStyle::Normal;
            st.line_height = Length{};
        } else {
            apply_font_shorthand(st, v, parent);
        }
    }
    else if (p == "text-align") {
        if (vl == "center") st.text_align = TextAlign::Center;
        else if (vl == "right" || vl == "end") st.text_align = TextAlign::Right;
        else if (vl == "justify") st.text_align = TextAlign::Justify;
        else st.text_align = TextAlign::Left;
    } else if (p == "white-space") {
        if (vl == "pre") st.white_space = WhiteSpace::Pre;
        else if (vl == "nowrap") st.white_space = WhiteSpace::Nowrap;
        else if (vl == "pre-wrap" || vl == "break-spaces") st.white_space = WhiteSpace::PreWrap;
        else st.white_space = WhiteSpace::Normal;
    } else if (p == "text-decoration" || p == "text-decoration-line") {
        st.underline = vl.find("underline") != std::string::npos;
        st.line_through = vl.find("line-through") != std::string::npos;
    } else if (p == "text-transform") {
        if (vl == "uppercase") st.text_transform = TextTransform::Uppercase;
        else if (vl == "lowercase") st.text_transform = TextTransform::Lowercase;
        else if (vl == "capitalize") st.text_transform = TextTransform::Capitalize;
        else st.text_transform = TextTransform::None;
    } else if (p == "letter-spacing") {
        if (vl == "normal") st.letter_spacing = 0;
        else {
            bool ok = false;
            Length l = parse_length_value(vl, st.font_size, 16, false, &ok);
            if (ok) st.letter_spacing = l.unit == Length::Percent ? 0 : resolve_length(l, 0, st.font_size, 16, true);
        }
    } else if (p == "visibility") {
        if (vl == "hidden") st.visibility = Visibility::Hidden;
        else if (vl == "collapse") st.visibility = Visibility::Collapse;
        else st.visibility = Visibility::Visible;
    } else if (p == "opacity") {
        float o = util::ends_with(vl, "%") ? parse_float_value(vl) / 100.0f : parse_float_value(vl, 1);
        st.opacity = std::clamp(o, 0.0f, 1.0f);
    } else if (p == "overflow" || p == "overflow-x" || p == "overflow-y") {
        if (vl == "hidden" || vl == "auto" || vl == "scroll" || vl == "clip") st.overflow = Overflow::Hidden;
        else if (vl == "visible") st.overflow = Overflow::Visible;
    } else if (p == "box-sizing") st.box_sizing_border = vl == "border-box";
    else if (p == "border-radius") {
        auto toks = split_ws_paren(v);
        if (!toks.empty()) {
            bool ok = false;
            Length l = parse_length_value(toks[0], st.font_size, 16, false, &ok);
            if (ok) st.border_radius = l.unit == Length::Percent ? 0 : resolve_length(l, 0, st.font_size, 16, true);
        }
    } else if (p == "border" || p == "border-top" || p == "border-right" || p == "border-bottom" ||
               p == "border-left") {
        BorderParse b = parse_border_tokens(v, st);
        if (!b.valid) return;
        int idx = p == "border-top" ? 0 : p == "border-right" ? 1 : p == "border-bottom" ? 2 : p == "border-left" ? 3 : -1;
        // Shorthands reset omitted components; longhands retain their specified values.
        if (idx >= 0) { st.border[idx] = BorderSide{}; apply_border_side(st.border[idx], b, true, true, true); }
        else for (auto& side : st.border) { side = BorderSide{}; apply_border_side(side, b, true, true, true); }
    } else if (p == "border-width" || p == "border-style" || p == "border-color") {
        auto toks = split_ws_paren(v);
        if (toks.empty()) return;
        std::vector<std::string> vals(4);
        if (toks.size() == 1) vals = {toks[0], toks[0], toks[0], toks[0]};
        else if (toks.size() == 2) vals = {toks[0], toks[1], toks[0], toks[1]};
        else if (toks.size() == 3) vals = {toks[0], toks[1], toks[2], toks[1]};
        else vals = {toks[0], toks[1], toks[2], toks[3]};
        for (int i = 0; i < 4; i++) {
            const auto parsed = parse_border_tokens(vals[i], st);
            apply_border_side(st.border[i], parsed, p == "border-width", p == "border-style", p == "border-color");
        }
    } else if (util::starts_with(p, "border-") && util::ends_with(p, "-width")) {
        int idx = p == "border-top-width" ? 0 : p == "border-right-width" ? 1 : p == "border-bottom-width" ? 2 : 3;
        apply_border_side(st.border[idx], parse_border_tokens(vl, st), true, false, false);
    } else if (util::starts_with(p, "border-") && util::ends_with(p, "-style")) {
        int idx = p == "border-top-style" ? 0 : p == "border-right-style" ? 1 : p == "border-bottom-style" ? 2 : 3;
        apply_border_side(st.border[idx], parse_border_tokens(vl, st), false, true, false);
    } else if (util::starts_with(p, "border-") && util::ends_with(p, "-color")) {
        int idx = p == "border-top-color" ? 0 : p == "border-right-color" ? 1 : p == "border-bottom-color" ? 2 : 3;
        apply_border_side(st.border[idx], parse_border_tokens(vl, st), false, false, true);
    } else if (p == "flex") {
        if (vl == "none") { st.flex_grow = 0; st.flex_shrink = 0; st.flex_basis = Length{}; }
        else if (vl == "auto") { st.flex_grow = 1; st.flex_shrink = 1; st.flex_basis = Length{}; }
        else {
            auto toks = split_ws_paren(vl);
            if (!toks.empty()) {
                st.flex_grow = parse_float_value(toks[0], 0);
                st.flex_shrink = toks.size() > 1 ? parse_float_value(toks[1], 1) : 1;
                if (toks.size() > 2) st.flex_basis = parse_length_value(toks[2], st.font_size, 16, true);
                else st.flex_basis = Length::percent(0);
            }
        }
    } else if (p == "aspect-ratio") {
        std::string s = util::trim(vl);
        if (util::starts_with(s, "auto")) {
            size_t sp = s.find(' ');
            s = sp == std::string::npos ? "" : util::trim(s.substr(sp + 1));
        }
        if (!s.empty() && s != "auto") {
            size_t slash = s.find('/');
            float a = slash == std::string::npos ? parse_float_value(s)
                                                 : parse_float_value(util::trim(s.substr(0, slash)));
            float b = slash == std::string::npos ? 1.0f : parse_float_value(util::trim(s.substr(slash + 1)), 1);
            if (a > 0 && b > 0) st.aspect_ratio = a / b;
        }
    } else if (p == "flex-grow") st.flex_grow = std::max(0.0f, parse_float_value(vl, 0));
    else if (p == "flex-shrink") st.flex_shrink = std::max(0.0f, parse_float_value(vl, 1));
    else if (p == "flex-basis") st.flex_basis = parse_length_value(vl, st.font_size, 16, true);
    else if (p == "flex-direction") {
        if (vl == "row-reverse") st.flex_direction = FlexDirection::RowReverse;
        else if (vl == "column") st.flex_direction = FlexDirection::Column;
        else if (vl == "column-reverse") st.flex_direction = FlexDirection::ColumnReverse;
        else st.flex_direction = FlexDirection::Row;
    } else if (p == "flex-wrap") {
        if (vl == "wrap") st.flex_wrap = FlexWrap::Wrap;
        else if (vl == "wrap-reverse") st.flex_wrap = FlexWrap::WrapReverse;
        else st.flex_wrap = FlexWrap::NoWrap;
    } else if (p == "flex-flow") {
        auto toks = split_ws_paren(vl);
        for (auto& t : toks) {
            if (t == "row" || t == "row-reverse" || t == "column" || t == "column-reverse")
                apply_property(st, "flex-direction", t, parent, node);
            else if (t == "wrap" || t == "nowrap" || t == "wrap-reverse")
                apply_property(st, "flex-wrap", t, parent, node);
        }
    } else if (p == "grid-template-columns") {
        st.grid_columns = parse_grid_tracks(vl, st.font_size);
    } else if (p == "grid-template-rows") {
        st.grid_rows = parse_grid_tracks(vl, st.font_size);
    } else if (p == "grid-gap" || p == "grid-row-gap" || p == "grid-column-gap") {
        auto toks = split_ws_paren(vl);
        auto gap_len = [&](const std::string& s) {
            bool ok = false;
            Length l = parse_length_value(s, st.font_size, 16, false, &ok);
            return ok ? resolve_length(l, 0, st.font_size, 16, true) : 0.0f;
        };
        float rv = toks.empty() ? 0 : gap_len(toks[0]);
        if (p == "grid-row-gap") st.row_gap = rv;
        else if (p == "grid-column-gap") st.column_gap = rv;
        else if (toks.size() == 1) { st.row_gap = st.column_gap = rv; }
        else if (toks.size() >= 2) {
            st.row_gap = gap_len(toks[0]);
            st.column_gap = gap_len(toks[1]);
        }
    } else if (p == "justify-content") st.justify = parse_justify(vl);
    else if (p == "align-items") st.align_items = parse_align(vl);
    else if (p == "gap") {
        auto toks = split_ws_paren(vl);
        auto gap_len = [&](const std::string& s) {
            bool ok = false;
            Length l = parse_length_value(s, st.font_size, 16, false, &ok);
            return ok ? resolve_length(l, 0, st.font_size, 16, true) : 0.0f;
        };
        if (toks.size() == 1) {
            st.row_gap = gap_len(toks[0]);
            st.column_gap = st.row_gap;
        } else if (toks.size() >= 2) {
            st.row_gap = gap_len(toks[0]);
            st.column_gap = gap_len(toks[1]);
        }
    } else if (p == "row-gap") {
        bool ok = false;
        Length l = parse_length_value(vl, st.font_size, 16, false, &ok);
        if (ok) st.row_gap = resolve_length(l, 0, st.font_size, 16, true);
    } else if (p == "column-gap") {
        bool ok = false;
        Length l = parse_length_value(vl, st.font_size, 16, false, &ok);
        if (ok) st.column_gap = resolve_length(l, 0, st.font_size, 16, true);
    }
    else if (p == "order") st.order = (int)parse_float_value(vl, 0);
    else if (p == "grid-column") {auto value=util::split_ws(vl);if(value.size()==2&&value[0]=="span")st.grid_column_span=std::clamp((int)parse_float_value(value[1],1),1,32);}
    else if(p=="transform") {
        st.translate_x=Length::px(0);st.translate_y=Length::px(0);
        auto open=vl.find('('),close=vl.rfind(')');if(open!=std::string::npos&&close==vl.size()-1&&vl.find(')',open)==close){
            const auto name=vl.substr(0,open);auto values=util::split(vl.substr(open+1,close-open-1),',');if(values.size()==1)values=util::split_ws(values[0]);
            if(name=="translate"&&!values.empty()&&values.size()<=2){st.translate_x=parse_length_value(values[0],st.font_size,16,false);if(values.size()==2)st.translate_y=parse_length_value(values[1],st.font_size,16,false);}
            else if(name=="translatex"&&values.size()==1)st.translate_x=parse_length_value(values[0],st.font_size,16,false);
            else if(name=="translatey"&&values.size()==1)st.translate_y=parse_length_value(values[0],st.font_size,16,false);
        }
    }
    else if (p == "z-index") st.z_index = (int)parse_float_value(vl, 0);
    else if (p == "top") st.top = len(st.font_size, true);
    else if (p == "right") st.right = len(st.font_size, true);
    else if (p == "bottom") st.bottom = len(st.font_size, true);
    else if (p == "left") st.left = len(st.font_size, true);
    else if (p == "inset") {
        auto toks = split_ws_paren(vl);
        if (!toks.empty()) {
            st.top = st.right = st.bottom = st.left = parse_length_value(toks[0], st.font_size, 16, true);
            if (toks.size() >= 2) { st.right = st.left = parse_length_value(toks[1], st.font_size, 16, true); }
            if (toks.size() >= 3) st.bottom = parse_length_value(toks[2], st.font_size, 16, true);
            if (toks.size() >= 4) st.left = parse_length_value(toks[3], st.font_size, 16, true);
        }
    } else if (p == "object-fit") {
        if (vl == "contain") st.object_fit = ObjectFit::Contain;
        else if (vl == "cover") st.object_fit = ObjectFit::Cover;
        else if (vl == "none") st.object_fit = ObjectFit::None;
        else if (vl == "scale-down") st.object_fit = ObjectFit::ScaleDown;
        else st.object_fit = ObjectFit::Fill;
    } else if (p == "hidden") {
        st.display = Display::None;
    }
    (void)node;
}

std::vector<CssDeclaration> parse_inline_style(const std::string& text) {
    std::vector<CssDeclaration> out;
    std::string cur;
    int par = 0;
    char quote = 0;
    auto flush = [&]() {
        std::string d = util::trim(cur);
        cur.clear();
        if (d.empty()) return;
        size_t colon = d.find(':');
        if (colon == std::string::npos) return;
        std::string name = util::trim(d.substr(0, colon));
        std::string value = util::trim(d.substr(colon + 1));
        if (name.empty() || value.empty()) return;
        CssDeclaration cd;
        cd.name = util::starts_with(name, "--") ? name : util::to_lower(name);
        cd.value = value;
        std::string lower = util::to_lower(value);
        size_t ip = lower.rfind("!important");
        if (ip != std::string::npos && util::trim(lower.substr(ip + 10)).empty()) {
            cd.important = true;
            cd.value = util::trim(value.substr(0, ip));
        }
        out.push_back(std::move(cd));
    };
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < text.size()) { cur.push_back(text[i + 1]); i++; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '(') par++;
        if (c == ')') par--;
        if (c == ';' && par == 0) flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

std::string substitute_vars(const std::string& input, const std::unordered_map<std::string, std::string>& props,
                            int depth = 0) {
    if (depth > 8) return input;
    std::string out;
    size_t i = 0;
    while (i < input.size()) {
        size_t vpos = input.find("var(", i);
        if (vpos == std::string::npos) { out += input.substr(i); break; }
        out += input.substr(i, vpos - i);
        size_t open = vpos + 3;
        size_t close = std::string::npos;
        int par = 1;
        char quote = 0;
        for (size_t j = open + 1; j < input.size(); j++) {
            char c = input[j];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '(') par++;
            else if (c == ')') {
                par--;
                if (par == 0) { close = j; break; }
            }
        }
        if (close == std::string::npos) { out += input.substr(vpos); break; }
        std::string body = input.substr(open + 1, close - open - 1);
        std::string name = body;
        std::string fallback;
        size_t comma = std::string::npos;
        int p = 0;
        for (size_t k = 0; k < body.size(); k++) {
            if (body[k] == '(') p++;
            if (body[k] == ')') p--;
            if (body[k] == ',' && p == 0) { comma = k; break; }
        }
        if (comma != std::string::npos) {
            name = util::trim(body.substr(0, comma));
            fallback = util::trim(body.substr(comma + 1));
        } else name = util::trim(name);
        std::string rep;
        auto it = props.find(name);
        if (it != props.end()) rep = it->second;
        else if (!fallback.empty()) rep = fallback;
        out += substitute_vars(rep, props, depth + 1);
        i = close + 1;
    }
    return out;
}

} // namespace

void apply_cascade(Document& doc, const std::vector<Stylesheet>& sheets, int viewport_w, int viewport_h) {
    g_viewport_w = (float)viewport_w;
    g_viewport_h = (float)viewport_h;
    static Stylesheet ua;
    static bool ua_ready = false;
    if (!ua_ready) {
        int order = -100000;
        ua = parse_css(kUserAgentCss, "ua://miniweb.css", 1280, 800, &order);
        ua_ready = true;
    }

    std::function<void(Node*, Node*)> resolve = [&](Node* node, Node* parent) {
        if (!node) return;
        if (!node->is_element()) {
            if (parent) node->style = parent->style;
            for (auto* c : node->children) resolve(c, node);
            return;
        }
        ComputedStyle st = parent ? inherited_style(parent->style) : ComputedStyle{};
        st.margin = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)};
        st.padding = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)};
        st.is_root = node == doc.html;

        std::vector<DeclItem> items;
        auto add_rule_decls = [&](const CssRule& rule, int spec_id, int spec_class, int spec_tag) {
            for (size_t k = 0; k < rule.declarations.size(); k++) {
                DeclItem it;
                it.decl = &rule.declarations[k];
                it.name = rule.declarations[k].name;
                it.value = rule.declarations[k].value;
                it.important = rule.declarations[k].important;
                it.spec_id = spec_id;
                it.spec_class = spec_class;
                it.spec_tag = spec_tag;
                it.order = rule.order;
                it.index = (int)k;
                items.push_back(std::move(it));
            }
        };

        auto consider_sheet = [&](const Stylesheet& sheet) {
            for (auto& rule : sheet.rules) {
                bool matched = false;
                int bi = -1, bc = -1, bt = -1;
                for (auto& sel : rule.selectors) {
                    if (css_matches_selector(node, sel, &doc)) {
                        matched = true;
                        if (sel.spec_id > bi) { bi = sel.spec_id; bc = sel.spec_class; bt = sel.spec_tag; }
                        else if (sel.spec_id == bi && sel.spec_class > bc) { bc = sel.spec_class; bt = sel.spec_tag; }
                        else if (sel.spec_id == bi && sel.spec_class == bc && sel.spec_tag > bt) { bt = sel.spec_tag; }
                    }
                }
                if (matched) add_rule_decls(rule, bi, bc, bt);
            }
        };
        consider_sheet(ua);
        for (auto& sheet : sheets) consider_sheet(sheet);

        auto inline_decls = parse_inline_style(node->attr("style"));
        for (size_t k = 0; k < inline_decls.size(); k++) {
            DeclItem it;
            it.decl = nullptr;
            it.name = inline_decls[k].name;
            it.value = inline_decls[k].value;
            it.important = inline_decls[k].important;
            it.inline_style = true;
            it.spec_id = 1000;
            it.spec_class = 0;
            it.spec_tag = 0;
            it.order = INT_MAX / 2;
            it.index = (int)k;
            items.push_back(std::move(it));
        }

        std::stable_sort(items.begin(), items.end(), [](const DeclItem& a, const DeclItem& b) {
            if (a.important != b.important) return !a.important;
            if (a.inline_style != b.inline_style) return !a.inline_style;
            if (a.spec_id != b.spec_id) return a.spec_id < b.spec_id;
            if (a.spec_class != b.spec_class) return a.spec_class < b.spec_class;
            if (a.spec_tag != b.spec_tag) return a.spec_tag < b.spec_tag;
            if (a.order != b.order) return a.order < b.order;
            return a.index < b.index;
        });

        // Custom properties cascade independently and are inherited.
        for (auto& it : items) {
            if (util::starts_with(it.name, "--")) st.custom_props[it.name] = util::trim(it.value);
        }
        for (int pass = 0; pass < 8; pass++) {
            bool changed = false;
            for (auto& kv : st.custom_props) {
                std::string nv = substitute_vars(kv.second, st.custom_props);
                if (nv != kv.second) { kv.second = nv; changed = true; }
            }
            if (!changed) break;
        }

        for (auto& it : items) {
            if (util::starts_with(it.name, "--")) continue;
            std::string val = substitute_vars(it.value, st.custom_props);
            apply_property(st, it.name, val, parent ? &parent->style : nullptr, node);
        }

        for (auto& side : st.border) if (side.current_color) side.color = st.color;
        if (node->has_attr("hidden")) st.display = Display::None;
        st.display_none = st.display == Display::None;

        node->style = st;
        for (auto* c : node->children) resolve(c, node);
    };

    resolve(doc.root, nullptr);
}
