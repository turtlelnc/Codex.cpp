#include "engine.h"
#include "diagnostics.h"
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
    diag::report().reset();
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
                    // Keep failed loads for this document, just like successful loads.
                    std::optional<std::string> source;
                    if (loaded.ok) source = std::move(loaded.data);
                    else util::warn("stylesheet load failed (%s): %s", url.c_str(), loaded.error.c_str());
                    cached = stylesheet_cache_.emplace(url, std::move(source)).first;
                }
                if (cached->second)
                    sheets_.push_back(parse_css(*cached->second, url, viewport_w_, viewport_h_, &order));
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
    update_scroll_layout(*doc_);
    layout_revision_=doc_->revision;
}

void Engine::ensure_layout(){if(doc_&&layout_revision_!=doc_->revision){doc_->recompute_head_body();collect_stylesheets();apply_styles();do_layout();}}

bool Engine::render(bool full_page) {
    if (!doc_ || !loader_) return false;
    if (!js_ran_) {
        // The load pass already applied styles and laid out once; start the
        // report at the rendering pass so every feature is counted once.
        diag::report().reset();
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
    if(js_runtime_ && !js_runtime_->eval("__mwResumeMutations();__mwReconcileFocus();","<focus-reconcile>"))return false;
    ensure_layout();
    if(!settle_frame())return false;
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
    if (!js_runtime_ || !doc_ || !doc_->html || !std::isfinite(x) || !std::isfinite(y) || x<0 || y<0 || x>=viewport_w_ || y>=viewport_h_) return false;
    Node* target = hit_test(doc_->html, 0, 0, x, y);
    if (!target) return interact("__mwDispatchBackground("+util::format_float(x,2)+","+util::format_float(y,2)+");");
    const bool fallback_summary = target->is_element("details") &&
        std::any_of(target->layout.box_children.begin(), target->layout.box_children.end(),
            [&](const Node* child) {
                return child->anonymous && child->is_element("summary") && child->viewport_rect().contains(x, y);
            });
    std::string code = "__mwDispatchClick(" + std::to_string(target->id) + "," +
                       util::format_float(x, 2) + "," + util::format_float(y, 2) + (fallback_summary ? ",true);" : ",false);");
    return interact(code);
}

bool Engine::interact(const std::string& code) {
    if (!js_runtime_ || !js_runtime_->eval("__mwResumeMutations();__mwReconcileFocus();"+code, "<viewer-interaction>", false)) return false;
    js_runtime_->drain_timers(500);
    if(!js_runtime_->eval("__mwReconcileFocus();","<focus-reconcile>"))return false;
    collect_stylesheets();
    apply_styles();
    do_layout();
    return settle_frame();
}

bool Engine::settle_frame() {
    if (!flush_scroll_events()) return false;
    if (js_runtime_ && !js_runtime_->eval("__mwResizeFrame();", "<resize-observers>", false))
        return false;
    ensure_layout();
    return flush_scroll_events();
}

bool Engine::flush_scroll_events(){
    if(!js_runtime_)return true;
    for(int pass=0;pass<8&&!doc_->pending_scroll_events.empty();++pass){
        if(!js_runtime_->eval("__mwFlushScrollEvents();__mwReconcileFocus();","<scroll-events>",false))return false;
        js_runtime_->drain_timers(0);ensure_layout();
    }
    if(!doc_->pending_scroll_events.empty())diag::approximate("scroll event feedback deferred","More than eight callback-induced scroll rounds; remaining events deferred to the next action/frame");
    return true;
}

bool Engine::type(const std::string& text) {
    if (text.size() > 8192) return false;
    return interact("__mwType(\"" + diag::detail::json_escape(text) + "\");");
}
bool Engine::key(const std::string& key) {
    static const std::vector<std::string> keys={"Tab","Shift+Tab","Enter"," ","Backspace","Delete","ArrowLeft","ArrowRight","Home","End","Escape"};
    if (std::find(keys.begin(),keys.end(),key)==keys.end()) return false;
    return interact("__mwKey(\"" + diag::detail::json_escape(key) + "\");");
}
bool Engine::advance(int milliseconds) {
    if (!js_runtime_ || milliseconds<0 || milliseconds>3000) return false;
    js_runtime_->drain_timers(milliseconds);
    if(!js_runtime_->eval("__mwResumeMutations();__mwReconcileFocus();","<focus-reconcile>"))return false;
    ensure_layout(); return settle_frame();
}

bool Engine::scroll(float x,float y,float dx,float dy) {
    if(!doc_||!js_runtime_||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(dx)||!std::isfinite(dy)||x<0||y<0||x>=viewport_w_||y>=viewport_h_||std::abs(dx)>10000||std::abs(dy)>10000)return false;
    ensure_layout();auto* target=hit_test(doc_->html,0,0,x,y);
    return interact("__mwScroll("+std::to_string(target?target->id:0)+","+util::format_float(dx,2)+","+util::format_float(dy,2)+");");
}

std::string Engine::inspect(const std::string& element_id) {
    ensure_layout();
    auto quote = [](const std::string& value) {
        return "\"" + diag::detail::json_escape(value) + "\"";
    };
    auto bounded = [](std::string value, size_t limit) {
        size_t end = 0;
        while (end < value.size()) {
            uint32_t cp = 0;
            const size_t next = util::utf8_next(value, end, &cp);
            if (next > limit) break;
            end = next;
        }
        value.resize(end);
        return value;
    };
    auto number = [](float value) { return util::format_float(value, 2); };
    std::string out = "{\"scroll_x\":" + number(doc_->scroll_x) +
        ",\"scroll_y\":" + number(doc_->scroll_y);
    const auto active = doc_->focused_id;
    const auto* focus = active > 0 && static_cast<size_t>(active) < doc_->id_map.size()
        ? doc_->id_map[active] : nullptr;
    out += ",\"focused_id\":" + quote(focus ? bounded(focus->attr("id"), 128) : "");
    auto center = [&](const Node* node, const RectF& rect) {
        const float x = rect.x + rect.w / 2, y = rect.y + rect.h / 2;
        auto* hit = rect.w > 0 && rect.h > 0 && x >= 0 && y >= 0 && x < viewport_w_ && y < viewport_h_
            ? hit_test(doc_->html, 0, 0, x, y) : nullptr;
        while (hit && hit != node) hit = hit->parent;
        return hit != nullptr;
    };
    auto rectangle = [&](const RectF& rect) {
        return "[" + number(rect.x) + "," + number(rect.y) + "," + number(rect.w) + "," + number(rect.h) + "]";
    };
    auto checked = [](const Node& node, const std::string& type) -> std::string {
        if (!node.is_element("input") || (type != "checkbox" && type != "radio")) return "";
        const bool value = node.checked_dirty ? node.checked_value : node.has_attr("checked");
        return value ? ",\"checked\":true" : ",\"checked\":false";
    };
    if (element_id.empty()) {
        std::string controls, status;
        size_t control_count = 0, status_count = 0, hit_tests = 0;
        bool truncated = false;
        for (const auto& item : doc_->arena) {
            if (!item.is_element()) continue;
            const std::string type = util::to_lower(item.attr("type"));
            const auto role = item.attr("role");
            const bool control = item.is_element("button") || item.is_element("textarea") ||
                item.is_element("select") || (item.is_element("input") && type != "hidden") ||
                (item.is_element("a") && item.has_attr("href"));
            const bool result = item.is_element("output") || role == "status" || role == "alert";
            if (!control && !result) continue;
            const auto rect = item.client_rect();
            if (rect.w <= 0 || rect.h <= 0 || rect.x + rect.w / 2 < 0 || rect.y + rect.h / 2 < 0 ||
                rect.x + rect.w / 2 >= viewport_w_ || rect.y + rect.h / 2 >= viewport_h_) continue;
            if (hit_tests++ == 64) {
                truncated = true;
                break;
            }
            if (!center(&item, rect)) continue;
            if (control && control_count < 12) {
                auto label = item.attr("aria-label");
                if (label.empty()) {
                    if (const auto* parent = item.closest("label")) label = parent->text_content();
                    else if (item.is_element("button") || item.is_element("a")) label = item.text_content();
                    else if (item.is_element("input") && (type == "submit" || type == "button" || type == "reset"))
                        label = item.attr("value");
                    else label = item.attr("placeholder");
                }
                if (control_count++) controls += ',';
                controls += "{\"tag\":" + quote(item.tag) + ",\"type\":" + quote(bounded(type, 24)) +
                    ",\"label\":" + quote(bounded(util::trim(label), 64)) +
                    ",\"id\":" + quote(bounded(item.attr("id"), 64)) +
                    ",\"focused\":" + (&item == focus ? "true" : "false") +
                    ",\"disabled\":" + (item.form_disabled() || item.attr("aria-disabled") == "true" ? "true" : "false");
                controls += checked(item, type);
                controls += ",\"rect\":" + rectangle(rect) + "}";
            } else if (control) truncated = true;
            if (result && !control && status_count < 4) {
                if (status_count++) status += ',';
                status += quote(bounded(util::trim(item.text_content()), 128));
            } else if (result && !control) truncated = true;
        }
        return out + ",\"controls\":[" + controls + "],\"status\":[" + status +
            "],\"truncated\":" + (truncated ? "true}" : "false}");
    }
    const auto* node = doc_->get_element_by_id(element_id);
    if (!node) return out + ",\"element\":null}";
    const auto rect = node->client_rect();
    // Return only bounded DOM text. Live input values are deliberately omitted.
    std::string label = node->is_element("input") || node->is_element("textarea") ? "" : node->text_content();
    return out + ",\"element\":{\"tag\":" + quote(node->tag) +
        ",\"text\":" + quote(bounded(std::move(label), 512)) + ",\"center_hit\":" + (center(node, rect) ? "true" : "false") +
        ",\"scroll_x\":" + number(node->scroll_x) + ",\"scroll_y\":" + number(node->scroll_y) +
        ",\"rect\":" + rectangle(rect) + checked(*node, util::to_lower(node->attr("type"))) + "}}";
}

Node* Engine::hit_test(Node* n, float ox, float oy, float px, float py) {
    if (!n || !n->is_element() || !n->layout.has_box || n->style.display==Display::None) return nullptr;
    const LayoutInfo& L = n->layout;
    RectF box=n->viewport_rect();
    // A clipped descendant cannot paint or receive clicks over the border.
    // The border itself still belongs to this element.
    if(n->style.overflow!=Overflow::Visible&&!overflow_clip(n,box).contains(px,py))
        return n->style.visibility==Visibility::Visible&&box.contains(px,py)?n:nullptr;
    float cx = box.x + L.content_x - n->scroll_x;
    float cy = box.y + L.content_y - n->scroll_y;
    const auto ordered=paint_order(n);
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        if (Node* hit = hit_test(*it, cx, cy, px, py)) return hit;
    }
    for (auto& line : L.lines) {
        for (auto& item : line.items) {
            if (item.w <= 0 || item.h <= 0 || !item.style || item.style->visibility != Visibility::Visible) continue;
            RectF r{cx + item.x, cy + item.y, item.w, item.h};
            if (!r.contains(px, py)) continue;
            Node* target = item.node;
            if (target && (target->is_text() || target->is_comment())) target = target->parent;
            if (target && target->is_element()) return target->anonymous ? target->parent : target;
            if (item.link) return item.link;
        }
    }
    if (n->style.visibility == Visibility::Visible && box.contains(px, py)) return n->anonymous ? n->parent : n;
    return nullptr;
}

void Engine::collect_links(Node* n, float ox, float oy, const RectF* clip) {
    if (!n || !n->is_element() || !n->layout.has_box) return;
    const LayoutInfo& L = n->layout;
    RectF box=n->viewport_rect();
    float cx = box.x + L.content_x - n->scroll_x;
    float cy = box.y + L.content_y - n->scroll_y;
    auto add_link = [&](RectF r, Node* a) {
        if(clip){const float right=std::min(r.right(),clip->right()),bottom=std::min(r.bottom(),clip->bottom());
            r.x=std::max(r.x,clip->x);r.y=std::max(r.y,clip->y);r.w=right-r.x;r.h=bottom-r.y;}
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
    if (n->style.visibility==Visibility::Visible&&n->is_element("a") && n->has_attr("href")) {
        add_link(box, n);
    }
    RectF child_clip;
    if(n->style.overflow!=Overflow::Visible){
        child_clip=overflow_clip(n,box);
        if(clip){const float right=std::min(child_clip.right(),clip->right()),bottom=std::min(child_clip.bottom(),clip->bottom());
            child_clip.x=std::max(child_clip.x,clip->x);child_clip.y=std::max(child_clip.y,clip->y);
            child_clip.w=std::max(0.0f,right-child_clip.x);child_clip.h=std::max(0.0f,bottom-child_clip.y);}
        clip=&child_clip;
    }
    for (auto& line : L.lines) {
        for (auto& item : line.items) {
            if (item.style&&item.style->visibility==Visibility::Visible&&item.link && item.w > 0 && item.h > 0) {
                add_link(RectF{cx + item.x, cy + item.y, item.w, item.h}, item.link);
            }
        }
    }
    for (auto* child : L.box_children) collect_links(child, cx, cy,clip);
}
