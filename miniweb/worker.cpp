#include "worker.h"
#include "engine.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cmath>
#ifndef _WIN32
#include <sys/resource.h>
#endif
namespace {
int number(const char* value,int minimum,int maximum){size_t used=0;const std::string s(value);const auto n=std::stoi(s,&used);if(used!=s.size()||n<minimum||n>maximum)throw std::runtime_error("Preview value outside bounds");return n;}
}
int codex_miniweb_worker(int argc,char** argv){try{
  // Private child entry: workspace, input, output, width, height, wait, then clicks.
  if(argc<8||argc>40||(argc-8)%2)throw std::runtime_error("Invalid preview worker arguments");
#ifndef _WIN32
  struct rlimit cpu{20,20};if(setrlimit(RLIMIT_CPU,&cpu)!=0)throw std::runtime_error("Cannot limit preview CPU");
  struct rlimit data{768ull*1024*1024,768ull*1024*1024};if(setrlimit(RLIMIT_DATA,&data)!=0)throw std::runtime_error("Cannot limit preview memory");
  struct rlimit file{8*1024*1024,8*1024*1024};setrlimit(RLIMIT_FSIZE,&file);
#ifdef __linux__
  struct rlimit memory{1024ull*1024*1024,1024ull*1024*1024};setrlimit(RLIMIT_AS,&memory);
#endif
#endif
  const int width=number(argv[5],64,1920),height=number(argv[6],64,1920),wait=number(argv[7],0,3000);
  Loader::set_access_root(argv[2]);Engine engine(width,height);engine.set_wait_ms(wait);std::string error;
  if(!engine.load(argv[3],&error))throw std::runtime_error(error);
  if(!engine.render(false))throw std::runtime_error("Preview render failed");
  for(int i=8;i<argc;i+=2){engine.click(static_cast<float>(number(argv[i],0,width-1)),static_cast<float>(number(argv[i+1],0,height-1)));if(!engine.render(false))throw std::runtime_error("Preview click render failed");}
  if(!engine.canvas()||!engine.canvas()->save_png(argv[4]))throw std::runtime_error("Cannot save preview PNG");
  std::cout<<"MiniWeb experimental preview "<<width<<"x"<<height<<"; not Chromium verification\n";return 0;
}catch(const std::exception& e){std::cerr<<"Preview: "<<e.what()<<'\n';return 2;}}
