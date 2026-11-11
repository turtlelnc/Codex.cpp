#pragma once
#include "canvas.h"
#include "css.h"
#include "dom.h"
#include "font.h"
#include "image.h"
#include "layout.h"
#include "loader.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

class JsRuntime;

struct LinkHit {
    RectF rect;
    std::string href;
};

class Engine {
public:
    Engine(int viewport_w, int viewport_h, const std::vector<std::string>& extra_fonts = {});
    ~Engine();

    bool load(const std::string& url_or_path, std::string* error = nullptr);
    bool render(bool full_page = true);
    bool click(float x, float y);
    void set_wait_ms(int ms) { wait_ms_ = ms; }
    const std::string& url() const { return url_; }
    const std::string& title() const { return title_; }
    const std::vector<LinkHit>& links() const { return links_; }
    Canvas* canvas() { return canvas_.get(); }
    int content_height() const { return content_height_; }
    Document* document() { return doc_.get(); }

private:
    int viewport_w_ = 1280, viewport_h_ = 800;
    int wait_ms_ = 2500;
    int content_height_ = 0;
    bool js_ran_ = false;
    std::string url_, title_;
    std::vector<std::string> extra_fonts_;

    std::unique_ptr<Document> doc_;
    std::unique_ptr<Loader> loader_;
    std::unique_ptr<JsRuntime> js_runtime_;
    FontManager fonts_;
    ImageCache images_;
    std::vector<Stylesheet> sheets_;
    std::unordered_map<std::string,std::string> stylesheet_cache_;
    std::unique_ptr<Canvas> canvas_;
    std::vector<LinkHit> links_;

    void collect_stylesheets();
    void apply_styles();
    void do_layout();
    void collect_links(Node* n, float ox, float oy);
    Node* hit_test(Node* n, float ox, float oy, float px, float py);
};
