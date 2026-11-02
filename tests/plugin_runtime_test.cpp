#include "../plugin/runtime.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

int main(){
  plugin::Runtime runtime;
  runtime.mount({"builtin",{},{{"echo",{},{}}}});
  auto base=[](const std::string& request,const plugin::Emit&){return request;};
  assert(runtime.call("echo","a",{},base)=="a");
  runtime.mount({"decoration",{"builtin"},{{"echo","builtin",[](const std::string& r,const plugin::Emit& emit,const plugin::Next& next){return next(r+"b",emit)+"c";}}}});
  assert(runtime.call("echo","a",{},base)=="abc");
  bool rejected=false;try{runtime.mount({"collision",{},{{"new-service",{},{}},{"echo",{},{}}}});}catch(...){rejected=true;}
  assert(rejected && !runtime.contains("new-service")); // Transactional mount.
  rejected=false;try{runtime.unmount("builtin");}catch(...){rejected=true;}assert(rejected);
  runtime.unmount("decoration");assert(runtime.call("echo","a",{},base)=="a");
  rejected=false;try{runtime.mount({"missing",{"absent"},{}});}catch(...){rejected=true;}assert(rejected);
  // Concurrent calls must not share request callbacks or hold registry locks.
  std::atomic<int> calls{0};std::vector<std::thread> threads;
  for(int i=0;i<8;++i)threads.emplace_back([&]{for(int j=0;j<1000;++j){assert(runtime.call("echo","ok",{},[&](const std::string& input,const plugin::Emit&){++calls;return input;})=="ok");}});
  for(auto& thread:threads)thread.join();assert(calls==8000);
  runtime.mount({"recursive",{},{{"loop",{},[&](const std::string& input,const plugin::Emit&,const plugin::Next&){return runtime.call("loop",input);}}}});
  rejected=false;try{runtime.call("loop","");}catch(...){rejected=true;}assert(rejected);
  runtime.freeze();rejected=false;try{runtime.unmount("recursive");}catch(...){rejected=true;}assert(rejected);
  std::cout<<"PASS Plugin layering, conflict rollback, dependencies, disposal order, concurrent calls, recursion and freeze\n";
}
