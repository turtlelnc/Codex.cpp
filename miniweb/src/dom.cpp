#include "dom.h"
#include <algorithm>
#include <cstdio>
#include <functional>

namespace {
const std::string kEmpty;

bool is_void_tag(const std::string& t) {
    static const char* tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr"};
    for (auto* x : tags) if (t == x) return true;
    return false;
}

bool is_raw_text_tag(const std::string& t) {
    return t == "script" || t == "style" || t == "textarea" || t == "title";
}

std::string lower_attr(std::string s) { return util::to_lower(s); }
} // namespace

const std::string* Node::attr_ptr(const std::string& name) const {
    std::string n = lower_attr(name);
    for (auto& a : attrs) if (a.first == n) return &a.second;
    return nullptr;
}

std::string Node::attr(const std::string& name) const {
    auto p = attr_ptr(name);
    return p ? *p : std::string();
}

std::string Node::attr_or(const std::string& name, const std::string& def) const {
    auto p = attr_ptr(name);
    return p ? *p : def;
}

bool Node::has_attr(const std::string& name) const { return attr_ptr(name) != nullptr; }

void Node::set_attr(const std::string& name, const std::string& value) {
    std::string n = lower_attr(name);
    for (auto& a : attrs) {
        if (a.first == n) { a.second = value; return; }
    }
    attrs.emplace_back(n, value);
}

void Node::remove_attr(const std::string& name) {
    std::string n = lower_attr(name);
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(), [&](auto& a) { return a.first == n; }),
                attrs.end());
}

std::vector<std::string> Node::classes() const {
    return util::split_ws(attr("class"));
}

bool Node::has_class(const std::string& c) const {
    for (auto& x : classes()) if (x == c) return true;
    return false;
}

Node* Node::append_child(Node* child) {
    if (!child || child == this) return nullptr;
    for (Node* p = this; p; p = p->parent) if (p == child) return nullptr;
    if (child->parent) child->detach();
    child->parent = this;
    children.push_back(child);
    if (doc) child->doc = doc;
    return child;
}

Node* Node::insert_before(Node* child, Node* ref) {
    if (!child) return nullptr;
    if (!ref) return append_child(child);
    if (ref->parent != this) return nullptr;
    if (child == this) return nullptr;
    for (Node* p = this; p; p = p->parent) if (p == child) return nullptr;
    if (child->parent) child->detach();
    auto it = std::find(children.begin(), children.end(), ref);
    if (it == children.end()) return append_child(child);
    child->parent = this;
    if (doc) child->doc = doc;
    children.insert(it, child);
    return child;
}

Node* Node::remove_child(Node* child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) return nullptr;
    children.erase(it);
    if (child) child->parent = nullptr;
    return child;
}

Node* Node::replace_child(Node* new_child, Node* old_child) {
    if (!new_child || !old_child) return nullptr;
    auto it = std::find(children.begin(), children.end(), old_child);
    if (it == children.end()) return nullptr;
    if (new_child->parent) new_child->detach();
    new_child->parent = this;
    if (doc) new_child->doc = doc;
    *it = new_child;
    old_child->parent = nullptr;
    return old_child;
}

void Node::detach() {
    if (parent) {
        auto it = std::find(parent->children.begin(), parent->children.end(), this);
        if (it != parent->children.end()) parent->children.erase(it);
        parent = nullptr;
    }
}

void Node::remove_all_children() {
    for (auto* c : children) if (c) c->parent = nullptr;
    children.clear();
}

Node* Node::first_element_child() const {
    for (auto* c : children) if (c && c->is_element()) return c;
    return nullptr;
}

Node* Node::next_sibling() const {
    if (!parent) return nullptr;
    auto it = std::find(parent->children.begin(), parent->children.end(), this);
    if (it == parent->children.end()) return nullptr;
    ++it;
    return it == parent->children.end() ? nullptr : *it;
}

Node* Node::previous_sibling() const {
    if (!parent) return nullptr;
    auto it = std::find(parent->children.begin(), parent->children.end(), this);
    if (it == parent->children.begin() || it == parent->children.end()) return nullptr;
    --it;
    return *it;
}

std::string Node::text_content() const {
    std::string out;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Text) { out += n->data; return; }
        for (auto* c : n->children) walk(c);
    };
    walk(this);
    return out;
}

void Node::set_text_content(const std::string& text) {
    remove_all_children();
    if (doc) {
        Node* t = doc->create_text(text);
        append_child(t);
    }
}

std::string Node::inner_html() const {
    std::string out;
    for (auto* c : children) out += serialize_html(c, true);
    return out;
}

std::string Node::outer_html() const { return serialize_html(this, true); }

std::string Node::inner_text() const {
    std::string out;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Text) { out += n->data; return; }
        if (n->is_element("script") || n->is_element("style")) return;
        bool block = n->style.is_block_level();
        if (block && !out.empty() && out.back() != '\n') out.push_back('\n');
        for (auto* c : n->children) walk(c);
        if (block && !out.empty() && out.back() != '\n') out.push_back('\n');
    };
    walk(this);
    return out;
}

Node* Node::closest(const char* tag_name) const {
    for (Node* n = const_cast<Node*>(this); n; n = n->parent) {
        if (n->is_element(tag_name)) return n;
    }
    return nullptr;
}

Node* Node::link_ancestor() const {
    for (Node* n = const_cast<Node*>(this); n; n = n->parent) {
        if (n->is_element("a") && n->has_attr("href")) return n;
    }
    return nullptr;
}

std::string Node::href() const { return attr("href"); }
std::string Node::src() const { return attr("src"); }

RectF Node::absolute_rect() const {
    RectF r;
    if (!layout.has_box) return r;
    r = visual_box(this,0,0);
    const Node* p = parent;
    while (p) {
        auto b=visual_box(p,0,0);r.x += p->layout.content_x + b.x;
        r.y += p->layout.content_y + b.y;
        p = p->parent;
    }
    return r;
}

Document::Document() {
    root = alloc(NodeType::Document);
    root->tag = "#document";
}

Node* Document::alloc(NodeType type) {
    arena.emplace_back();
    Node* n = &arena.back();
    int id = (int)id_map.size();
    if (id == 0) {
        id_map.push_back(nullptr);
        id = 1;
    }
    n->id = id;
    n->doc = this;
    n->type = type;
    id_map.push_back(n);
    return n;
}

Node* Document::create_element(const std::string& tag) {
    Node* n = alloc(NodeType::Element);
    n->tag = util::to_lower(tag);
    return n;
}

Node* Document::create_text(const std::string& text) {
    Node* n = alloc(NodeType::Text);
    n->data = text;
    return n;
}

Node* Document::create_comment(const std::string& text) {
    Node* n = alloc(NodeType::Comment);
    n->data = text;
    return n;
}

Node* Document::create_document() {
    Node* n = alloc(NodeType::Document);
    n->tag = "#document";
    return n;
}

Node* Document::get_element_by_id(const std::string& id) const {
    if (!root) return nullptr;
    std::function<Node*(Node*)> walk = [&](Node* n) -> Node* {
        if (!n) return nullptr;
        if (n->is_element() && n->attr("id") == id) return n;
        for (auto* c : n->children) {
            if (auto* r = walk(c)) return r;
        }
        return nullptr;
    };
    return walk(root);
}

Node* Document::query_selector(const std::string& selector, Node* scope) const {
    auto all = query_selector_all(selector, scope);
    return all.empty() ? nullptr : all[0];
}

std::vector<Node*> Document::query_selector_all(const std::string& selector, Node* scope) const {
    std::vector<std::string> parts;
    // comma-separated selector lists
    std::string cur;
    int depth = 0;
    for (char c : selector) {
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) { parts.push_back(util::trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    if (!util::trim(cur).empty()) parts.push_back(util::trim(cur));
    std::vector<Node*> out;
    Node* start = scope ? scope : root;
    if (!start) return out;
    std::function<void(Node*)> walk = [&](Node* n) {
        for (auto* c : n->children) {
            if (!c || !c->is_element()) continue;
            for (auto& p : parts) {
                if (!p.empty() && matches_selector(c, p)) {
                    if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
                    break;
                }
            }
            walk(c);
        }
    };
    if (scope && scope->is_element()) {
        // querySelector includes descendants, not the scope itself
    }
    walk(start);
    return out;
}

bool Document::matches_selector(const Node* node, const std::string& selector) const {
    // Implemented in css.cpp through a lightweight matcher declared here to avoid a cycle.
    extern bool css_matches(const Node*, const std::string&, const Document*);
    return css_matches(node, selector, this);
}

void Document::recompute_head_body() {
    html = head = body = nullptr;
    if (!root) return;
    for (auto* c : root->children) {
        if (c->is_element("html")) { html = c; break; }
    }
    Node* scope = html ? html : root;
    for (auto* c : scope->children) {
        if (c->is_element("head") && !head) head = c;
        if (c->is_element("body") && !body) body = c;
    }
    if (!body && html) {
        for (auto* c : html->children) {
            if (!c->is_element("head")) { body = c->is_element() ? c : nullptr; break; }
        }
    }
}

void Document::set_title_from_dom() {
    title.clear();
    if (!root) return;
    std::function<Node*(Node*)> find = [&](Node* n) -> Node* {
        if (!n) return nullptr;
        if (n->is_element("title")) return n;
        for (auto* c : n->children) {
            if (auto* r = find(c)) return r;
        }
        return nullptr;
    };
    Node* t = find(root);
    if (t) title = util::trim(t->text_content());
}

std::string Document::resolve(const std::string& rel) const {
    return util::resolve_url(base_url.empty() ? url : base_url, rel);
}

void Document::remove_all() {
    arena.clear();
    id_map.clear();
    root = html = head = body = nullptr;
    title.clear();
    style_nodes.clear();
    script_nodes.clear();
    link_nodes.clear();
}

namespace {
void serialize_into(const Node* n, std::string& out) {
    if (!n) return;
    if (n->type == NodeType::Text) {
        out += util::html_escape(n->data);
        return;
    }
    if (n->type == NodeType::Comment) {
        out += "<!--";
        out += n->data;
        out += "-->";
        return;
    }
    if (n->type == NodeType::Document) {
        for (auto* c : n->children) serialize_into(c, out);
        return;
    }
    if (!n->is_element()) return;
    out += "<";
    out += n->tag;
    for (auto& a : n->attrs) {
        out += " ";
        out += a.first;
        out += "=\"";
        out += util::html_escape(a.second);
        out += "\"";
    }
    out += ">";
    if (is_void_tag(n->tag)) return;
    bool raw = is_raw_text_tag(n->tag);
    for (auto* c : n->children) {
        if (raw && c->type == NodeType::Text) out += c->data;
        else serialize_into(c, out);
    }
    out += "</";
    out += n->tag;
    out += ">";
}
} // namespace

std::string serialize_html(const Node* node, bool outer) {
    std::string out;
    if (!node) return out;
    if (!outer && node->is_element()) {
        for (auto* c : node->children) serialize_into(c, out);
        return out;
    }
    serialize_into(node, out);
    return out;
}
