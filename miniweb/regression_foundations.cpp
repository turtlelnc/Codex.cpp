#include "src/engine.h"
#include "src/diagnostics.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>

void check_foundations(const std::filesystem::path& directory){
    Engine engine(640,800);engine.set_wait_ms(0);std::string error;
    Loader::set_access_root(directory.u8string());
    if(!engine.load((directory/"foundations.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    auto rect=[&](const char* id){auto* n=engine.document()->get_element_by_id(id);if(!n)throw std::runtime_error(id);return n->absolute_rect();};
    auto equal=[](float actual,float expected,const char* what){if(std::abs(actual-expected)>.1f)throw std::runtime_error(std::string(what)+": "+std::to_string(actual)+" != "+std::to_string(expected));};
    equal(rect("positive-b").y-rect("positive-a").bottom(),30,"Positive sibling margins");
    equal(rect("mixed-b").y-rect("mixed-a").bottom(),12,"Mixed sibling margins");
    equal(rect("negative-b").y-rect("negative-a").bottom(),-12,"Negative sibling margins");
    equal(rect("empty-b").y-rect("empty-a").bottom(),30,"Empty block margin strut");
    equal(rect("inline-b").y-rect("inline-a").bottom(),70,"Inline content separates margins");
    for(const auto& item:std::vector<std::pair<const char*,float>>{
        {"grow-a",60},{"grow-b",140},{"shrink-a",120},{"shrink-b",80},{"partial-a",50},{"partial-b",137.5},
        {"basis-a",100},{"basis-b",100},{"wrap-a",60},{"wrap-b",60}})
        equal(rect(item.first).w,item.second,"Constrained flex redistribution");
    equal(rect("wrap-a").y,rect("wrap-b").y,"Flex wrapping uses constrained hypothetical widths");
    for(const auto& item:std::vector<std::pair<const char*,float>>{
        {"percent-auto",20},{"percent-fixed",39},{"percent-zero",0},{"min-height",100},
        {"max-height",80},{"height-conflict",100},{"height-content-box",124},{"percent-bound",89}})
        equal(rect(item.first).h,item.second,"Percentage and constrained heights");
    equal(rect("font-stack").w,rect("font-mono").w,"Font family fallback list");
    equal(rect("percent-auto").y,rect("height-auto").y,"Indefinite percentage relative offset");
    equal(rect("font-shorthand").w,rect("font-mono").w,"Font shorthand fallback list");

    if(!engine.load((directory/"clipping.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    equal(rect("auto-parent").h,80,"Absolute child does not affect auto parent flow");
    equal(rect("bottom").y,rect("auto-parent").bottom()-20,"Bottom inset uses completed auto parent padding box");
    const auto& pixels=engine.canvas()->pixels();
    auto pixel=[&](int x,int y,int r,int g,int b){const auto i=(y*640+x)*4;
        if(pixels[i]!=r||pixels[i+1]!=g||pixels[i+2]!=b)throw std::runtime_error("Padding-box clipping pixel mismatch");};
    pixel(25,25,0,0,0);pixel(35,35,0,136,102);pixel(115,35,0,0,0);pixel(125,35,255,255,255);
    for(const auto& link:engine.links()){
        if(link.rect.x<30||link.rect.y<30||link.rect.right()>110||link.rect.bottom()>70)
            throw std::runtime_error("Clipped link retains invisible hit region");
    }
    if(engine.links().empty())throw std::runtime_error("Visible link disappeared");
    if(!engine.click(25,25)||engine.document()->body->attr("data-clicks")!="0")throw std::runtime_error("Border click reached clipped child");
    engine.click(125,35);
    if(engine.document()->body->attr("data-clicks")!="0")throw std::runtime_error("Outside click reached clipped child");
    if(!engine.click(35,35)||engine.document()->body->attr("data-clicks")!="1")throw std::runtime_error("Visible child click failed");

    if(!engine.load((directory/"foundation-limits.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    for(const char* feature:{"parent-child margin collapse","absolute containing block through static ancestors","flex automatic minimum size"})
        if(!diag::has(diag::Kind::Approximate,feature))throw std::runtime_error(std::string("Known limitation not reported: ")+feature);
}
