#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
void check_scroll(const std::filesystem::path& directory,bool report){
 Loader::set_access_root(directory.u8string());Engine e(640,480);e.set_wait_ms(0);std::string error;
 auto check=[](bool yes,const char* message){if(!yes)throw std::runtime_error(message);};
 check(e.load((directory/"scroll.html").u8string(),&error)&&e.render(false),"Scroll fixture load");auto* d=e.document();auto node=[&](const char* id){return d->get_element_by_id(id);};
 check(node("panel")->scroll_height==700&&node("panel")->scroll_width==400,"Scrollable overflow extents");
 check(e.scroll(50,150,100,260)&&e.render(false),"Scroll nested panel");
 check(node("panel")->scroll_x==100&&node("panel")->scroll_y==260&&d->body->attr("data-scroll")=="100:260","Native offset and scroll event");
 auto r=node("target")->viewport_rect(),s=node("sticky")->viewport_rect();
 check(std::abs(r.x-40)<.1&&std::abs(r.y-80)<.1&&std::abs(s.y-60)<.1,"Scrolled and sticky viewport geometry");
 auto pixel=[&](int x,int y,int red,int green,int blue){auto i=(y*640+x)*4;const auto& p=e.canvas()->pixels();check(p[i]==red&&p[i+1]==green&&p[i+2]==blue,"Scroll pixels agree with geometry");};
 pixel(45,105,0,0,255);pixel(25,65,0,128,0);pixel(305,15,255,0,0);
 check(e.click(45,105)&&d->body->attr("data-click")=="target","Scrolled target receives click");
 check(e.scroll(600,400,0,400)&&e.render(false)&&d->scroll_y==400&&d->body->attr("data-window-scroll")=="400","Page scroll events and state");
 check(std::abs(node("target")->viewport_rect().y-(r.y-400))<.1,"Document contents move with viewport scroll");
 check(node("fixed")->viewport_rect().y==10&&e.click(305,15)&&d->body->attr("data-click")=="fixed","Fixed geometry and hit testing survive viewport scroll");
 check(e.scroll(305,15,0,100)&&d->scroll_y==400&&d->body->attr("data-wheel")=="canceled","Cancelable wheel default");
 check(e.scroll(600,400,0,-10000)&&d->scroll_y==0,"Page scroll clamps at origin");
 check(set_scroll(node("hidden"),50,50)&&node("hidden")->scroll_y==50,"Hidden overflow permits programmatic scroll");
 check(!set_scroll(node("clip"),50,50)&&node("clip")->scroll_y==0,"Clip forbids scrolling");
 check(e.scroll(50,150,10000,10000)&&node("panel")->scroll_x==180&&node("panel")->scroll_y==520,"Nested offsets clamp at content bounds");
 check(e.scroll(50,150,0,100)&&d->scroll_y==100,"Scroll at container boundary chains to viewport");
 node("target")->detach();node("content")->set_attr("style","height:100px");check(e.render(false)&&node("panel")->scroll_y==0,"Content shrink clamps stale offset");
 d->html->set_attr("style","height:480px;overflow:hidden");check(e.render(false)&&e.scroll(600,400,0,100)&&d->scroll_y==100,"Hidden viewport rejects wheel scrolling");
 check(set_scroll(d->html,0,400)&&e.render(false)&&d->scroll_y==400,"Hidden viewport still permits programmatic scrolling");pixel(305,15,255,0,0);
 check(!e.scroll(-1,0,0,10)&&!e.scroll(0,0,0,10001)&&!e.scroll(0,0,NAN,10),"Invalid scroll operations rejected");
 if(report)std::cout<<"SCROLL_RESULT {\"target\":["<<r.x<<","<<r.y<<"],\"sticky\":["<<s.x<<","<<s.y<<"],\"extent\":[400,700],\"window\":400}\n";
 std::cerr<<"MiniWeb scroll regressions passed\n";
}
