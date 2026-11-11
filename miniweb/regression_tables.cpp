#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>

static void load_table(Engine& engine,const std::filesystem::path& page){
    engine.set_wait_ms(0);std::string error;Loader::set_access_root(page.parent_path().u8string());
    if(!engine.load(page.u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
}
int table_geometry(const std::filesystem::path& path,int width){
    const auto page=std::filesystem::canonical(path);Engine engine(width,800);load_table(engine,page);
    std::cout<<'{';bool first=true;
    for(const char* id:{"report","head","header","h1","h2","h3","body","first","a1","b1","c1","add","tall","a2","b2","c2","foot","footer","f1","f2","f3",
        "auto","auto-body","auto-r1","auto-r2","auto-r3","x1","x2","x3","y1","y2","y3","outer","outer-row","outer-a","outer-b","inner","i1","i2","i3","i4","css","css-r1","css-r2","d1","d2","d3","d4","narrow","n1","n2","responsive","resp-a","resp-b","resp-c","resp-d"}){
        auto* node=engine.document()->get_element_by_id(id);if(!node)throw std::runtime_error("Missing table node");auto r=node->absolute_rect();
        if(!first)std::cout<<',';first=false;std::cout<<'"'<<id<<"\":["<<r.x<<','<<r.y<<','<<r.w<<','<<r.h<<']';
    }
    std::cout<<"}\n";return 0;
}
void check_tables(const std::filesystem::path& directory){
    Engine engine(640,800);load_table(engine,directory/"tables.html");auto* doc=engine.document();
    auto rect=[&](const char* id){auto* n=doc->get_element_by_id(id);if(!n)throw std::runtime_error("Missing table fixture node");return n->absolute_rect();};
    for(const auto& ids:std::vector<std::pair<const char*,const char*>>{{"h1","a1"},{"h2","b1"},{"h3","c1"},{"a1","f1"},{"x1","x2"},{"y1","y3"}}){
        auto a=rect(ids.first),b=rect(ids.second);if(std::abs(a.x-b.x)>.01||std::abs(a.w-b.w)>.01)throw std::runtime_error("Table columns are not shared across rows");}
    if(rect("h2").x<=rect("h1").x||rect("b1").y!=rect("a1").y||rect("tall").h!=80)
        throw std::runtime_error("Table cells stacked vertically or row height lost");
    if(doc->body->attr("data-table-display")!="table"||doc->body->attr("data-cell-display")!="table-cell"||doc->body->attr("data-spacing")!="0px")throw std::runtime_error("Computed table styles are inconsistent");
    if(rect("n1").h<50||rect("b1").h<70)throw std::runtime_error("Table cell text failed to wrap");
    Engine narrow(320,800);load_table(narrow,directory/"tables.html");
    if(std::abs(narrow.document()->get_element_by_id("responsive")->absolute_rect().w-288)>.01)throw std::runtime_error("Percentage table width failed at narrow viewport");
    if(doc->get_element_by_id("resp-d")->layout.lines.empty()||doc->get_element_by_id("resp-d")->layout.lines.front().y<9)throw std::runtime_error("Ungrouped HTML rows lost default middle alignment");
    const auto rows=doc->get_element_by_id("body")->layout.box_children.size();
    auto button=rect("add");if(!engine.click(button.x+button.w/2,button.y+button.h/2)||!engine.render(false))throw std::runtime_error("Cell button click failed");
    if(!doc->get_element_by_id("added")||doc->get_element_by_id("body")->layout.box_children.size()!=rows+1)throw std::runtime_error("Dynamic table row did not reflow");
    if(rect("footer").y<=doc->get_element_by_id("added")->absolute_rect().y)throw std::runtime_error("Footer failed to move after inserted row");
}
