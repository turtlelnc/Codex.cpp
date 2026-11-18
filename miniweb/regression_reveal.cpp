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
 Engine absolute(640,480);absolute.set_wait_ms(0);check(absolute.load((directory/"reveal-absolute.html").u8string(),&error)&&absolute.render(false),"Absolute reveal fixture");
 for(const char* id:{"start","nearest","focus"}){auto r=absolute.document()->get_element_by_id(id)->viewport_rect();check(absolute.click(r.x+10,r.y+10),"Absolute reveal action");check(absolute.document()->body->attr("data-measure")==std::string(id==std::string("focus")?"[0,570,230]":"[0,800,0]"),"Reveal only scrolls layout ancestors that affect absolute target geometry");}
 Engine text(640,480);text.set_wait_ms(0);check(text.load((directory/"reveal-inline.html").u8string(),&error)&&text.render(false),"Inline reveal fixture");
 auto* link=text.document()->get_element_by_id("link");auto* inl=text.document()->get_element_by_id("inline");
 check(text.key("Tab")&&text.document()->focused_id==link->id&&text.document()->scroll_y>900,"Tab reveals inline link without standalone box");
 auto link_rect=link->client_rect();check(link_rect.w>0&&std::abs(link_rect.y+link_rect.h/2-240)<5&&text.click(link_rect.x+link_rect.w/2,link_rect.y+link_rect.h/2)&&text.document()->body->attr("data-click")=="link","Inline link fragments yield visible, clickable client geometry");
 check(text.key("Tab")&&text.document()->focused_id==inl->id&&text.document()->scroll_y>1300,"Tab reveals focusable inline span");
 for(const char* id:{"start","nearest"}){auto r=text.document()->get_element_by_id(id)->viewport_rect();check(text.click(r.x+10,r.y+10)&&std::abs(inl->client_rect().y)<1,"Explicit inline reveal uses fragment owner scroll chain");}
 const float previous=text.document()->scroll_y;auto hide=text.document()->get_element_by_id("hide")->viewport_rect();check(text.click(hide.x+10,hide.y+10)&&inl->client_rect().w==0&&text.document()->scroll_y==previous,"Hidden inline fragments cannot reveal stale geometry");
 if(report)std::cout<<"REVEAL_RESULT "<<d->body->attr("data-measure")<<"\n";
 std::cerr<<"MiniWeb focus/reveal acceptance passed\n";
}
