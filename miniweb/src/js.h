#pragma once
#include "dom.h"
#include "loader.h"
#include "util.h"
#include <string>
#include <functional>

class JsRuntime {
public:
    JsRuntime(Document& doc, Loader& loader, int viewport_w, int viewport_h, std::function<void()> refresh = {});
    ~JsRuntime();

    bool init();
    void refresh_layout(){if(refresh_)refresh_();}
    void run(int wait_ms = 2500);
    void execute_scripts();
    void dispatch_dom_content_loaded();
    void dispatch_load();
    void drain_timers(int virtual_ms);
    bool eval(const std::string& code, const std::string& filename, bool module = false);
    bool should_interrupt() const {
        return deadline_ms_ > 0 && util::now_ms() > deadline_ms_;
    }
    struct Bridge;

private:
    std::function<void()> refresh_;
    Document& doc_;
    Loader& loader_;
    int vw_ = 1280, vh_ = 800;
    Bridge* bridge_ = nullptr;
    void* rt_ = nullptr;
    void* ctx_ = nullptr;
    double deadline_ms_ = 0;

    bool install_bridge();
    void drain_jobs();
    void dump_exception(const char* where);
};
