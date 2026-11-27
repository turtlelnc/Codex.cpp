#include "worker.h"
#include "engine.h"
#include "diagnostics.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
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
  for(int i=8;i<argc;i+=2){
    const int click = (i-8)/2+1;
    if(!engine.click(static_cast<float>(number(argv[i],0,width-1)),static_cast<float>(number(argv[i+1],0,height-1))))
      throw std::runtime_error("Preview click "+std::to_string(click)+" failed");
    if(!engine.render(false))throw std::runtime_error("Preview click render failed");
  }
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

// Sessions use a bounded private stdin protocol. Page console output is never
// an acknowledgement; only native-written, sequenced private artifacts are.
int codex_miniweb_session_worker(int argc,char** argv){try{
  if(argc!=8)throw std::runtime_error("Invalid session worker arguments");
#ifndef _WIN32
  limit_resource(RLIMIT_CPU,20);limit_resource(RLIMIT_FSIZE,8*1024*1024);
#ifdef __linux__
  limit_resource(RLIMIT_DATA,768ull*1024*1024);limit_resource(RLIMIT_AS,1024ull*1024*1024);
#endif
#endif
#ifdef __APPLE__
  ResidentGuard memory_guard;
#endif
  struct Lifetime {
    std::atomic<bool> done{false};std::atomic<long long> touched{0};std::thread monitor;
    static long long now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    Lifetime(){touched=now();monitor=std::thread([this]{const auto start=now();while(!done){if(now()-start>300000||now()-touched>60000)std::_Exit(124);std::this_thread::sleep_for(std::chrono::milliseconds(50));}});}
    ~Lifetime(){done=true;monitor.join();}
  } lifetime;
  const int width=number(argv[5],64,1920),height=number(argv[6],64,1920);
  Loader::set_access_root(argv[2]);Engine engine(width,height);engine.set_wait_ms(number(argv[7],0,3000));std::string error;
  if(!engine.load(argv[3],&error)||!engine.render(false))throw std::runtime_error("Session load failed: "+error);
  const auto output=std::filesystem::u8path(argv[4]);
  auto acknowledge=[&](int sequence,bool ok){
    // Diagnostics precede the native acknowledgement, including buffered logs.
    std::cout.flush();std::cerr.flush();std::fflush(stdout);std::fflush(stderr);
    std::ofstream reply(output/"reply.tmp",std::ios::binary|std::ios::trunc);reply<<sequence<<' '<<(ok?1:0);reply.close();if(!reply)throw std::runtime_error("Cannot save session reply");
    std::filesystem::rename(output/"reply.tmp",output/"reply");
  };
  acknowledge(0,true);
  size_t input_bytes=0;
  for(int sequence=1;sequence<=128;++sequence){
    // Read without allowing an unbounded getline allocation.
    std::string line;char c;while(std::cin.get(c)&&c!='\n'){if(line.size()>=20000)throw std::runtime_error("Session frame limit");line+=c;}
    if(!std::cin)return 0;
    input_bytes+=line.size();if(input_bytes>128*1024)throw std::runtime_error("Session input budget exceeded");
    lifetime.touched=Lifetime::now();
    std::istringstream frame(line);int received;std::string op,data,extra;if(!(frame>>received>>op)||received!=sequence)throw std::runtime_error("Invalid session sequence");
    bool ok=false;
    if(op=="click"){std::string x,y;if(!(frame>>x>>y)||frame>>extra)throw std::runtime_error("Invalid click frame");ok=engine.click(number(x.c_str(),0,width-1),number(y.c_str(),0,height-1));}
    else if(op=="type"||op=="key"||op=="inspect"){
      if((!(frame>>data)&&op!="inspect")||frame>>extra||data.size()>16384||data.size()%2)throw std::runtime_error("Invalid text frame");
      std::string text;for(size_t i=0;i<data.size();i+=2){auto hex=[](char h)->int{if(h>='0'&&h<='9')return h-'0';if(h>='a'&&h<='f')return h-'a'+10;throw std::runtime_error("Invalid hex");};text+=static_cast<char>(hex(data[i])*16+hex(data[i+1]));}
      if (op == "inspect") {
        if (text.size() > 128) throw std::runtime_error("Element ID exceeds preview limit");
        std::ofstream observation(output/"inspection.json", std::ios::binary | std::ios::trunc);
        observation << engine.inspect(text);
        observation.close();
        ok = bool(observation);
      } else ok=op=="type"?engine.type(text):engine.key(text);
    }else if(op=="scroll"){
      std::string x,y,dx,dy;if(!(frame>>x>>y>>dx>>dy)||frame>>extra)throw std::runtime_error("Invalid scroll frame");
      ok=engine.scroll(number(x.c_str(),0,width-1),number(y.c_str(),0,height-1),number(dx.c_str(),-10000,10000),number(dy.c_str(),-10000,10000));
    }else if(op=="screenshot"){
      if(frame>>extra)throw std::runtime_error("Invalid screenshot frame");
      ok=engine.advance(0)&&engine.render(false)&&engine.canvas()->save_png((output/"screenshot.png").u8string());
      if(ok){std::ofstream meta(output/"screenshot.png.report.json",std::ios::binary|std::ios::trunc);meta<<"{\"version\":1,\"summary\":\""<<diag::detail::json_escape(diag::report().summary())<<"\",\"report\":"<<diag::report().json()<<'}';meta.close();ok=bool(meta);}
    }else if(op=="wait") {if(!(frame>>data)||frame>>extra)throw std::runtime_error("Invalid wait frame");ok=engine.advance(number(data.c_str(),0,3000));}
    else throw std::runtime_error("Unknown session operation");
    acknowledge(sequence,ok);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Preview session: "<<e.what()<<'\n';return 2;}}
