#include "src/diagnostics.h"
#include "src/engine.h"
#include <filesystem>
#include <stdexcept>
#include <string>

static void load_diagnostic_page(Engine& engine, const std::filesystem::path& page) {
    engine.set_wait_ms(0);
    std::string error;
    Loader::set_access_root(page.parent_path().u8string());
    if (!engine.load(page.u8string(), &error) || !engine.render(false)) throw std::runtime_error(error);
}

void check_diagnostics(const std::filesystem::path& directory) {
    using diag::Kind;
    Engine engine(640, 800);
    load_diagnostic_page(engine, directory / "diagnostics.html");
    auto has = [](Kind kind, const char* part) { return diag::has(kind, part); };
    struct Expected { Kind kind; const char* feature; const char* message; };
    const Expected expected[] = {
        {Kind::IgnoredCss, "filter", "filter was not reported"},
        {Kind::IgnoredCss, "text-shadow", "text-shadow was not reported"},
        {Kind::IgnoredCss, "vendor-prefixed property", "vendor-prefixed property was not reported"},
        {Kind::Approximate, "display:contents", "display:contents was not reported"},
        {Kind::Approximate, "position:sticky", "position:sticky was not reported"},
        {Kind::Approximate, "transform", "unsupported transform was not reported"},
        {Kind::Approximate, "automatic table column distribution", "approximate table distribution was not reported"},
        {Kind::Approximate, "auto-width table nested in a cell", "nested auto table was not reported"},
        {Kind::Structural, "collapsed table borders", "border-collapse:collapse was not reported"},
        {Kind::Structural, "table caption", "table caption was not reported"},
        {Kind::Structural, "col/colgroup", "col/colgroup was not reported"},
        {Kind::Structural, "rowspan/colspan", "rowspan/colspan was not reported"},
        {Kind::Structural, "inline-table", "inline-table was not reported"},
        {Kind::MissingJs, "attachShadow", "attachShadow was not reported"},
        {Kind::MissingJs, "scrollIntoView", "scrollIntoView was not reported"},
        {Kind::MissingJs, "window.scrollTo", "window.scrollTo was not reported"},
        {Kind::MissingJs, "IntersectionObserver", "IntersectionObserver was not reported"},
        {Kind::MissingJs, "ResizeObserver", "ResizeObserver was not reported"},
        {Kind::MissingJs, "undefinedGlobalApi", "undefined global was not reported"},
        {Kind::MissingJs, "unknown function call", "unknown method call was not reported"},
    };
    for (const auto& item : expected)
        if (!has(item.kind, item.feature)) throw std::runtime_error(item.message);
    const std::string summary = diag::report().summary();
    if (summary.find("Capability report:") != 0 || summary.find("not Chromium verification") == std::string::npos)
        throw std::runtime_error("Capability summary is not actionable");
    const std::string json = diag::report().json();
    for (const char* key : {"\"ignored_css\"", "\"approximate\"", "\"structural\"", "\"missing_js\"", "\"truncated\""})
        if (json.find(key) == std::string::npos) throw std::runtime_error(std::string("Capability JSON missing ") + key);

    // A page that only uses the supported subset must not produce false alarms.
    Engine clean(640, 800);
    load_diagnostic_page(clean, directory / "diagnostics-clean.html");
    if (!diag::report().empty()) throw std::runtime_error("Clean page produced: " + diag::report().summary());

    Engine bounds(640,800);load_diagnostic_page(bounds,directory/"box-bounds.html");
    for(const auto& item:std::vector<std::pair<const char*,float>>{{"max",200},{"min",100},{"conflict",100},{"content",224}})
        if(std::abs(bounds.document()->get_element_by_id(item.first)->absolute_rect().w-item.second)>.02f)
            throw std::runtime_error(std::string("Box sizing min/max mismatch: ")+item.first);
    // Exercise a real report's responsive columns and actual DOM click state.
    for(int width:{1280,390}){
        Engine design(width,844);load_diagnostic_page(design,directory/"design-report.html");
        auto* doc=design.document();
        auto rect=[&](const char* id){return doc->get_element_by_id(id)->absolute_rect();};
        const auto main=rect("main"),overview=rect("overview"),progress=rect("progress"),next=rect("next");
        if(std::abs(main.w-std::min(width,960))>.02f||std::abs(main.x-(width-main.w)/2)>.02f)
            throw std::runtime_error("Border-box max-width includes padding twice");
        if(width>600?(std::abs(progress.y-next.y)>.02f||next.x<=progress.x+progress.w):
                     (next.y<progress.y+progress.h||std::abs(next.w-overview.w)>.02f))
            throw std::runtime_error("Design report responsive columns failed");
        const auto button=rect("review");
        for(const char* expected:{"true","false"}){
            if(!design.click(button.x+4,button.y+4)||!design.render(false)||
               doc->get_element_by_id("review")->attr("aria-expanded")!=expected)
                throw std::runtime_error("Design report checklist toggle failed");
        }
    }
}
