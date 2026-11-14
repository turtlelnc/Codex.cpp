#include "src/engine.h"
#include <cmath>
#include <filesystem>
#include <stdexcept>

void check_margins(const std::filesystem::path& directory){
    Engine engine(640,800);engine.set_wait_ms(0);std::string error;
    Loader::set_access_root(directory.u8string());
    if(!engine.load((directory/"margins.html").u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
    auto rect=[&](const char* id){return engine.document()->get_element_by_id(id)->absolute_rect();};
    auto eq=[](float a,float b,const char* label){if(std::abs(a-b)>.1f)throw std::runtime_error(label);};
    eq(rect("basic").h,20,"Parent excludes escaped child margins");
    eq(rect("basic").y,rect("basic-first").y,"First child top margin escapes");
    eq(rect("border").h,92,"Border blocks child margin collapse");
    eq(rect("padding").h,92,"Padding blocks child margin collapse");
    eq(rect("hidden").h,90,"Overflow establishes margin boundary");
    eq(rect("fixed").h,80,"Definite height blocks trailing collapse");
    eq(rect("grand").y,rect("mixed-first").y,"Nested mixed margin struts");
    eq(rect("empty").y-rect("before").bottom(),20,"Empty border edge excludes own bottom margin");
    eq(rect("after").y-rect("before").bottom(),30,"Empty margins collapse through");
    eq(rect("leading-parent").y,rect("leading-first").y,"Leading empty child margins escape");
    eq(rect("trailing").y-rect("trailing-parent").bottom(),50,"Trailing empty border edge");
    eq(rect("nonbinding").h,20,"Nonbinding minimum keeps trailing collapse");
    eq(rect("binding").h,21,"Binding minimum acts as definite height");
}
