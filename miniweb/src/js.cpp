#include "js.h"
#include "css.h"
#include "html.h"
#include "js_prelude.h"
#include "quickjs.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

struct JsRuntime::Bridge {
    Document* doc = nullptr;
    Loader* loader = nullptr;
    JsRuntime* owner = nullptr;
    int vw = 1280, vh = 800;
};

namespace {

char* module_normalize_cb(JSContext* ctx, const char* base_name, const char* name, void* opaque) {
    auto* b = static_cast<JsRuntime::Bridge*>(opaque);
    if (!b || !b->loader || !name) return nullptr;
    std::string base = (base_name && util::url_scheme(base_name).size()) ? base_name : b->loader->base_url();
    std::string resolved;
    if (util::url_scheme(name).empty()) {
        if (base.empty()) resolved = name;
        else resolved = util::resolve_url(base, name);
    } else {
        resolved = name;
    }
    return js_strdup(ctx, resolved.c_str());
}

JSModuleDef* module_load_cb(JSContext* ctx, const char* module_name, void* opaque) {
    auto* b = static_cast<JsRuntime::Bridge*>(opaque);
    if (!b || !b->loader) {
        JS_ThrowTypeError(ctx, "module loader unavailable");
        return nullptr;
    }
    LoadResult r = b->loader->load_absolute(module_name);
    if (!r.ok) {
        JS_ThrowTypeError(ctx, "module load failed (%s): %s", module_name, r.error.c_str());
        return nullptr;
    }
    JSValue v = JS_Eval(ctx, r.data.data(), r.data.size(), module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        util::error("ES module compile error (%s): %s", module_name, s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        return nullptr;
    }
    auto* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(v));
    JS_FreeValue(ctx, v);
    return m;
}


struct StyleDecl {
    std::string name;
    std::string value;
};

std::vector<StyleDecl> parse_style_decls(const std::string& css) {
    std::vector<StyleDecl> out;
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
        if (name.empty()) return;
        if (!util::starts_with(name, "--")) name = util::to_lower(name);
        out.push_back({name, value});
    };
    for (size_t i = 0; i < css.size(); i++) {
        char c = css[i];
        if (quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < css.size()) cur.push_back(css[++i]);
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur.push_back(c); continue; }
        if (c == '(') par++;
        else if (c == ')') par--;
        if (c == ';' && par == 0) flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

std::string serialize_style(const std::vector<StyleDecl>& decls) {
    std::string out;
    for (auto& d : decls) {
        if (d.value.empty()) continue;
        out += d.name;
        out += ": ";
        out += d.value;
        out += "; ";
    }
    return out;
}

std::string get_style_prop(const std::string& css, const std::string& prop) {
    auto decls = parse_style_decls(css);
    std::string p = util::starts_with(prop, "--") ? prop : util::to_lower(prop);
    for (auto it = decls.rbegin(); it != decls.rend(); ++it) {
        if (it->name == p) return it->value;
    }
    return {};
}

void set_style_prop(std::string& css, const std::string& prop, const std::string& value) {
    auto decls = parse_style_decls(css);
    std::string p = util::starts_with(prop, "--") ? prop : util::to_lower(prop);
    std::vector<StyleDecl> out;
    bool set = false;
    for (auto& d : decls) {
        if (d.name != p) { out.push_back(d); continue; }
        if (value.empty()) continue;
        if (!set) { out.push_back({p, value}); set = true; }
        else out.push_back(d);
    }
    if (!value.empty() && !set) out.push_back({p, value});
    css = serialize_style(out);
}

JsRuntime::Bridge* bridge(JSContext* ctx) {
    return static_cast<JsRuntime::Bridge*>(JS_GetContextOpaque(ctx));
}

Node* node_of(JSContext* ctx, int id) {
    auto* b = bridge(ctx);
    if (!b || !b->doc || id <= 0 || id >= (int)b->doc->id_map.size()) return nullptr;
    return b->doc->id_map[id];
}

int id_of(Node* n) { return n ? n->id : 0; }

int arg_i32(JSContext* ctx, JSValueConst v, int def = 0) {
    int32_t x = def;
    JS_ToInt32(ctx, &x, v);
    return x;
}

std::string arg_str(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    if (!s) return {};
    std::string out(s);
    JS_FreeCString(ctx, s);
    return out;
}

JSValue str_val(JSContext* ctx, const std::string& s) {
    return JS_NewStringLen(ctx, s.data(), s.size());
}

JSValue array_of_ids(JSContext* ctx, const std::vector<int>& ids) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ids.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
    return arr;
}

std::string color_css(Color c) {
    if (c.a == 255) return "rgb(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
    return "rgba(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ", " +
           util::format_float(c.a / 255.0f, 3) + ")";
}

std::string display_css(Display d) {
    switch (d) {
        case Display::Block: return "block";
        case Display::Inline: return "inline";
        case Display::InlineBlock: return "inline-block";
        case Display::Flex: return "flex";
        case Display::Grid: return "grid";
        case Display::None: return "none";
    }
    return "inline";
}

std::string position_css(Position p) {
    switch (p) {
        case Position::Static: return "static";
        case Position::Relative: return "relative";
        case Position::Absolute: return "absolute";
        case Position::Fixed: return "fixed";
    }
    return "static";
}

std::string justify_css(Justify j) {
    switch (j) {
        case Justify::FlexStart: return "flex-start";
        case Justify::FlexEnd: return "flex-end";
        case Justify::Center: return "center";
        case Justify::SpaceBetween: return "space-between";
        case Justify::SpaceAround: return "space-around";
        case Justify::SpaceEvenly: return "space-evenly";
    }
    return "flex-start";
}

std::string align_css(AlignItems a) {
    switch (a) {
        case AlignItems::Stretch: return "stretch";
        case AlignItems::FlexStart: return "flex-start";
        case AlignItems::FlexEnd: return "flex-end";
        case AlignItems::Center: return "center";
        case AlignItems::Baseline: return "baseline";
    }
    return "stretch";
}

std::string prop_css(const Node* n, const std::string& prop) {
    const ComputedStyle& st = n->style;
    if (prop == "display") return display_css(st.display);
    if (prop == "position") return position_css(st.position);
    if (prop == "color") return color_css(st.color);
    if (prop == "background-color") return color_css(st.background);
    if (prop == "font-size") return util::format_float(st.font_size, 2) + "px";
    if (prop == "font-family") return st.font_family.empty() ? "sans-serif" : st.font_family;
    if (prop == "font-weight") return std::to_string(st.font_weight);
    if (prop == "font-style") return st.font_style == FontStyle::Normal ? "normal" : "italic";
    if (prop == "line-height") return util::format_float(style_line_height(st), 2) + "px";
    if (prop == "width") return util::format_float(n->layout.has_box ? n->layout.content_w : 0, 2) + "px";
    if (prop == "height") return util::format_float(n->layout.has_box ? n->layout.content_h : 0, 2) + "px";
    if (prop == "opacity") return util::format_float(st.opacity, 3);
    if (prop == "visibility") return st.visibility == Visibility::Visible ? "visible" : "hidden";
    if (prop == "overflow") return st.overflow == Overflow::Visible ? "visible" : "hidden";
    if (prop == "z-index") return std::to_string(st.z_index);
    if (prop == "flex-direction") {
        switch (st.flex_direction) {
            case FlexDirection::Row: return "row";
            case FlexDirection::RowReverse: return "row-reverse";
            case FlexDirection::Column: return "column";
            case FlexDirection::ColumnReverse: return "column-reverse";
        }
    }
    if (prop == "justify-content") return justify_css(st.justify);
    if (prop == "align-items") return align_css(st.align_items);
    if (prop == "gap") {
        if (st.row_gap == st.column_gap) return util::format_float(st.row_gap, 2) + "px";
        return util::format_float(st.row_gap, 2) + "px " + util::format_float(st.column_gap, 2) + "px";
    }
    if (prop == "text-align") {
        switch (st.text_align) {
            case TextAlign::Left: return "left";
            case TextAlign::Right: return "right";
            case TextAlign::Center: return "center";
            case TextAlign::Justify: return "justify";
        }
    }
    if (prop == "white-space") {
        switch (st.white_space) {
            case WhiteSpace::Normal: return "normal";
            case WhiteSpace::Pre: return "pre";
            case WhiteSpace::Nowrap: return "nowrap";
            case WhiteSpace::PreWrap: return "pre-wrap";
        }
    }
    if (prop == "border-top-width") return util::format_float(border_top(st), 2) + "px";
    if (prop == "border-right-width") return util::format_float(border_right(st), 2) + "px";
    if (prop == "border-bottom-width") return util::format_float(border_bottom(st), 2) + "px";
    if (prop == "border-left-width") return util::format_float(border_left(st), 2) + "px";
    if (prop == "transform") return "none";
    if (prop == "box-sizing") return st.box_sizing_border ? "border-box" : "content-box";
    return {};
}

Node* clone_node(Document& doc, Node* n, bool deep) {
    if (!n) return nullptr;
    Node* c = doc.alloc(n->type);
    c->tag = n->tag;
    c->data = n->data;
    c->attrs = n->attrs;
    if (deep) {
        for (auto* ch : n->children) {
            Node* cc = clone_node(doc, ch, true);
            if (cc) c->append_child(cc);
        }
    }
    return c;
}

JSValue js_node_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return JS_NewInt32(ctx, n ? (int)n->type : 0);
}

JSValue js_node_name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return str_val(ctx, "");
    if (n->type == NodeType::Text) return str_val(ctx, "#text");
    if (n->type == NodeType::Comment) return str_val(ctx, "#comment");
    if (n->type == NodeType::Document) return str_val(ctx, "#document");
    return str_val(ctx, util::to_upper(n->tag));
}

JSValue js_tag_name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->tag : "");
}

JSValue js_parent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return JS_NewInt32(ctx, n && n->parent ? n->parent->id : 0);
}

JSValue js_next_sibling(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* s = n ? n->next_sibling() : nullptr;
    return JS_NewInt32(ctx, id_of(s));
}

JSValue js_prev_sibling(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* s = n ? n->previous_sibling() : nullptr;
    return JS_NewInt32(ctx, id_of(s));
}

JSValue js_children(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::vector<int> ids;
    if (argc >= 1) {
        Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
        if (n) for (auto* c : n->children) ids.push_back(c->id);
    }
    return array_of_ids(ctx, ids);
}

JSValue js_create_element(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = b->doc->create_element(arg_str(ctx, argv[0]));
    return JS_NewInt32(ctx, n->id);
}

JSValue js_create_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = b->doc->create_text(arg_str(ctx, argv[0]));
    return JS_NewInt32(ctx, n->id);
}

JSValue js_create_comment(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = b->doc->create_comment(arg_str(ctx, argv[0]));
    return JS_NewInt32(ctx, n->id);
}

JSValue js_append(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_NewInt32(ctx, 0);
    Node* p = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* c = node_of(ctx, arg_i32(ctx, argv[1]));
    if (!p || !c) return JS_NewInt32(ctx, 0);
    p->append_child(c);
    return JS_NewInt32(ctx, c->id);
}

JSValue js_insert_before(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_NewInt32(ctx, 0);
    Node* p = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* c = node_of(ctx, arg_i32(ctx, argv[1]));
    Node* r = argc >= 3 ? node_of(ctx, arg_i32(ctx, argv[2])) : nullptr;
    if (!p || !c) return JS_NewInt32(ctx, 0);
    p->insert_before(c, r);
    return JS_NewInt32(ctx, c->id);
}

JSValue js_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node* p = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* c = node_of(ctx, arg_i32(ctx, argv[1]));
    if (p && c) p->remove_child(c);
    return JS_UNDEFINED;
}

JSValue js_replace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    Node* p = node_of(ctx, arg_i32(ctx, argv[0]));
    Node* n = node_of(ctx, arg_i32(ctx, argv[1]));
    Node* o = node_of(ctx, arg_i32(ctx, argv[2]));
    if (p && n && o) p->replace_child(n, o);
    return JS_UNDEFINED;
}

JSValue js_by_id(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = b->doc->get_element_by_id(arg_str(ctx, argv[0]));
    return JS_NewInt32(ctx, id_of(n));
}

JSValue js_query(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 2) return JS_NewInt32(ctx, 0);
    int root_id = arg_i32(ctx, argv[0]);
    Node* scope = root_id > 0 && root_id != b->doc->root->id ? node_of(ctx, root_id) : nullptr;
    Node* n = b->doc->query_selector(arg_str(ctx, argv[1]), scope);
    return JS_NewInt32(ctx, id_of(n));
}

JSValue js_query_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    std::vector<int> ids;
    if (b && argc >= 2) {
        int root_id = arg_i32(ctx, argv[0]);
        Node* scope = root_id > 0 && root_id != b->doc->root->id ? node_of(ctx, root_id) : nullptr;
        auto nodes = b->doc->query_selector_all(arg_str(ctx, argv[1]), scope);
        for (auto* n : nodes) ids.push_back(n->id);
    }
    return array_of_ids(ctx, ids);
}

JSValue js_matches(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 2) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return JS_NewInt32(ctx, n && b->doc->matches_selector(n, arg_str(ctx, argv[1])) ? 1 : 0);
}

JSValue js_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->attr(arg_str(ctx, argv[1])) : "");
}

JSValue js_has_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_FALSE;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return JS_NewBool(ctx, n && n->has_attr(arg_str(ctx, argv[1])));
}

JSValue js_set_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (n) n->set_attr(arg_str(ctx, argv[1]), arg_str(ctx, argv[2]));
    return JS_UNDEFINED;
}

JSValue js_remove_attr(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (n) n->remove_attr(arg_str(ctx, argv[1]));
    return JS_UNDEFINED;
}

JSValue js_attrs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    if (argc < 1) return arr;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return arr;
    uint32_t k = 0;
    for (auto& a : n->attrs) {
        JS_SetPropertyUint32(ctx, arr, k++, str_val(ctx, a.first));
        JS_SetPropertyUint32(ctx, arr, k++, str_val(ctx, a.second));
    }
    return arr;
}

JSValue js_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->data : "");
}

JSValue js_set_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (n && (n->type == NodeType::Text || n->type == NodeType::Comment)) {n->data = arg_str(ctx, argv[1]);if(n->doc)++n->doc->revision;}
    return JS_UNDEFINED;
}

JSValue js_text_content(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->text_content() : "");
}

JSValue js_set_text_content(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return JS_UNDEFINED;
    std::string text = arg_str(ctx, argv[1]);
    if (n->type == NodeType::Text || n->type == NodeType::Comment) {n->data = text;if(n->doc)++n->doc->revision;}
    else n->set_text_content(text);
    return JS_UNDEFINED;
}

JSValue js_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->inner_html() : "");
}

JSValue js_set_inner_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 2) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return JS_UNDEFINED;
    std::string html = arg_str(ctx, argv[1]);
    auto kids = parse_html_fragment(*b->doc, html);
    n->remove_all_children();
    for (auto* k : kids) n->append_child(k);
    b->doc->recompute_head_body();
    return JS_UNDEFINED;
}

JSValue js_outer_html(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->outer_html() : "");
}

JSValue js_clone(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_NewInt32(ctx, 0);
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    bool deep = argc >= 2 && arg_i32(ctx, argv[1]) != 0;
    Node* c = clone_node(*b->doc, n, deep);
    return JS_NewInt32(ctx, id_of(c));
}

JSValue js_style_css(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? n->attr("style") : "");
}

JSValue js_set_style_css(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (n) n->set_attr("style", arg_str(ctx, argv[1]));
    return JS_UNDEFINED;
}

JSValue js_get_style(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    return str_val(ctx, n ? get_style_prop(n->attr("style"), arg_str(ctx, argv[1])) : "");
}

JSValue js_set_style(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return JS_UNDEFINED;
    std::string css = n->attr("style");
    set_style_prop(css, arg_str(ctx, argv[1]), arg_str(ctx, argv[2]));
    n->set_attr("style", css);
    return JS_UNDEFINED;
}

bool refresh_geometry(JSContext* ctx){try{auto* b=bridge(ctx);if(b&&b->owner)b->owner->refresh_layout();return true;}catch(const std::exception&){JS_ThrowInternalError(ctx,"preview layout refresh failed");return false;}}

JSValue js_rect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if(!refresh_geometry(ctx))return JS_EXCEPTION;
    RectF r;
    if (argc >= 1) {
        Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
        if (n) {auto* ancestor=n;while(ancestor->parent)ancestor=ancestor->parent;auto* b=bridge(ctx);if(b&&ancestor==b->doc->root)r=n->absolute_rect();}
    }
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, r.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, r.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, r.w));
    JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, r.h));
    return arr;
}

JSValue js_computed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if(!refresh_geometry(ctx))return JS_EXCEPTION;
    if (argc < 2) return str_val(ctx, "");
    Node* n = node_of(ctx, arg_i32(ctx, argv[0]));
    if (!n) return str_val(ctx, "");
    std::string prop = util::to_lower(arg_str(ctx, argv[1]));
    return str_val(ctx, prop_css(n, prop));
}

JSValue js_document_id(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return JS_NewInt32(ctx, b && b->doc->root ? b->doc->root->id : 1);
}

JSValue js_document_element(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return JS_NewInt32(ctx, b && b->doc->html ? b->doc->html->id : 0);
}

JSValue js_head(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return JS_NewInt32(ctx, b && b->doc->head ? b->doc->head->id : 0);
}

JSValue js_body(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return JS_NewInt32(ctx, b && b->doc->body ? b->doc->body->id : 0);
}

JSValue js_title(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return str_val(ctx, b ? b->doc->title : "");
}

JSValue js_set_title(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_UNDEFINED;
    std::string t = arg_str(ctx, argv[0]);
    b->doc->title = t;
    Node* title = nullptr;
    std::function<Node*(Node*)> find = [&](Node* n) -> Node* {
        if (!n) return nullptr;
        if (n->is_element("title")) return n;
        for (auto* c : n->children) if (auto* r = find(c)) return r;
        return nullptr;
    };
    title = find(b->doc->root);
    if (!title) {
        Node* head = b->doc->head;
        if (!head) return JS_UNDEFINED;
        title = b->doc->create_element("title");
        head->append_child(title);
    }
    title->set_text_content(t);
    return JS_UNDEFINED;
}

JSValue js_document_url(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return str_val(ctx, b ? b->doc->url : "");
}

JSValue js_base_url(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    return str_val(ctx, b ? b->doc->base_url : "");
}

JSValue js_viewport(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* b = bridge(ctx);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, b ? b->vw : 1280));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, b ? b->vh : 800));
    return arr;
}

JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    std::string level = util::to_lower(arg_str(ctx, argv[0]));
    std::string msg = arg_str(ctx, argv[1]);
    if (level == "error") util::error("[js] %s", msg.c_str());
    else if (level == "warn") util::warn("[js] %s", msg.c_str());
    else util::log("[js] %s", msg.c_str());
    return JS_UNDEFINED;
}

JSValue js_read_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* b = bridge(ctx);
    if (!b || argc < 1) return JS_ThrowTypeError(ctx, "readText: missing URL");
    std::string url = arg_str(ctx, argv[0]);
    LoadResult r = b->loader->load(url);
    if (!r.ok) return JS_ThrowTypeError(ctx, "readText failed: %s", r.error.c_str());
    return str_val(ctx, r.data);
}

// Avoid QuickJS's C99 designated-initializer macro in this C++17 bridge.
JSCFunctionListEntry bridge_function(const char* name,uint8_t length,JSCFunction* function){
    JSCFunctionListEntry entry{};entry.name=name;entry.prop_flags=JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE;entry.def_type=JS_DEF_CFUNC;
    entry.u.func.length=length;entry.u.func.cproto=JS_CFUNC_generic;entry.u.func.cfunc.generic=function;return entry;
}
const JSCFunctionListEntry kMwFuncs[] = {
    bridge_function("nodeType", 1, js_node_type),
    bridge_function("nodeName", 1, js_node_name),
    bridge_function("tagName", 1, js_tag_name),
    bridge_function("parent", 1, js_parent),
    bridge_function("nextSibling", 1, js_next_sibling),
    bridge_function("prevSibling", 1, js_prev_sibling),
    bridge_function("children", 1, js_children),
    bridge_function("createElement", 1, js_create_element),
    bridge_function("createTextNode", 1, js_create_text),
    bridge_function("createComment", 1, js_create_comment),
    bridge_function("append", 2, js_append),
    bridge_function("insertBefore", 3, js_insert_before),
    bridge_function("remove", 2, js_remove),
    bridge_function("replace", 3, js_replace),
    bridge_function("byId", 1, js_by_id),
    bridge_function("query", 2, js_query),
    bridge_function("queryAll", 2, js_query_all),
    bridge_function("matches", 2, js_matches),
    bridge_function("attr", 2, js_attr),
    bridge_function("hasAttr", 2, js_has_attr),
    bridge_function("setAttr", 3, js_set_attr),
    bridge_function("removeAttr", 2, js_remove_attr),
    bridge_function("attrs", 1, js_attrs),
    bridge_function("text", 1, js_text),
    bridge_function("setText", 2, js_set_text),
    bridge_function("textContent", 1, js_text_content),
    bridge_function("setTextContent", 2, js_set_text_content),
    bridge_function("innerHTML", 1, js_inner_html),
    bridge_function("setInnerHTML", 2, js_set_inner_html),
    bridge_function("outerHTML", 1, js_outer_html),
    bridge_function("clone", 2, js_clone),
    bridge_function("styleCss", 1, js_style_css),
    bridge_function("setStyleCss", 2, js_set_style_css),
    bridge_function("getStyle", 2, js_get_style),
    bridge_function("setStyle", 3, js_set_style),
    bridge_function("rect", 1, js_rect),
    bridge_function("computed", 2, js_computed),
    bridge_function("documentId", 0, js_document_id),
    bridge_function("documentElement", 0, js_document_element),
    bridge_function("head", 0, js_head),
    bridge_function("body", 0, js_body),
    bridge_function("title", 0, js_title),
    bridge_function("setTitle", 1, js_set_title),
    bridge_function("documentUrl", 0, js_document_url),
    bridge_function("baseUrl", 0, js_base_url),
    bridge_function("viewport", 0, js_viewport),
    bridge_function("log", 2, js_log),
    bridge_function("readText", 1, js_read_text),
};

int interrupt_cb(JSContext*, void* opaque) {
    auto* rt = static_cast<JsRuntime*>(opaque);
    return rt && rt->should_interrupt() ? 1 : 0;
}

} // namespace

JsRuntime::JsRuntime(Document& doc, Loader& loader, int viewport_w, int viewport_h, std::function<void()> refresh)
    : refresh_(std::move(refresh)), doc_(doc), loader_(loader), vw_(viewport_w), vh_(viewport_h) {}

JsRuntime::~JsRuntime() {
    auto* ctx = static_cast<JSContext*>(ctx_);
    auto* rt = static_cast<JSRuntime*>(rt_);
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
    delete bridge_;
}

bool JsRuntime::install_bridge() {
    auto* ctx = static_cast<JSContext*>(ctx_);
    bridge_ = new Bridge{&doc_, &loader_, this, vw_, vh_};
    JS_SetContextOpaque(ctx, bridge_);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mw = JS_NewObject(ctx);
    if (JS_SetPropertyFunctionList(ctx, mw, kMwFuncs, (int)(sizeof(kMwFuncs) / sizeof(kMwFuncs[0]))) < 0) {
        JS_FreeValue(ctx, mw);
        JS_FreeValue(ctx, global);
        return false;
    }
    JS_SetPropertyStr(ctx, global, "__mw", mw);
    JS_FreeValue(ctx, global);
    return true;
}

bool JsRuntime::init() {
    auto* rt = JS_NewRuntime();
    if (!rt) {
        util::error("cannot create JavaScript runtime");
        return false;
    }
    rt_ = rt;
    JS_SetMemoryLimit(rt, 256u * 1024u * 1024u);
    JS_SetMaxStackSize(rt, 8u * 1024u * 1024u);
    JS_SetInterruptHandler(rt, interrupt_cb, this);
    auto* ctx = JS_NewContext(rt);
    if (!ctx) {
        util::error("cannot create JavaScript context");
        return false;
    }
    ctx_ = ctx;
    if (!install_bridge()) return false;
    JS_SetModuleLoaderFunc(rt, module_normalize_cb, module_load_cb, bridge_);
    deadline_ms_ = util::now_ms() + 2000;
    JSValue v = JS_Eval(ctx, miniweb_prelude(), strlen(miniweb_prelude()), "<miniweb-prelude>", JS_EVAL_TYPE_GLOBAL);
    deadline_ms_ = 0;
    if (JS_IsException(v)) {
        dump_exception("prelude");
        JS_FreeValue(ctx, v);
        return false;
    }
    JS_FreeValue(ctx, v);
    drain_jobs();
    return true;
}

void JsRuntime::dump_exception(const char* where) {
    auto* ctx = static_cast<JSContext*>(ctx_);
    JSValue exc = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, exc);
    if (s) {
        util::error("JavaScript error (%s): %s", where, s);
        JS_FreeCString(ctx, s);
    } else {
        util::error("JavaScript error (%s)", where);
    }
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsNull(stack)) {
        const char* ss = JS_ToCString(ctx, stack);
        if (ss) {
            util::error("JS stack:\n%s", ss);
            JS_FreeCString(ctx, ss);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

void JsRuntime::drain_jobs() {
    auto* ctx = static_cast<JSContext*>(ctx_);
    auto* rt = static_cast<JSRuntime*>(rt_);
    for (int i = 0; i < 10000; i++) {
        JSContext* job_ctx = nullptr;
        int r = JS_ExecutePendingJob(rt, &job_ctx);
        if (r == 0) break;
        if (r < 0) {
            dump_exception("promise");
            break;
        }
        (void)ctx;
    }
}

bool JsRuntime::eval(const std::string& code, const std::string& filename, bool module) {
    auto* ctx = static_cast<JSContext*>(ctx_);
    if (!ctx) return false;
    deadline_ms_ = util::now_ms() + 2000;
    JSValue v = JS_Eval(ctx, code.data(), code.size(), filename.c_str(),
                        module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
    bool ok = true;
    if (JS_IsException(v)) {
        dump_exception(filename.c_str());
        ok = false;
    }
    JS_FreeValue(ctx, v);
    drain_jobs();
    deadline_ms_ = 0;
    return ok;
}

void JsRuntime::execute_scripts() {
    int inline_idx = 0;
    // innerHTML can append inert script nodes while an initial script executes.
    // Never iterate the vector that those DOM operations can reallocate.
    const auto initial_scripts = doc_.script_nodes;
    for (Node* s : initial_scripts) {
        if (!s || !s->is_element("script")) continue;
        if (s->has_attr("nomodule")) continue;
        std::string type = util::to_lower(util::trim(s->attr("type")));
        size_t semi = type.find(';');
        if (semi != std::string::npos) type = util::trim(type.substr(0, semi));
        bool module = type == "module";
        bool js = type.empty() || module || type == "text/javascript" || type == "application/javascript" ||
                  type == "text/ecmascript" || type == "application/ecmascript";
        if (!js) continue;
        std::string code;
        std::string filename;
        if (s->has_attr("src")) {
            std::string url = doc_.resolve(s->attr("src"));
            LoadResult r = loader_.load_absolute(url);
            if (!r.ok) {
                util::warn("script load failed (%s): %s", url.c_str(), r.error.c_str());
                continue;
            }
            code = std::move(r.data);
            filename = url;
        } else {
            code = s->text_content();
            filename = "<inline-script-" + std::to_string(++inline_idx) + ">";
        }
        if (code.empty()) continue;
        eval(code, filename, module);
    }
}

void JsRuntime::dispatch_dom_content_loaded() {
    eval("__mwFire('DOMContentLoaded');", "<dom-content-loaded>", false);
}

void JsRuntime::dispatch_load() {
    eval("__mwFire('load');", "<load>", false);
}

void JsRuntime::drain_timers(int virtual_ms) {
    auto* context=static_cast<JSContext*>(ctx_);double now=0;
    auto clock=JS_Eval(context,"__mwTimerNow()",14,"<timer-clock>",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(clock)||JS_ToFloat64(context,&now,clock)<0||!std::isfinite(now)){JS_FreeValue(context,clock);dump_exception("timer-clock");return;}
    JS_FreeValue(context,clock);const auto end=util::format_float(now+std::max(0,virtual_ms),3);
    int total = 0;
    for (int i = 0; i < 5000; i++) {
        auto* ctx = static_cast<JSContext*>(ctx_);
        deadline_ms_ = util::now_ms() + 2000;
        std::string code = "__mwDrainTimers(0," + end + ");";
        JSValue v = JS_Eval(ctx, code.data(), code.size(), "<timers>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            deadline_ms_ = 0;
            dump_exception("timers");
            JS_FreeValue(ctx, v);
            break;
        }
        int32_t ran = 0;
        JS_ToInt32(ctx, &ran, v);
        JS_FreeValue(ctx, v);
        drain_jobs();
        deadline_ms_ = 0;
        total += ran;
        if (ran <= 0) break;
        if (total >= 5000) {util::warn("preview timer callback budget reached");break;}
    }
}

void JsRuntime::run(int wait_ms) {
    execute_scripts();
    dispatch_dom_content_loaded();
    dispatch_load();
    drain_timers(wait_ms);
}
