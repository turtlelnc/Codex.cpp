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
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
