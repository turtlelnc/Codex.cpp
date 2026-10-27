// Browser and headless server entry points. Agent execution remains in this binary.
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
static int launch_web(int argc,char** argv){
#if defined(_WIN32)
  std::cerr<<"Web hosting currently requires Linux/macOS and Python 3; connect from any browser.\n";return 2;
#else
  std::error_code ec;fs::path binary;
#if defined(__linux__)
  binary=fs::read_symlink("/proc/self/exe",ec);
#elif defined(__APPLE__)
  uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> buffer(size);
  if(_NSGetExecutablePath(buffer.data(),&size)){std::cerr<<"Cannot locate codex-cpp executable\n";return 2;}
  binary=fs::canonical(buffer.data(),ec);
#else
  binary=fs::canonical(argv[0],ec);
#endif
  if(ec){std::cerr<<"Cannot locate codex-cpp executable\n";return 2;}
  fs::path entry=binary.parent_path()/"server/web_server.py";
  if(!fs::is_regular_file(entry))entry="/usr/local/libexec/codex-server/web_server.py";
  if(!fs::is_regular_file(entry)){std::cerr<<"Web runtime missing: install server/ and web/ beside codex-cpp, or run server/install.sh\n";return 2;}
  std::vector<std::string> args={"python3","-I",entry.string(),"--binary",binary.string(),"--mode",std::string(argv[1])=="serve"?"server":"local"};
  for(int i=2;i<argc;++i)args.push_back(argv[i]);
  std::vector<char*> pointers;for(auto& arg:args)pointers.push_back(arg.data());pointers.push_back(nullptr);
  ::execvp("python3",pointers.data());std::cerr<<"Unable to launch Python 3 web runtime\n";return 127;
#endif
}
