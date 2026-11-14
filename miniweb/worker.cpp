#include "worker.h"
#include "engine.h"
#include "diagnostics.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>
#ifndef _WIN32
#include <sys/resource.h>
#endif
namespace {
#ifndef _WIN32
void limit_resource(int resource,rlim_t maximum){struct rlimit inherited{};if(getrlimit(resource,&inherited)!=0)throw std::runtime_error("Cannot inspect preview resource limit");
    const rlim_t bound=std::min(maximum,std::min(inherited.rlim_cur,inherited.rlim_max));struct rlimit limit{bound,bound};if(setrlimit(resource,&limit)!=0)throw std::runtime_error(std::string("Cannot set preview resource limit ")+std::to_string(resource)+": "+std::strerror(errno));}
#endif
#ifdef __APPLE__
// Darwin's data-limit accounting includes pre-reserved VM. Bound resident
// memory instead of rejecting healthy workers whose reserved map is large.
class ResidentGuard {
    std::atomic<bool> done{false};std::thread monitor;
public:
    ResidentGuard():monitor([this](){while(!done.load()){
        struct rusage usage{};if(getrusage(RUSAGE_SELF,&usage)!=0||usage.ru_maxrss>768ll*1024*1024){std::fputs("Preview resident memory limit exceeded\n",stderr);std::fflush(stderr);std::_Exit(125);}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }}){}
    ~ResidentGuard(){done=true;monitor.join();}
};
#endif
int number(const char* value,int minimum,int maximum){size_t used=0;const std::string s(value);const auto n=std::stoi(s,&used);if(used!=s.size()||n<minimum||n>maximum)throw std::runtime_error("Preview value outside bounds");return n;}
}
int codex_miniweb_worker(int argc,char** argv){try{
  // Private child entry: workspace, input, output, width, height, wait, then clicks.
  if(argc<8||argc>40||(argc-8)%2)throw std::runtime_error("Invalid preview worker arguments");
#ifndef _WIN32
  limit_resource(RLIMIT_CPU,20);
  limit_resource(RLIMIT_FSIZE,8*1024*1024);
#ifdef __linux__
  limit_resource(RLIMIT_DATA,768ull*1024*1024);
  limit_resource(RLIMIT_AS,1024ull*1024*1024);
#endif
#endif
  const int width=number(argv[5],64,1920),height=number(argv[6],64,1920),wait=number(argv[7],0,3000);
#ifdef __APPLE__
  ResidentGuard memory_guard;
#endif
  Loader::set_access_root(argv[2]);Engine engine(width,height);engine.set_wait_ms(wait);std::string error;
  if(!engine.load(argv[3],&error))throw std::runtime_error(error);
  if(!engine.render(false))throw std::runtime_error("Preview render failed");
  for(int i=8;i<argc;i+=2){engine.click(static_cast<float>(number(argv[i],0,width-1)),static_cast<float>(number(argv[i+1],0,height-1)));if(!engine.render(false))throw std::runtime_error("Preview click render failed");}
  if(!engine.canvas()||!engine.canvas()->save_png(argv[4]))throw std::runtime_error("Cannot save preview PNG");
  // JavaScript console output is untrusted. The host reads this private artifact,
  // not the human-readable log lines below, as the capability result.
  std::ofstream metadata(std::filesystem::u8path(std::string(argv[4])+miniweb_report_suffix),std::ios::binary|std::ios::trunc);
  metadata<<"{\"version\":1,\"summary\":\""<<diag::detail::json_escape(diag::report().summary())
          <<"\",\"report\":"<<diag::report().json()<<'}';
  metadata.close();
  if(!metadata)throw std::runtime_error("Cannot save preview capability report");
  std::cout<<"[miniweb] capability: "<<diag::report().summary()<<'\n';
  std::cout<<"[miniweb] capability-json: "<<diag::report().json()<<'\n';
  std::cout<<"MiniWeb experimental preview "<<width<<"x"<<height<<"; not Chromium verification\n";return 0;
}catch(const std::exception& e){std::cerr<<"Preview: "<<e.what()<<'\n';return 2;}}
