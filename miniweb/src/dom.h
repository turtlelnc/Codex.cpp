#pragma once
#include "box.h"
#include "style.h"
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

enum class NodeType : int {
    Element = 1,
    Text = 3,
    Comment = 8,
    Document = 9,
};

struct Document;

struct Node {
    NodeType type = NodeType::Element;
    std::string tag;                  // lowercase tag name for elements
    std::string data;                 // text / comment data
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node*> children;
    Node* parent = nullptr;
    Document* doc = nullptr;
    int id = 0;
    bool anonymous = false;         // layout-only box; never exposed through the DOM

    ComputedStyle style;
    LayoutInfo layout;

    bool is_element() const { return type == NodeType::Element; }
    bool is_text() const { return type == NodeType::Text; }
    bool is_comment() const { return type == NodeType::Comment; }
    bool is_document() const { return type == NodeType::Document; }
    bool is_element(const char* name) const { return type == NodeType::Element && tag == name; }
    bool is_element(const std::string& name) const { return type == NodeType::Element && tag == name; }

    const std::string* attr_ptr(const std::string& name) const;
    std::string attr(const std::string& name) const;
    std::string attr_or(const std::string& name, const std::string& def) const;
    bool has_attr(const std::string& name) const;
    void set_attr(const std::string& name, const std::string& value);
    void remove_attr(const std::string& name);

    std::vector<std::string> classes() const;
    bool has_class(const std::string& c) const;

    Node* append_child(Node* child);
    Node* insert_before(Node* child, Node* ref);
    Node* remove_child(Node* child);
    Node* replace_child(Node* new_child, Node* old_child);
    void detach();
    void remove_all_children();

    Node* first_element_child() const;
    Node* next_sibling() const;
    Node* previous_sibling() const;

    std::string text_content() const;
    void set_text_content(const std::string& text);
    std::string inner_html() const;
    std::string outer_html() const;
    std::string inner_text() const;

    Node* closest(const char* tag_name) const;
    Node* link_ancestor() const;
    std::string href() const;
    std::string src() const;
    RectF absolute_rect() const;
};

struct Document {
    uint64_t revision = 0;
    std::deque<Node> arena;
    std::deque<Node> layout_nodes;   // anonymous boxes, rebuilt on each layout
    std::vector<Node*> id_map; // index == node id
    Node* root = nullptr;
    Node* html = nullptr;
    Node* head = nullptr;
    Node* body = nullptr;

    std::string url;        // canonical file:// or http://127.0.0.1 URL
    std::string base_url;   // <base href> or url
    std::string title;
    int viewport_w = 1280;
    int viewport_h = 800;

    std::vector<Node*> style_nodes;
    std::vector<Node*> script_nodes;
    std::vector<Node*> link_nodes;

    Document();
    Node* alloc(NodeType type);
    Node* create_element(const std::string& tag);
    Node* create_text(const std::string& text);
    Node* create_comment(const std::string& text);
    Node* create_document();

    Node* get_element_by_id(const std::string& id) const;
    Node* query_selector(const std::string& selector, Node* scope = nullptr) const;
    std::vector<Node*> query_selector_all(const std::string& selector, Node* scope = nullptr) const;
    bool matches_selector(const Node* node, const std::string& selector) const;

    void recompute_head_body();
    void set_title_from_dom();
    std::string resolve(const std::string& rel) const;
    void remove_all();
};

std::string serialize_html(const Node* node, bool outer = true);

// Shared visual geometry/order keeps painting, links and hit-testing consistent.
inline RectF visual_box(const Node* n,float ox,float oy){
    const auto& l=n->layout;return {ox+l.border.x+resolve_length(n->style.translate_x,l.border.w,n->style.font_size),oy+l.border.y+resolve_length(n->style.translate_y,l.border.h,n->style.font_size),l.border.w,l.border.h};
}
inline RectF padding_box(const Node* n,const RectF& border){
    const auto& l=n->layout;
    return {border.x+l.border_left,border.y+l.border_top,
            std::max(0.0f,border.w-l.border_left-l.border_right),
            std::max(0.0f,border.h-l.border_top-l.border_bottom)};
}
inline std::vector<Node*> paint_order(const Node* n){auto children=n->layout.box_children;
    auto z=[&](const Node* c){return c->style.position!=Position::Static||n->style.display==Display::Flex||n->style.display==Display::Grid?c->style.z_index:0;};
    auto layer=[&](const Node* c){return c->style.position!=Position::Static||c->style.has_transform||c->style.opacity<1||
        (!c->style.z_auto&&(n->style.display==Display::Flex||n->style.display==Display::Grid))?1:0;};
    std::stable_sort(children.begin(),children.end(),[&](const Node* a,const Node* b){
        if(z(a)!=z(b))return z(a)<z(b);
        if(layer(a)!=layer(b))return layer(a)<layer(b);
        return layer(a)?a->layout.order<b->layout.order:false;
    });return children;
}
