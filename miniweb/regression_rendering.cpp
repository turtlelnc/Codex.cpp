#include "src/engine.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

void check_text_visibility(const std::filesystem::path& directory) {
    Engine engine(640,480);
    engine.set_wait_ms(0);
    std::string error;
    if (!engine.load((directory/"text-visibility.html").u8string(), &error))
        throw std::runtime_error(error);
    auto* doc=engine.document();
    const auto before=serialize_html(doc->root);
    const auto nodes=doc->arena.size();
    if (!engine.render(false)) throw std::runtime_error("Text/visibility fixture failed");
    for(const auto& value:std::vector<std::pair<const char*,float>>{
        {"line-px",24},{"line-percent",30},{"line-number",24},{"line-em",24},{"line-rem",24}})
        if(std::abs(style_line_height(doc->get_element_by_id(value.first)->style)-value.second)>.01f)
            throw std::runtime_error("Font shorthand line-height unit mismatch");
    if(doc->get_element_by_id("size-rem")->style.font_size!=16)
        throw std::runtime_error("Font shorthand rem size used parent font size");
    auto ink=[&](int x,int y,int width,int height) {
        int count=0;
        const auto& pixels=engine.canvas()->pixels();
        for(int row=y;row<y+height;++row) for(int col=x;col<x+width;++col) {
            const auto offset=(row*640+col)*4;
            if(pixels[offset]<100&&pixels[offset+1]<100&&pixels[offset+2]<100)++count;
        }
        return count;
    };
    if(ink(10,10,180,60)<30)throw std::runtime_error("Flex button direct text disappeared");
    if(ink(320,0,160,140)<30)throw std::runtime_error("Column flex text disappeared");
    if(ink(500,0,80,100)<30)throw std::runtime_error("Wrapped flex text disappeared");
    const auto& pixels=engine.canvas()->pixels();
    auto pixel=[&](int x,int y,int r,int g,int b){const auto i=(y*640+x)*4;
        if(pixels[i]!=r||pixels[i+1]!=g||pixels[i+2]!=b)
            throw std::runtime_error("Visibility pixel mismatch at "+std::to_string(x)+","+std::to_string(y));};
    pixel(50,280,0,128,0);pixel(15,350,255,255,255);pixel(130,280,255,255,255);
    pixel(420,280,255,255,255);pixel(260,260,255,255,255);
    if(serialize_html(doc->root)!=before||doc->arena.size()!=nodes)
        throw std::runtime_error("Layout inserted anonymous items into the DOM");
    int darkest=255;
    for(int row=400;row<480;++row)for(int col=0;col<200;++col)
        darkest=std::min(darkest,(int)pixels[(row*640+col)*4]);
    if(darkest<120||darkest>160)throw std::runtime_error("Anonymous text inherited opacity twice");
    auto* mixed=doc->get_element_by_id("mixed");
    auto* whitespace=doc->get_element_by_id("whitespace");
    if(mixed->layout.box_children.size()!=3||whitespace->layout.box_children.size()!=2)
        throw std::runtime_error("Anonymous flex text grouping/whitespace mismatch");
    auto* plain=doc->get_element_by_id("plainword")->layout.box_children.at(0);
    auto* split=doc->get_element_by_id("splitword")->layout.box_children.at(0);
    if(plain->layout.lines.size()!=1||split->layout.lines.size()!=1||
        plain->layout.content_h!=split->layout.content_h)
        throw std::runtime_error("DOM text-node boundaries introduced word breaks");
    if(!engine.click(100,40)||doc->body->attr("data-label-click")!="yes")
        throw std::runtime_error("Flex text click failed");
    if(!engine.click(50,280)||doc->body->attr("data-visible-click")!="yes")
        throw std::runtime_error("Visible descendant click failed");
    engine.click(15,350);
    if(doc->body->has_attr("data-hidden-click"))throw std::runtime_error("Hidden parent accepted a click");
    const auto anonymous_nodes=doc->layout_nodes.size();
    for(int i=0;i<4;++i){
        if(!engine.click(100,40)||!engine.render(false))throw std::runtime_error("Repeated layout failed");
        if(doc->layout_nodes.size()!=anonymous_nodes||doc->arena.size()!=nodes)
            throw std::runtime_error("Anonymous boxes accumulated between layouts");
    }
}

void check_font_border(const std::filesystem::path& directory) {
    FontManager fonts;
    if(!fonts.init())throw std::runtime_error("No test fonts available");
    ComputedStyle style;style.font_family="monospace";style.font_size=12;
    const auto small=fonts.text_width("Fractional metrics",style);
    style.font_size=12.5f;const auto fractional=fonts.text_width("Fractional metrics",style);
    if(small<=0||std::abs(fractional/small-12.5f/12.f)>.0001f)
        throw std::runtime_error("Fractional font size rounded to a whole pixel");
    style.font_size=0;
    if(fonts.text_width("Zero size",style)!=0)throw std::runtime_error("Zero-size text has width");
    Engine engine(640,240);engine.set_wait_ms(0);std::string error;
    if(!engine.load((directory/"borders.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    for(const auto& entry:std::vector<std::pair<const char*,float>>{{"zero",40},{"none",40},{"zero-solid",40},
        {"width-first",48},{"style-first",48},{"transparent",48},{"width-only",40},{"hidden-width",50},
        {"side-zero",45},{"medium",46},{"reset-style",40},{"current",46},{"invalid",50},{"negative",50},{"initial",46},{"unset",46},{"reset-width",46},{"reset-style-long",50},
        {"reset-side",48},{"reset-radius",50},{"reset-color",50}})
        if(engine.document()->get_element_by_id(entry.first)->layout.border.w!=entry.second)
            throw std::runtime_error(std::string("Border width mismatch: ")+entry.first);
    const auto& pixels=engine.canvas()->pixels();
    for(const auto& point:std::vector<std::pair<int,int>>{{10,10},{70,10},{130,10},{330,10},{400,10},{530,20},{70,80}}){
        const auto i=(point.second*640+point.first)*4;
        if(pixels[i]!=255||pixels[i+1]!=255||pixels[i+2]!=255)throw std::runtime_error("Zero/hidden/transparent border painted");}
    const auto i=(80*640+130)*4;
    if(pixels[i]!=255||pixels[i+1]!=0||pixels[i+2]!=0)throw std::runtime_error("Border currentColor did not resolve final color");
}
