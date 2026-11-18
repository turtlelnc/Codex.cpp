#include "src/engine.h"
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <cmath>
void check_reveal(const std::filesystem::path& directory,bool report){
 Loader::set_access_root(directory.u8string());Engine e(640,480);e.set_wait_ms(0);std::string error;
 auto check=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
 check(e.load((directory/"focus-reveal.html").u8string(),&error)&&e.render(false),"Reveal fixture");auto* d=e.document();
 auto node=[&](const char* id){return d->get_element_by_id(id);};auto click=[&](const char* id){auto r=node(id)->viewport_rect();return e.click(r.x+r.w/2,r.y+r.h/2);};
 auto state=[&](float px,float py,float wy,float y){check(std::abs(node("panel")->scroll_x-px)<1&&std::abs(node("panel")->scroll_y-py)<1&&std::abs(d->scroll_y-wy)<1&&std::abs(node("far")->viewport_rect().y-y)<1,"Reveal geometry disagrees with Chromium baseline");};
 check(e.key("Tab")&&d->focused_id==node("far")->id,"Tab focuses offscreen form input");state(20,260,410,230);
 check(e.render(false),"Paint revealed form");if(report)e.canvas()->save_png("/tmp/miniweb-focus-native-form.png");
 {auto i=(235*640+210)*4;const auto& pixels=e.canvas()->pixels();check(pixels[i]==240&&pixels[i+1]==221&&pixels[i+2]==189,"Revealed input is painted at its viewport coordinates");}
 check(e.type("task")&&e.key("Enter")&&d->body->attr("data-measure").find("\"submitted\":1")!=std::string::npos,"Revealed input edits and submits");
 check(click("formreset")&&d->body->attr("data-measure").find("\"value\":\"seed\"")!=std::string::npos,"Form reset remains functional");
 check(click("reset")&&click("prevent")&&d->focused_id==node("far")->id,"preventScroll focuses without revealing");state(0,0,0,900);
 check(click("reset")&&click("start"),"Default reveal");state(20,300,600,0);
 check(click("reset")&&click("end"),"Boolean end reveal");state(20,220,220,460);
 check(click("reset")&&click("nearest"),"Nearest reveal");state(20,220,220,460);
 check(click("reset")&&click("center"),"Centered reveal");state(70,260,410,230);
 check(click("reset")&&click("container"),"Nearest-container reveal");state(20,300,0,600);
 check(click("reset")&&click("hidden")&&d->scroll_y==0&&node("panel")->scroll_y==0,"Hidden reveal is a no-op");
 check(click("reset")&&click("detached")&&d->scroll_y==0&&node("panel")->scroll_y==0,"Detached reveal is a no-op");
 check(click("invalid")&&d->body->attr("data-invalid").empty()&&d->scroll_y==0&&node("panel")->scroll_y==0,"Invalid reveal options do not scroll or abort the handler");
 check(click("reset")&&click("center")&&click("settings"),"Reveal settings control");
 check(d->scroll_y==410&&node("dialog-scroll")->scroll_y==240&&d->focused_id==node("dialog-input")->id,"Fixed settings reveal preserves background scroll");
 check(e.render(false),"Paint revealed settings");if(report)e.canvas()->save_png("/tmp/miniweb-focus-native-settings.png");
 {auto i=(185*640+400)*4;const auto& pixels=e.canvas()->pixels();check(pixels[i]==240&&pixels[i+1]==221&&pixels[i+2]==189,"Modal input is visible after inner scrolling");}
 check(e.type("!")&&e.key("Escape")&&node("dialog")->has_attr("hidden")&&d->scroll_y==410&&d->focused_id==node("near")->id,"Close settings with keyboard and restore focus without moving background");
 if(report)std::cout<<"REVEAL_RESULT "<<d->body->attr("data-measure")<<"\n";
 std::cerr<<"MiniWeb focus/reveal acceptance passed\n";
}
