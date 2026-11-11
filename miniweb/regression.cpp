#include "src/engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
int main(int argc,char** argv){try{
 if(argc!=2)return 2;auto page=std::filesystem::canonical(std::filesystem::u8path(argv[1]));Loader::set_access_root(page.parent_path().u8string());Engine engine(640,480);engine.set_wait_ms(0);std::string error;if(!engine.load(page.u8string(),&error)||!engine.render(false))throw std::runtime_error(error);
 auto pixel=[&](int x,int y,int r,int g,int b){const auto& data=engine.canvas()->pixels();size_t i=(y*640+x)*4;if(data[i]!=r||data[i+1]!=g||data[i+2]!=b)throw std::runtime_error("Unexpected rendered pixel at "+std::to_string(x)+","+std::to_string(y));};
 pixel(300,230,255,0,0);pixel(260,230,255,255,255);pixel(40,40,255,0,0);pixel(390,340,255,0,0);pixel(420,340,0,0,255);pixel(20,390,0,128,0);
 if(!engine.click(40,40)||!engine.render(false))throw std::runtime_error("z-index hit test failed");pixel(40,40,0,128,0);
 if(!engine.click(300,230)||!engine.render(false))throw std::runtime_error("translated hit test failed");pixel(300,230,0,128,0);
 std::cout<<"{";bool first=true;for(const char* id:{"center","front","back","grid","wide","next","wrap","min","max","clamp"}){auto* node=engine.document()->get_element_by_id(id);if(!node)throw std::runtime_error("Missing fixture node");auto box=node->absolute_rect();if(!first)std::cout<<',';first=false;std::cout<<'"'<<id<<"\":["<<box.x<<','<<box.y<<','<<box.w<<','<<box.h<<']';}std::cout<<"}\n";
 Engine styles(640,480);styles.set_wait_ms(0);if(!styles.load((page.parent_path()/"styles.html").u8string(),&error)||!styles.render(false))throw std::runtime_error(error);
 auto style_pixel=[&](int x,int r,int g,int b,int y=10){const auto& data=styles.canvas()->pixels();const size_t i=(y*640+x)*4;if(data[i]!=r||data[i+1]!=g||data[i+2]!=b)throw std::runtime_error("Stylesheet cascade/mutation/URL pixel mismatch at "+std::to_string(x));};
 style_pixel(10,0,0,255);style_pixel(70,0,0,255);style_pixel(130,255,0,0);style_pixel(190,255,0,0);style_pixel(250,0,128,0);style_pixel(310,0,0,255);style_pixel(370,0,0,255);
 style_pixel(10,255,255,255,170);styles.click(10,170);styles.render(false);style_pixel(70,255,0,0,130);
 if(!styles.click(10,130)||!styles.render(false))throw std::runtime_error("Visible clipped child click failed");style_pixel(70,0,128,0,130);
 if(!styles.click(70,10)||!styles.render(false))throw std::runtime_error("Stylesheet mutation click failed");style_pixel(70,0,128,0);
 if(!styles.click(310,10)||!styles.render(false))throw std::runtime_error("Inserted stylesheet mutation failed");style_pixel(310,0,128,0);
 Engine runtime(640,480);runtime.set_wait_ms(500);if(!runtime.load((page.parent_path()/"runtime.html").u8string(),&error)||!runtime.render(false))throw std::runtime_error(error);
 auto* body=runtime.document()->body;for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"data-width","200"},{"data-computed","200px"},{"data-added","123"},{"data-detached","0"},{"data-siblings","2:a:c"},{"data-before-hidden","123"},{"data-hidden","0"},{"data-once","1"},{"data-repeat","5"},{"data-microtask-clock","0"}})if(body->attr(pair.first)!=pair.second)throw std::runtime_error(pair.first+" expected "+pair.second+" got "+body->attr(pair.first));
 if(body->has_attr("data-late"))throw std::runtime_error("Virtual wait window was extended");
 if(!runtime.click(10,10)||!runtime.render(false)||body->attr("data-click-clock")!="500"||body->attr("data-after-click")!="yes")throw std::runtime_error("Wait deadline failed to advance across clicks");
 auto* siblings=runtime.document()->get_element_by_id("siblings");auto* a=runtime.document()->get_element_by_id("a");if(a->replace_child(siblings,nullptr)||siblings->replace_child(body,a)||body->parent==siblings)throw std::runtime_error("Ancestor replacement accepted");
 Engine clock(640,480);clock.set_wait_ms(500);if(!clock.load((page.parent_path()/"clock.html").u8string(),&error)||!clock.render(false))throw std::runtime_error(error);
 if(clock.document()->body->has_attr("data-timer")||!clock.click(2,2)||clock.document()->body->attr("data-click-clock")!="500"||clock.document()->body->attr("data-timer")!="fired")throw std::runtime_error("Idle clock did not advance through replay wait");
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
