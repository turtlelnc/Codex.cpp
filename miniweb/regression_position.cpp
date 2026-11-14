#include "src/engine.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>

void check_positioning(const std::filesystem::path& directory){
    Engine engine(640,800);engine.set_wait_ms(0);std::string error;
    Loader::set_access_root(directory.u8string());
    if(!engine.load((directory/"positioning.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    auto node=[&](const char* id){return engine.document()->get_element_by_id(id);};
    auto eq=[](float a,float b,const char* label){if(std::abs(a-b)>.1f)throw std::runtime_error(label);};
    eq(node("pin")->absolute_rect().x,68,"Positioned ancestor percentage width");
    eq(node("pin")->absolute_rect().y,94,"Positioned ancestor percentage height");
    eq(node("corner")->absolute_rect().x,317,"Nested absolute padding box");
    eq(node("viewport")->absolute_rect().x,602,"Initial containing block");
    eq(node("fill")->absolute_rect().w,184,"Opposing horizontal insets");
    eq(node("fill")->absolute_rect().h,94,"Opposing vertical insets");
    eq(node("center")->absolute_rect().x,102,"Absolute automatic margins");
    if(node("pin")->parent!=node("inner")||node("pin")->layout.parent!=node("cb"))
        throw std::runtime_error("DOM and layout ancestry are not separated");
    const auto& pixels=engine.canvas()->pixels();
    const size_t red=(99*640+73)*4,alpha=(49*640+189)*4;
    if(pixels[red]!=255||pixels[red+1]!=0||pixels[red+2]!=0)throw std::runtime_error("Static overflow ancestor clipped escaped absolute");
    if(pixels[alpha]<126||pixels[alpha]>129||pixels[alpha+1]!=255||pixels[alpha+2]<126||pixels[alpha+2]>129)
        throw std::runtime_error("Reparenting lost DOM opacity");
    if(!engine.click(73,99)||engine.document()->body->attr("data-clicks")!="1"||engine.document()->body->attr("data-bubbled")!="yes")
        throw std::runtime_error("Escaped absolute click or DOM bubbling failed");
    eq(node("zero-pin")->absolute_rect().x,222,"Identity transform establishes containing block");
    for(float expected:{620.f,222.f}){
        if(!engine.click(10,10)||!engine.render(false))throw std::runtime_error("Transform toggle failed");
        eq(node("zero-pin")->absolute_rect().x,expected,"Transform mutation updates containing block");
    }
}
