#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <sstream>

static void load_table(Engine& engine,const std::filesystem::path& page){
    engine.set_wait_ms(0);std::string error;Loader::set_access_root(page.parent_path().u8string());
    if(!engine.load(page.u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
}
// Test-only probe: dump absolute geometry for arbitrary fixture ids at a viewport width.
// clicks is a semicolon-separated list of "x,y" viewport coordinates replayed in order.
int geometry_probe(const std::filesystem::path& path,int width,const std::string& ids_csv,const std::string& clicks){
    const auto page=std::filesystem::canonical(path);Engine engine(width,800);load_table(engine,page);
    std::stringstream click_stream(clicks);std::string click;
    while(std::getline(click_stream,click,';')){
        if(click.empty())continue;
        const auto comma=click.find(',');if(comma==std::string::npos)throw std::runtime_error("Bad probe click: "+click);
        const float x=(float)std::atof(click.substr(0,comma).c_str()),y=(float)std::atof(click.substr(comma+1).c_str());
        if(!engine.click(x,y)||!engine.render(false))throw std::runtime_error("probe click failed");
    }
    std::vector<std::string> ids;std::stringstream stream(ids_csv);std::string id;
    while(std::getline(stream,id,','))if(!id.empty())ids.push_back(id);
    std::cout<<'{';bool first=true;
    for(const auto& name:ids){
        auto* node=engine.document()->get_element_by_id(name.c_str());if(!node)throw std::runtime_error("Missing probe node: "+name);
        auto r=node->absolute_rect();
        if(!first)std::cout<<',';first=false;std::cout<<'"'<<name<<"\":["<<r.x<<','<<r.y<<','<<r.w<<','<<r.h<<']';
    }
    if(auto* body=engine.document()->body;body&&body->has_attr("data-measure"))
        std::cout<<",\"_measure\":\""<<body->attr("data-measure")<<'"';
    std::cout<<",\"_nodes\":\""<<engine.document()->arena.size()<<'"';
    std::cout<<"}\n";return 0;
}
int table_geometry(const std::filesystem::path& path,int width){
    const auto page=std::filesystem::canonical(path);Engine engine(width,800);load_table(engine,page);
    std::cout<<'{';bool first=true;
    for(const char* id:{"report","head","header","h1","h2","h3","body","first","a1","b1","c1","add","tall","a2","b2","c2","foot","footer","f1","f2","f3",
        "auto","auto-body","auto-r1","auto-r2","auto-r3","x1","x2","x3","y1","y2","y3","outer","outer-row","outer-a","outer-b","inner","i1","i2","i3","i4","css","css-r1","css-r2","d1","d2","d3","d4","narrow","n1","n2","responsive","resp-a","resp-b","resp-c","resp-d",
        "shrinkwrap","shrink","shrinkrow","sh1","sh2","sh3","sh4","pctfix","pctfixrow","pf1","pf2","pf3","pf4","constrain","constrainrow","co1","co2","lh","lhrow","lh1","lh2","flowwrap","flowa","flowb","absfit","absrow","af1","af2","af3","af4"}){
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
    if(doc->body->attr("data-table-display")!="table"||doc->body->attr("data-cell-display")!="table-cell"||doc->body->attr("data-spacing")!="0px"||
       doc->body->attr("data-layout")!="fixed"||doc->body->attr("data-valign")!="middle")throw std::runtime_error("Computed table styles are inconsistent");
    if(rect("n1").h<50||rect("b1").h<70)throw std::runtime_error("Table cell text failed to wrap");
    Engine narrow(320,800);load_table(narrow,directory/"tables.html");
    if(std::abs(narrow.document()->get_element_by_id("responsive")->absolute_rect().w-288)>.01)throw std::runtime_error("Percentage table width failed at narrow viewport");
    if(doc->get_element_by_id("resp-d")->layout.lines.empty()||doc->get_element_by_id("resp-d")->layout.lines.front().y<9)throw std::runtime_error("Ungrouped HTML rows lost default middle alignment");
    const auto rows=doc->get_element_by_id("body")->layout.box_children.size();
    auto button=rect("add");if(!engine.click(button.x+button.w/2,button.y+button.h/2)||!engine.render(false))throw std::runtime_error("Cell button click failed");
    if(!doc->get_element_by_id("added")||doc->get_element_by_id("body")->layout.box_children.size()!=rows+1)throw std::runtime_error("Dynamic table row did not reflow");
    if(rect("footer").y<=doc->get_element_by_id("added")->absolute_rect().y)throw std::runtime_error("Footer failed to move after inserted row");
    auto near=[](float a,float b){return std::abs(a-b)<.6f;};
    // A specified cell width is a preference; it must not make the table overflow its wrapper.
    if(!near(rect("shrink").w,300)||!near(rect("sh1").w,74.5f)||!near(rect("sh2").x,85.5f)||!near(rect("sh3").x,160)||!near(rect("sh4").x,234.5f))
        throw std::runtime_error("Auto table did not shrink specified cell widths to fit");
    // Percentage columns are fitted into the space left after border spacing.
    if(!near(rect("pctfix").w,300)||!near(rect("pf1").w,64.5f)||!near(rect("pf4").x,236.5f))
        throw std::runtime_error("Fixed percentage columns overflowed border spacing");
    // Cell min/max constraints must not detach the cell box from its column.
    if(!near(rect("co1").w,119)||!near(rect("co2").x,rect("co1").x+rect("co1").w)||!near(rect("co2").w,119))
        throw std::runtime_error("Cell constraints detached the cell box from its column");
    // A line-height smaller than the font metrics must not inflate a row.
    if(!near(rect("lhrow").h,30)||!near(rect("lh").h,32))throw std::runtime_error("Small line-height inflated the table row");
    // Static boxes ignore top/left without changing block flow.
    if(!near(rect("flowa").y,rect("flowwrap").y)||!near(rect("flowb").y-rect("flowa").y,20))
        throw std::runtime_error("Static top offset shifted block flow");
    // An absolutely positioned auto-width table shrink-fits into the inset space.
    if(!near(rect("absfit").w,120)||!near(rect("af1").w,29.5f)||!near(rect("af4").x,609.5f))
        throw std::runtime_error("Absolutely positioned table ignored left/right insets");
}

void check_dynamic_tables(const std::filesystem::path& directory){
    Engine engine(640,800);load_table(engine,directory/"tables-dynamic.html");auto* doc=engine.document();
    auto rect=[&](const char* id){auto* n=doc->get_element_by_id(id);if(!n)throw std::runtime_error("Missing dynamic table node");return n->absolute_rect();};
    auto press=[&](const char* id){const auto r=rect(id);if(!engine.click(r.x+r.w/2,r.y+r.h/2)||!engine.render(false))throw std::runtime_error(std::string("click failed: ")+id);};
    auto near=[](float a,float b){return std::abs(a-b)<.6f;};
    const float footer=rect("df").y,height=rect("df").h;
    const size_t body_rows=doc->get_element_by_id("db")->layout.box_children.size();
    press("addBtn");
    auto* added=doc->get_element_by_id("added1");
    if(!added||!near(rect("df").y,footer+height)||!near(rect("df").h,height))throw std::runtime_error("Inserted row did not reflow the footer");
    // getBoundingClientRect inside the click handler must observe the new layout.
    {const auto measured=doc->body->attr("data-measure");const auto want=[](float v){return std::to_string((int)std::lround(v));};
     const std::string expected=want(rect("df").y)+","+want(rect("df").h)+","+want(rect("mv2a").x)+","+want(rect("mv2a").y)+","+want(rect("mv2a").h);
     if(measured!=expected)throw std::runtime_error("Stale geometry inside click handler: "+measured+" expected "+expected);
     if(doc->body->attr("data-added-display")!="table-row"||doc->body->attr("data-display")!="table-row"||doc->body->attr("data-added-height")!="30")
         throw std::runtime_error("Stale computed style inside click handler");}
    press("addBtn");press("delBtn");
    if(doc->get_element_by_id("added2")||!near(rect("df").y,footer+height))throw std::runtime_error("Removed row left stale geometry");
    press("delBtn");
    if(doc->get_element_by_id("added1")||doc->get_element_by_id("db")->layout.box_children.size()!=body_rows||!near(rect("df").y,footer))
        throw std::runtime_error("Deleting rows did not restore the table");
    press("hideBtn");
    if(!near(rect("mv").h,32)||rect("mv2").w!=0||!near(rect("df").y,footer)||doc->body->attr("data-display")!="none")
        throw std::runtime_error("Hidden row did not reflow");
    press("showBtn");
    if(!near(rect("mv").h,62)||!near(rect("mv2a").y,rect("mv2").y)||doc->body->attr("data-display")!="table-row")
        throw std::runtime_error("Showing a row did not reflow");
    for(int i=0;i<20;++i){press("addBtn");press("delBtn");}
    if(doc->get_element_by_id("db")->layout.box_children.size()!=body_rows||!near(rect("df").y,footer)||!near(rect("mv").h,62))
        throw std::runtime_error("Repeated reflow accumulated table rows");
}
