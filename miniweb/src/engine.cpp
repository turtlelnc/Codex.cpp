#include "engine.h"
#include "html.h"
#include "js.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <functional>

Engine::Engine(int viewport_w, int viewport_h, const std::vector<std::string>& extra_fonts)
    : viewport_w_(std::max(1, viewport_w)), viewport_h_(std::max(1, viewport_h)), extra_fonts_(extra_fonts),
      images_(nullptr) {
    fonts_.init(extra_fonts_);
}

Engine::~Engine() = default;

bool Engine::load(const std::string& url_or_path, std::string* error) {
    std::string url = url_or_path;
    if (util::url_scheme(url).empty()) {
        std::string p = util::absolute_path(url);
        if (util::is_directory(p)) p = util::path_join(p, "index.html");
        url = util::path_to_file_url(p);
    }
    if (!util::url_allowed(url)) {
        if (error) *error = "blocked URL (only file://, data: and loopback http:// are allowed)";
        return false;
    }
    auto loader = std::make_unique<Loader>(url);
    LoadResult page = loader->load_absolute(url);
    if (!page.ok) {
        if (error) *error = page.error;
        return false;
    }
    auto doc = std::make_unique<Document>();
    doc->url = url;
    doc->base_url = url;
    doc->viewport_w = viewport_w_;
    doc->viewport_h = viewport_h_;
    parse_html_document(*doc, page.data);
    if (doc->base_url.empty()) doc->base_url = url;
    loader->set_base_url(doc->base_url);

    js_runtime_.reset();
    doc_ = std::move(doc);
    loader_ = std::move(loader);
    images_.clear();
    stylesheet_cache_.clear();
    images_.set_loader(loader_.get());
    url_ = url;
    title_ = doc_->title;
    js_ran_ = false;
    links_.clear();
    canvas_.reset();
    collect_stylesheets();
    apply_styles();
    do_layout();
    return true;
}

void Engine::collect_stylesheets() {
    sheets_.clear();
    if (!doc_ || !loader_) return;
    int order = 0;
    // Walk the live tree, not parser-time lists: scripts may insert/move styles.
    std::function<void(Node*)> visit=[&](Node* node){
        if(!node||node->is_element("template"))return;
        if((node->is_element("style")||node->is_element("link"))&&!css_media_matches(node->attr("media"),viewport_w_,viewport_h_))return;
        if(node->is_element("style")){
            const auto text=node->text_content();if(!text.empty())sheets_.push_back(parse_css(text,doc_->base_url.empty()?doc_->url:doc_->base_url,viewport_w_,viewport_h_,&order));
        }else if(node->is_element("link")&&!node->has_attr("disabled")){
            auto rel=util::split_ws(util::to_lower(node->attr("rel")));
            if(std::find(rel.begin(),rel.end(),"stylesheet")!=rel.end()&&!node->attr("href").empty()){
                const auto url=doc_->resolve(node->attr("href"));auto cached=stylesheet_cache_.find(url);
                if(cached==stylesheet_cache_.end()){
                    auto loaded=loader_->load_absolute(url);
                    if(loaded.ok)cached=stylesheet_cache_.emplace(url,std::move(loaded.data)).first;
                    else util::warn("stylesheet load failed (%s): %s",url.c_str(),loaded.error.c_str());
                }
                if(cached!=stylesheet_cache_.end())sheets_.push_back(parse_css(cached->second,url,viewport_w_,viewport_h_,&order));
            }
        }
        for(auto* child:node->children)visit(child);
    };
    visit(doc_->root);
}

void Engine::apply_styles() {
    if (!doc_) return;
    apply_cascade(*doc_, sheets_, viewport_w_, viewport_h_);
}

void Engine::do_layout() {
    if (!doc_ || !doc_->root) return;
    LayoutContext ctx;
    ctx.doc = doc_.get();
    ctx.fonts = &fonts_;
    ctx.images = &images_;
    ctx.viewport_w = (float)viewport_w_;
    ctx.viewport_h = (float)viewport_h_;
    layout_document(*doc_, ctx);
    layout_revision_=doc_->revision;
}

void Engine::ensure_layout(){if(doc_&&layout_revision_!=doc_->revision){doc_->recompute_head_body();collect_stylesheets();apply_styles();do_layout();}}

bool Engine::render(bool full_page) {
    if (!doc_ || !loader_) return false;
    if (!js_ran_) {
        auto js = std::make_unique<JsRuntime>(*doc_, *loader_, viewport_w_, viewport_h_,[this](){ensure_layout();});
        if (js->init()) {
            js->run(wait_ms_);
            doc_->recompute_head_body();
            collect_stylesheets();
            apply_styles();
            do_layout();
            js_runtime_ = std::move(js);
        }
        js_ran_ = true;
    }
    content_height_ = (int)std::ceil(document_content_height(*doc_));
    if (content_height_ <= 0) content_height_ = 1;
    content_height_ = std::min(content_height_, 20000);
    int out_h = full_page ? std::max(viewport_h_, content_height_) : viewport_h_;
    canvas_ = std::make_unique<Canvas>(viewport_w_, out_h);
    paint_document(*canvas_, *doc_, fonts_, &images_);
    links_.clear();
    Node* start = doc_->html ? doc_->html : (doc_->root ? doc_->root : nullptr);
    if (start) collect_links(start, 0, 0);
    title_ = doc_->title;
    return true;
}

bool Engine::click(float x, float y) {
    if (!js_runtime_ || !doc_ || !doc_->html) return false;
    Node* target = hit_test(doc_->html, 0, 0, x, y);
    if (!target) return false;
    std::string code = "__mwDispatchClick(" + std::to_string(target->id) + "," +
                       util::format_float(x, 2) + "," + util::format_float(y, 2) + ");";
    if (!js_runtime_->eval(code, "<viewer-click>", false)) return false;
    js_runtime_->drain_timers(500);
    collect_stylesheets();
    apply_styles();
    do_layout();
    return true;
}

Node* Engine::hit_test(Node* n, float ox, float oy, float px, float py) {
    if (!n || !n->is_element() || !n->layout.has_box || n->style.display==Display::None || n->style.visibility!=Visibility::Visible) return nullptr;
    const LayoutInfo& L = n->layout;
    RectF box=visual_box(n,ox,oy);
    if(n->style.overflow==Overflow::Hidden&&!box.contains(px,py))return nullptr;
    float cx = box.x + L.content_x;
    float cy = box.y + L.content_y;
    const auto ordered=paint_order(n);
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        if (Node* hit = hit_test(*it, cx, cy, px, py)) return hit;
    }
    for (auto& line : L.lines) {
        for (auto& item : line.items) {
            if (item.w <= 0 || item.h <= 0) continue;
            RectF r{cx + item.x, cy + item.y, item.w, item.h};
            if (!r.contains(px, py)) continue;
            Node* target = item.node;
            if (target && (target->is_text() || target->is_comment())) target = target->parent;
            if (target && target->is_element()) return target;
            if (item.link) return item.link;
        }
    }
    if (box.contains(px, py)) return n;
    return nullptr;
}

void Engine::collect_links(Node* n, float ox, float oy) {
    if (!n || !n->is_element() || !n->layout.has_box) return;
    const LayoutInfo& L = n->layout;
    RectF box=visual_box(n,ox,oy);
    float cx = box.x + L.content_x;
    float cy = box.y + L.content_y;
    auto add_link = [&](const RectF& r, Node* a) {
        if (!a || !a->has_attr("href") || r.w <= 0 || r.h <= 0) return;
        std::string raw = a->attr("href");
        if (raw.empty() || raw[0] == '#') return;
        std::string low = util::to_lower(raw);
        if (util::starts_with(low, "javascript:") || util::starts_with(low, "mailto:") ||
            util::starts_with(low, "tel:"))
            return;
        LinkHit hit;
        hit.rect = r;
        hit.href = doc_->resolve(raw);
        links_.push_back(std::move(hit));
    };
    if (n->is_element("a") && n->has_attr("href")) {
        add_link(box, n);
    }
    for (auto& line : L.lines) {
        for (auto& item : line.items) {
            if (item.link && item.w > 0 && item.h > 0) {
                add_link(RectF{cx + item.x, cy + item.y, item.w, item.h}, item.link);
            }
        }
    }
    for (auto* child : L.box_children) collect_links(child, cx, cy);
}
