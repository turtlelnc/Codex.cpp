#include "dom.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
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
    if(doc)++doc->revision;
    std::string n = lower_attr(name);
    for (auto& a : attrs) {
        if (a.first == n) { if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"attributes",n,a.second,true);a.second = value; return; }
    }
    if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"attributes",n);
    attrs.emplace_back(n, value);
}

void Node::remove_attr(const std::string& name) {
    if(doc)++doc->revision;
    std::string n = lower_attr(name);
    auto old=attr_ptr(n);if(!old)return;if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"attributes",n,*old,true);
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
    if(doc)++doc->revision;
    if (child->parent) child->detach();
    auto* previous=children.empty()?nullptr:children.back();
    child->parent = this;
    children.push_back(child);
    if(doc)child->doc=doc;
    if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"childList",{},{},false,{child},{},previous,nullptr);
    return child;
}

Node* Node::insert_before(Node* child, Node* ref) {
    if (!child) return nullptr;
    if (!ref) return append_child(child);
    if (ref->parent != this) return nullptr;
    if (child == ref) return child;
    if (child == this) return nullptr;
    for (Node* p = this; p; p = p->parent) if (p == child) return nullptr;
    if(doc)++doc->revision;
    if (child->parent) child->detach();
    auto it = std::find(children.begin(), children.end(), ref);
    if (it == children.end()) return append_child(child);
    auto* previous=it==children.begin()?nullptr:*(it-1);
    child->parent = this;
    if (doc) child->doc = doc;
    children.insert(it, child);
    if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"childList",{},{},false,{child},{},previous,ref);
    return child;
}

// Detaching a focused subtree loses focus even if it is reinserted before JS
// next reads activeElement. Layout ancestry may differ; use DOM ancestry here.
static void clear_detached_focus(Node* subtree) {
    if (!subtree || !subtree->doc) return;
    auto* doc=subtree->doc;const int id=doc->focused_id;
    if(id<=0 || id>=static_cast<int>(doc->id_map.size()))return;
    for(auto* n=doc->id_map[id];n;n=n->parent)if(n==subtree){doc->focused_id=0;return;}
}
Node* Node::remove_child(Node* child) {
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) return nullptr;
    if(doc)++doc->revision;
    clear_detached_focus(child);
    if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"childList",{},{},false,{}, {child},child->previous_sibling(),child->next_sibling());
    children.erase(it);
    if (child) child->parent = nullptr;
    return child;
}

Node* Node::replace_child(Node* new_child, Node* old_child) {
    if (!new_child || !old_child) return nullptr;
    auto it = std::find(children.begin(), children.end(), old_child);
    if (it == children.end()) return nullptr;
    if(new_child==old_child)return old_child;
    for(Node* p=this;p;p=p->parent)if(p==new_child)return nullptr;
    if(doc)++doc->revision;
    if (new_child->parent) new_child->detach();
    it=std::find(children.begin(),children.end(),old_child);
    if(it==children.end())return nullptr;
    auto* previous=old_child->previous_sibling();auto* next=old_child->next_sibling();
    new_child->parent = this;
    if (doc) new_child->doc = doc;
    *it = new_child;
    if(doc&&doc->capture_mutations&&!doc->mutation_suppression)doc->record_mutation(this,"childList",{},{},false,{new_child},{old_child},previous,next);
    clear_detached_focus(old_child);
    old_child->parent = nullptr;
    return old_child;
}

void Node::detach() { if(parent)parent->remove_child(this); }

void Node::remove_all_children() {
    if(doc){++doc->revision;if(doc->capture_mutations&&!doc->mutation_suppression&&!children.empty())doc->record_mutation(this,"childList",{},{},false,{},children);}
    for(auto* c:children)if(c){clear_detached_focus(c);c->parent=nullptr;}
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

void Node::set_data(const std::string& text) {
    if (doc) {
        ++doc->revision;
        if (doc->capture_mutations && !doc->mutation_suppression)
            doc->record_mutation(this, "characterData", {}, data, true);
    }
    data = text;
}

void Node::replace_children(const std::vector<Node*>& nodes) {
    for (auto* n : nodes)
        if (n && n->parent && n->parent != this) n->detach();
    std::vector<Node*> old;
    if (doc && doc->capture_mutations && !doc->mutation_suppression) old = children;
    struct Suppress {
        Document* d;
        explicit Suppress(Document* value) : d(value) { if (d) ++d->mutation_suppression; }
        ~Suppress() { if (d) --d->mutation_suppression; }
    };
    {
        Suppress guard(doc);
        remove_all_children();
        for (auto* n : nodes) append_child(n);
    }
    if (doc && doc->capture_mutations && !doc->mutation_suppression &&
        (!old.empty() || !children.empty()))
        doc->record_mutation(this, "childList", {}, {}, false, children, old);
}

void Node::set_text_content(const std::string& text) {
    if (is_text() || is_comment()) { set_data(text); return; }
    if (is_document()) return;
    std::vector<Node*> nodes;
    if (doc && !text.empty()) nodes.push_back(doc->create_text(text));
    replace_children(nodes);
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
    const Node* p = layout.parent;
    while (p) {
        auto b=visual_box(p,0,0);r.x += p->layout.content_x + b.x;
        r.y += p->layout.content_y + b.y;
        p = p->layout.parent;
    }
    return r;
}

Document::Document() {
    root = alloc(NodeType::Document);
    root->tag = "#document";
}

Node* Document::alloc(NodeType type) {
    ++revision;
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
    ++revision;
    layout_nodes.clear();
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

Overflow overflow_axis(const Node* n,bool horizontal) {
    auto value=horizontal?n->style.overflow_x:n->style.overflow_y;
    auto other=horizontal?n->style.overflow_y:n->style.overflow_x;
    if(other!=Overflow::Visible&&other!=Overflow::Clip){
        if(value==Overflow::Visible)value=Overflow::Auto;
        if(value==Overflow::Clip)value=Overflow::Hidden;
    }
    return value;
}
RectF overflow_clip(const Node* n,const RectF& box) {
    auto r=padding_box(n,box);
    if(n->doc&&n==n->doc->html)r={0,0,float(n->doc->viewport_w),float(n->doc->viewport_h)};
    if(overflow_axis(n,true)==Overflow::Visible){r.x=-10000000;r.w=20000000;}
    if(overflow_axis(n,false)==Overflow::Visible){r.y=-10000000;r.h=20000000;}
    return r;
}
namespace {
bool viewport_fixed(const Node* n){
    if(n->style.position!=Position::Fixed)return false;
    for(auto* p=n->parent;p;p=p->parent)if(p->style.has_transform)return false;
    return true;
}
float client_size(const Node* n,bool x){const auto& l=n->layout;return std::max(0.0f,x?l.border.w-l.border_left-l.border_right:l.border.h-l.border_top-l.border_bottom);}
bool scrollable(const Node* n,bool x){auto o=overflow_axis(n,x);return o==Overflow::Auto||o==Overflow::Scroll||o==Overflow::Hidden;}
}
RectF Node::viewport_rect() const {
    if(!layout.has_box)return {};
    if(doc&&visual_cache_revision==doc->visual_revision)return visual_cache;
    auto cached=[&](RectF r){if(doc){visual_cache=r;visual_cache_revision=doc->visual_revision;}return r;};
    if(viewport_fixed(this))return cached(absolute_rect());
    RectF r=visual_box(this,0,0);
    RectF parent_box;
    if(auto* p=layout.parent){parent_box=p->viewport_rect();r.x+=parent_box.x+p->layout.content_x-p->scroll_x;r.y+=parent_box.y+p->layout.content_y-p->scroll_y;}
    else if(doc){r.x-=doc->scroll_x;r.y-=doc->scroll_y;}
    if(style.position==Position::Sticky&&doc){
        const Node* container=parent;
        while(container&&container!=doc->html&&!scrollable(container,false)&&!scrollable(container,true))container=container->parent;
        RectF port{0,0,float(doc->viewport_w),float(doc->viewport_h)};
        if(container&&container!=doc->html)port=padding_box(container,container==layout.parent?parent_box:container->viewport_rect());
        if(!layout.parent)parent_box={0,0,float(doc->viewport_w),doc->scroll_height};
        const auto* p=layout.parent;const float normal_x=r.x,normal_y=r.y;
        const float begin_x=parent_box.x+(p?p->layout.content_x-p->scroll_x:0),begin_y=parent_box.y+(p?p->layout.content_y-p->scroll_y:0);
        const float width=p?(scrollable(p,true)?std::max(p->layout.content_w,p->scroll_width-p->layout.padding_left-p->layout.padding_right):p->layout.content_w):parent_box.w;
        const float height=p?(scrollable(p,false)?std::max(p->layout.content_h,p->scroll_height-p->layout.padding_top-p->layout.padding_bottom):p->layout.content_h):parent_box.h;
        if(!style.top.is_auto())r.y=std::max(normal_y,std::min(std::max(normal_y,port.y+resolve_length(style.top,port.h,style.font_size)),begin_y+height-r.h));
        else if(!style.bottom.is_auto())r.y=std::min(normal_y,std::max(std::min(normal_y,port.bottom()-resolve_length(style.bottom,port.h,style.font_size)-r.h),begin_y));
        if(!style.left.is_auto())r.x=std::max(normal_x,std::min(std::max(normal_x,port.x+resolve_length(style.left,port.w,style.font_size)),begin_x+width-r.w));
        else if(!style.right.is_auto())r.x=std::min(normal_x,std::max(std::min(normal_x,port.right()-resolve_length(style.right,port.w,style.font_size)-r.w),begin_x));
    }
    return cached(r);
}
void update_scroll_layout(Document& doc){
    ++doc.visual_revision;
    for(auto& n:doc.arena)if(!n.layout.has_box){n.scroll_x=n.scroll_y=n.scroll_width=n.scroll_height=0;doc.pending_scroll_events.erase(n.id);}
    std::function<RectF(Node*)> measure=[&](Node* n){
        const auto& l=n->layout;float right=l.content_w,bottom=l.content_h;
        for(const auto& line:l.lines)for(const auto& item:line.items){right=std::max(right,item.x+item.w);bottom=std::max(bottom,item.y+item.h);}
        for(auto* c:l.box_children){auto extent=measure(c);if(viewport_fixed(c))continue;auto b=visual_box(c,0,0);
            right=std::max(right,b.x+extent.w);bottom=std::max(bottom,b.y+extent.h);}
        n->scroll_width=std::max(client_size(n,true),l.padding_left+right+l.padding_right);
        n->scroll_height=std::max(client_size(n,false),l.padding_top+bottom+l.padding_bottom);
        const auto previous_x=n->scroll_x,previous_y=n->scroll_y;
        n->scroll_x=scrollable(n,true)?std::clamp(n->scroll_x,0.0f,std::max(0.0f,n->scroll_width-client_size(n,true))):0;
        n->scroll_y=scrollable(n,false)?std::clamp(n->scroll_y,0.0f,std::max(0.0f,n->scroll_height-client_size(n,false))):0;
        if(previous_x!=n->scroll_x||previous_y!=n->scroll_y)doc.pending_scroll_events.insert(n->id);
        return RectF{0,0,overflow_axis(n,true)==Overflow::Visible?std::max(l.border.w,l.border_left+n->scroll_width):l.border.w,overflow_axis(n,false)==Overflow::Visible?std::max(l.border.h,l.border_top+n->scroll_height):l.border.h};
    };
    if(!doc.html)return;auto extent=measure(doc.html);
    doc.scroll_width=std::max(float(doc.viewport_w),std::max(extent.w,doc.html->scroll_width));doc.scroll_height=std::max(float(doc.viewport_h),std::max(extent.h,doc.html->scroll_height));
    const auto previous_x=doc.scroll_x,previous_y=doc.scroll_y;
    doc.scroll_x=std::clamp(doc.scroll_x,0.0f,std::max(0.0f,doc.scroll_width-doc.viewport_w));doc.scroll_y=std::clamp(doc.scroll_y,0.0f,std::max(0.0f,doc.scroll_height-doc.viewport_h));
    if(previous_x!=doc.scroll_x||previous_y!=doc.scroll_y)doc.pending_scroll_events.insert(doc.html->id);
}
bool set_scroll(Node* n,float x,float y){
    if(!n||!n->doc||!n->layout.has_box||!std::isfinite(x)||!std::isfinite(y))return false;
    if(n==n->doc->html){auto& d=*n->doc;auto ox=d.scroll_x,oy=d.scroll_y;d.scroll_x=overflow_axis(n,true)==Overflow::Clip?0:std::clamp(x,0.0f,std::max(0.0f,d.scroll_width-d.viewport_w));d.scroll_y=overflow_axis(n,false)==Overflow::Clip?0:std::clamp(y,0.0f,std::max(0.0f,d.scroll_height-d.viewport_h));const bool changed=ox!=d.scroll_x||oy!=d.scroll_y;if(changed){++d.visual_revision;d.pending_scroll_events.insert(n->id);}return changed;}
    const auto ox=n->scroll_x,oy=n->scroll_y;
    n->scroll_x=scrollable(n,true)?std::clamp(x,0.0f,std::max(0.0f,n->scroll_width-client_size(n,true))):0;
    n->scroll_y=scrollable(n,false)?std::clamp(y,0.0f,std::max(0.0f,n->scroll_height-client_size(n,false))):0;
    const bool changed=ox!=n->scroll_x||oy!=n->scroll_y;if(changed){++n->doc->visual_revision;n->doc->pending_scroll_events.insert(n->id);}return changed;
}

void reveal_node(Node* n,int block,int inline_alignment,bool only_if_needed,bool nearest_container){
    if(!n||!n->doc||block<0||block>3||inline_alignment<0||inline_alignment>3)return;
    n->client_rect();if(!n->layout.has_box&&!n->inline_layout_owner)return;
    auto& doc=*n->doc;
    auto delta=[&](float start,float end,float low,float high,int alignment){
        const float a=start-low,b=end-high,size=end-start,port=high-low;
        if(only_if_needed&&((a>=0&&b<=0)||(a<=0&&b>=0)))return 0.0f;
        if(alignment==0)return a;
        if(alignment==1)return (a+b)/2;
        if(alignment==2)return b;
        if((a>=0&&b<=0)||(a<=0&&b>=0))return 0.0f;
        return (a<0&&size<=port)||(b>0&&size>port)?a:b;
    };
    Node* current=n;
    for(Node* ancestor=n==doc.html?n:n->layout.has_box?n->layout.parent:n->inline_layout_owner;ancestor;current=ancestor,ancestor=ancestor->layout.parent){
        if(viewport_fixed(current))break;
        const bool root=ancestor==doc.html;
        if(ancestor->layout.has_box&&(root||scrollable(ancestor,true)||scrollable(ancestor,false))){
            const auto target=n->client_rect();
            const auto port=root?RectF{0,0,float(doc.viewport_w),float(doc.viewport_h)}:padding_box(ancestor,ancestor->viewport_rect());
            const float x=root?doc.scroll_x:ancestor->scroll_x,y=root?doc.scroll_y:ancestor->scroll_y;
            const float dx=(root||scrollable(ancestor,true))?delta(target.x,target.right(),port.x,port.right(),inline_alignment):0;
            const float dy=(root||scrollable(ancestor,false))?delta(target.y,target.bottom(),port.y,port.bottom(),block):0;
            set_scroll(ancestor,x+dx,y+dy);
            if(nearest_container)break;
        }
        if(root)break;
    }
}

RectF Node::client_rect() const {
    if(layout.has_box)return viewport_rect();
    if(!doc||!is_element()||style.display!=Display::Inline){inline_layout_owner=nullptr;return {};}
    if(doc->inline_geometry_revision!=doc->visual_revision){
        // Build all inline unions once per visual revision, not once per query.
        for(auto& n:doc->arena)if(!n.layout.has_box){n.visual_cache={};n.visual_cache_revision=doc->visual_revision;n.inline_layout_owner=nullptr;}
        auto common=[](Node* a,Node* b){for(auto* x=a;x;x=x->layout.parent)for(auto* y=b;y;y=y->layout.parent)if(x==y)return x;return static_cast<Node*>(nullptr);};
        std::function<void(Node*)> visit=[&](Node* owner){
            if(!owner||!owner->layout.has_box)return;
            auto box=owner->viewport_rect();const float x=box.x+owner->layout.content_x-owner->scroll_x,y=box.y+owner->layout.content_y-owner->scroll_y;
            for(const auto& line:owner->layout.lines)for(const auto& item:line.items){
                if(!item.node||item.w<=0||item.h<=0)continue;RectF fragment=item.kind==LineItem::BoxItem&&item.box?item.box->viewport_rect():RectF{x+item.x,y+item.y,item.w,item.h};
                for(auto* target=item.node;target;target=target->parent){
                    // Enclosing inline elements receive the outer BoxItem, not its internals.
                    if(target!=item.node&&target->layout.has_box)break;
                    if(!target->is_element()||target->layout.has_box||target->style.display!=Display::Inline)continue;
                    if(!target->inline_layout_owner){target->inline_layout_owner=owner;target->visual_cache=fragment;}
                    else{auto& r=target->visual_cache;const float right=std::max(r.right(),fragment.right()),bottom=std::max(r.bottom(),fragment.bottom());r.x=std::min(r.x,fragment.x);r.y=std::min(r.y,fragment.y);r.w=right-r.x;r.h=bottom-r.y;target->inline_layout_owner=common(target->inline_layout_owner,owner);}
                }
            }
            for(auto* child:owner->layout.box_children)visit(child);
        };
        visit(doc->html);doc->inline_geometry_revision=doc->visual_revision;
    }
    return visual_cache_revision==doc->visual_revision?visual_cache:RectF{};
}

void Document::record_mutation(Node* node, const std::string& type, const std::string& name,
                               const std::string& old_value, bool old_present,
                               const std::vector<Node*>& added, const std::vector<Node*>& removed,
                               Node* previous, Node* next) {
    if (!capture_mutations || mutation_suppression || mutation_overflow || !node) return;
    bool interested = false;
    for (auto* p = node; p; p = p->parent)
        if (mutation_roots.count(p->id)) { interested = true; break; }
    if (!interested) return;
    if (old_value.size() > 2 * 1024 * 1024) {
        mutations.clear(); mutation_bytes = 0; mutation_overflow = true;
        return;
    }
    MutationChange m;
    m.target = node->id; m.type = type; m.name = name;
    m.old_value = old_value; m.old_present = old_present;
    m.previous = previous ? previous->id : 0;
    m.next = next ? next->id : 0;
    for (auto* p = node; p; p = p->parent) m.ancestors.push_back(p->id);
    for (auto* n : added) if (n) m.added.push_back(n->id);
    for (auto* n : removed) if (n) m.removed.push_back(n->id);
    size_t bytes = sizeof(m) + m.type.size() + m.name.size() + m.old_value.size() +
                   sizeof(int) * (m.ancestors.size() + m.added.size() + m.removed.size());
    if (mutations.size() >= 4096 || bytes > 2 * 1024 * 1024 ||
        mutation_bytes > 2 * 1024 * 1024 - bytes) {
        mutations.clear(); mutation_bytes = 0; mutation_overflow = true;
        return;
    }
    mutation_bytes += bytes;
    mutations.push_back(std::move(m));
}
