// The only OS process launcher. Child policy is chosen by trusted host code, never RPC.
enum class ProcessRole{Probe,Helper,Tool,Extension,Frontend,Network,Clipboard};
struct ProcessPolicy{
  ProcessRole role=ProcessRole::Helper;fs::path workspace;bool writable=false,unsafe=false;
  std::vector<fs::path> reads,writes;
  static ProcessPolicy tool(const fs::path& root,bool write,bool unrestricted=false){ProcessPolicy p;p.role=ProcessRole::Tool;p.workspace=root;p.writable=write;p.unsafe=unrestricted;return p;}
  static ProcessPolicy network(){ProcessPolicy p;p.role=ProcessRole::Network;return p;}
};
struct ExecutionContext{
  fs::path workspace,private_dir;bool unsafe=false;
  void initialize(const fs::path& root){workspace=fs::absolute(root);if(!private_dir.empty())return;std::error_code ec;
    private_dir=fs::temp_directory_path(ec)/random_id("codex-private");if(ec||!fs::create_directory(private_dir,ec))throw std::runtime_error("cannot create private execution directory");
    fs::permissions(private_dir,fs::perms::owner_all,fs::perm_options::replace,ec);if(ec)throw std::runtime_error("cannot protect execution directory");private_dir=fs::canonical(private_dir);}
  ~ExecutionContext(){std::error_code ec;if(!private_dir.empty())fs::remove_all(private_dir,ec);}
};
static ExecutionContext execution;
static bool sandbox_available();
static std::string sandbox_failure;
static fs::path process_executable(const std::string& name){std::error_code ec;if(name.find_first_of("/\\")!=std::string::npos){auto p=fs::canonical(name,ec);return ec?fs::path():p;}
  const auto path=getenv_or("PATH");
#if defined(_WIN32)
  const char separator=';';
#else
  const char separator=':';
#endif
  std::istringstream input(path);std::string item;while(std::getline(input,item,separator)){if(item.empty())continue;auto p=fs::path(item)/name;
#if defined(_WIN32)
    if(!fs::exists(p,ec)&&p.extension().empty())p+=".exe";
#endif
    if(fs::is_regular_file(p,ec)){auto absolute=fs::canonical(p,ec);if(!ec)return absolute;}}
  return{};
}
static std::vector<fs::path> system_runtime_paths(){return{
#if defined(__APPLE__)
  "/System","/usr/bin","/usr/lib","/usr/libexec","/usr/share","/bin","/sbin","/dev/null","/dev/random","/dev/urandom","/Library/Developer/CommandLineTools","/Applications/Xcode.app/Contents","/opt/homebrew/bin","/opt/homebrew/lib","/opt/homebrew/opt","/opt/homebrew/Cellar","/opt/homebrew/share","/private/etc/ssl","/Library/Keychains/System.keychain","/Library/Preferences/com.apple.dt.Xcode.plist","/private/etc/localtime","/private/etc/hosts","/private/etc/resolv.conf","/private/var/db/dyld"
#else
  "/usr/bin","/usr/lib","/usr/lib64","/usr/share","/usr/local/bin","/usr/local/lib","/bin","/sbin","/lib","/lib64","/etc/ld.so.cache","/etc/localtime","/etc/hosts","/etc/resolv.conf","/etc/nsswitch.conf","/etc/passwd","/etc/group","/etc/ssl","/etc/pki"
#endif
};}
class ManagedProcess{
public:
#if defined(_WIN32)
  HANDLE process=nullptr,input=nullptr,output=nullptr,job=nullptr;
#else
  pid_t pid=-1;int input=-1,output=-1;
#endif
  fs::path scratch;int exit_code=-1;bool eof=false;
  ManagedProcess()=default;ManagedProcess(const ManagedProcess&)=delete;ManagedProcess& operator=(const ManagedProcess&)=delete;~ManagedProcess(){stop();}
  static std::string backend(){
#if defined(__APPLE__)
    return "macOS Seatbelt";
#elif defined(__linux__)
    return "Linux bubblewrap";
#else
    return "unavailable: Windows AppContainer backend not implemented";
#endif
  }
  bool start(std::vector<std::string> command,const ProcessPolicy& policy,std::string& error,bool merge_stderr=false){
    if(command.empty()){error="empty process command";return false;}if(execution.workspace.empty())execution.initialize(fs::current_path());
    const bool unsafe=execution.unsafe||policy.unsafe;if(policy.role!=ProcessRole::Probe&&!unsafe&&!sandbox_available()){error="DENIED: "+backend()+" sandbox unavailable: "+sandbox_failure+"; no automatic unsafe fallback";return false;}
    auto executable=process_executable(command.front());if(executable.empty()){error="executable not found: "+command.front();return false;}command.front()=executable.string();
    std::error_code ec;scratch=execution.private_dir/random_id("worker");fs::create_directories(scratch/"home",ec);if(ec){error=ec.message();return false;}fs::permissions(scratch,fs::perms::owner_all,fs::perm_options::replace,ec);
    const auto workspace=policy.workspace.empty()?execution.workspace:fs::absolute(policy.workspace);
    if(!unsafe&&(policy.role==ProcessRole::Tool||policy.role==ProcessRole::Extension||policy.role==ProcessRole::Helper))if(auto risk=external_hardlink_risk(workspace)){error="DENIED: "+*risk;return false;}
    const auto cwd=(policy.role==ProcessRole::Frontend||policy.role==ProcessRole::Network||policy.role==ProcessRole::Probe)?scratch:workspace;
    std::vector<fs::path> reads=system_runtime_paths(),writes={scratch};reads.push_back(executable);reads.insert(reads.end(),policy.reads.begin(),policy.reads.end());
    writes.insert(writes.end(),policy.writes.begin(),policy.writes.end());
    if(policy.role==ProcessRole::Tool||policy.role==ProcessRole::Extension||policy.role==ProcessRole::Helper){reads.push_back(workspace);if(policy.writable)writes.push_back(workspace);}
    if(policy.role==ProcessRole::Network){reads.push_back(execution.private_dir);writes.push_back(execution.private_dir);}
    std::vector<std::string> env={"HOME="+(scratch/"home").string(),"TMPDIR="+scratch.string(),"TMP="+scratch.string(),"TEMP="+scratch.string(),"LANG=C.UTF-8","LC_ALL=C.UTF-8",
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin","GIT_TERMINAL_PROMPT=0","GIT_OPTIONAL_LOCKS=0","GIT_CONFIG_NOSYSTEM=1","GIT_CONFIG_GLOBAL=/dev/null","PAGER=cat"};
#if defined(_WIN32)
    wchar_t windows[MAX_PATH];const auto length=GetWindowsDirectoryW(windows,MAX_PATH);if(!length||length>=MAX_PATH){error="cannot locate Windows runtime";return false;}
    const auto system=fs::path(windows);env.erase(std::remove_if(env.begin(),env.end(),[](const std::string& item){return item.rfind("PATH=",0)==0||item.rfind("GIT_CONFIG_GLOBAL=",0)==0;}),env.end());
    env.push_back("SystemRoot="+system.u8string());env.push_back("WINDIR="+system.u8string());env.push_back("PATH="+(system/"System32").u8string()+";"+system.u8string());env.push_back("GIT_CONFIG_GLOBAL=NUL");
#endif
    if(policy.role==ProcessRole::Network)for(const char* key:{"HTTP_PROXY","HTTPS_PROXY","ALL_PROXY","NO_PROXY","http_proxy","https_proxy","all_proxy","no_proxy"})if(const char* value=std::getenv(key))env.push_back(std::string(key)+"="+value);
#if defined(__APPLE__)
    if(!unsafe){if(!fs::exists("/usr/bin/sandbox-exec")){error="sandbox-exec missing";return false;}
      auto quote=[](const std::string& value){std::string out="\"";for(char c:value){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';};
      std::string profile="(version 1)(deny default)(allow process-exec)(allow sysctl-read)(allow file-read-metadata)(allow file-read-data (literal \"/\"))(allow signal (target children))";
      if(policy.role!=ProcessRole::Frontend)profile+="(allow process-fork)";
      for(const auto& path:reads)if(fs::exists(path,ec)){auto real=fs::canonical(path,ec);if(!ec)profile+="(allow file-read* (subpath "+quote(real.string())+"))";}
      for(const auto& path:writes){auto real=fs::weakly_canonical(path,ec);if(!ec)profile+="(allow file-read* file-write* (subpath "+quote(real.string())+"))";}
      if(policy.role!=ProcessRole::Network)profile+="(deny file-read* file-write* (require-all (subpath "+quote(execution.private_dir.string())+") (require-not (subpath "+quote(scratch.string())+"))))";
      profile+="(allow file-write-data (literal \"/dev/null\"))";
      auto protected_path=[&](const fs::path& path){auto real=fs::weakly_canonical(path);return quote(real.string());};
      const auto home=getenv_or("HOME");const auto credentials=fs::path(getenv_or("CODEX_CPP_HOME",home+"/.codex-cpp"))/"auth.json";profile+="(deny file-read* file-write* (subpath "+protected_path(credentials)+"))";for(const auto& secret:{fs::path(home)/".ssh",fs::path(home)/".aws",fs::path(home)/".codex/auth.json",fs::path(home)/".codex-cpp/auth.json",fs::path(home)/"Library/Keychains"})profile+="(deny file-read* file-write* (subpath "+protected_path(secret)+"))";
      if(policy.role==ProcessRole::Network)profile+="(allow network-outbound)(allow mach-lookup (global-name \"com.apple.system.opendirectoryd.libinfo\") (global-name \"com.apple.trustd.agent\") (global-name \"com.apple.trustd\"))";
      if(policy.role==ProcessRole::Clipboard)profile+="(allow mach-lookup (global-name \"com.apple.pasteboard.1\"))";
      command.insert(command.begin(),{"/usr/bin/sandbox-exec","-p",profile});}
#elif defined(__linux__)
    if(!unsafe){if(!fs::exists("/usr/bin/bwrap")){error="bubblewrap missing";return false;}
      std::vector<std::string> wrapped={"/usr/bin/bwrap","--unshare-all","--die-with-parent","--new-session","--cap-drop","ALL","--proc","/proc","--dev","/dev","--tmpfs","/tmp"};
      if(policy.role==ProcessRole::Network)wrapped.push_back("--share-net");
      for(const auto& path:reads)if(fs::exists(path,ec)){wrapped.insert(wrapped.end(),{"--ro-bind",path.string(),path.string()});}
      for(const auto& path:writes)wrapped.insert(wrapped.end(),{"--bind",path.string(),path.string()});
      wrapped.insert(wrapped.end(),{"--chdir",cwd.string(),"--"});wrapped.insert(wrapped.end(),command.begin(),command.end());command=std::move(wrapped);}
#elif defined(_WIN32)
    if(!unsafe){error="DENIED: Windows AppContainer backend unavailable";return false;}
#endif
#if defined(_WIN32)
    // Explicitly unsafe compatibility path. Job Objects manage lifetime, not isolation.
    SECURITY_ATTRIBUTES inherit{sizeof inherit,nullptr,TRUE};HANDLE child_in=nullptr,child_out=nullptr;
    if(!CreatePipe(&child_in,&input,&inherit,65536)||!CreatePipe(&output,&child_out,&inherit,65536)){error="cannot create process pipes";if(child_in)CloseHandle(child_in);if(child_out)CloseHandle(child_out);stop();return false;}
    SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
    std::wstring line;for(const auto& arg:command){std::wstring wide=fs::u8path(arg).wstring();if(!line.empty())line+=L' ';line+=L'"';size_t slashes=0;for(wchar_t ch:wide){if(ch==L'\\'){++slashes;continue;}line.append(ch==L'"'?slashes*2+1:slashes,L'\\');line+=ch;slashes=0;}line.append(slashes*2,L'\\');line+=L'"';}
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=child_in;startup.hStdOutput=child_out;HANDLE quiet=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);startup.hStdError=merge_stderr?child_out:quiet;
    job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    bool ok=job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits);PROCESS_INFORMATION child{};
    std::sort(env.begin(),env.end(),[](std::string a,std::string b){std::transform(a.begin(),a.end(),a.begin(),::toupper);std::transform(b.begin(),b.end(),b.begin(),::toupper);return a<b;});
    std::wstring block;for(const auto& item:env){block+=fs::u8path(item).wstring();block+=L'\0';}block+=L'\0';
    if(ok)ok=CreateProcessW(nullptr,line.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,block.data(),cwd.c_str(),&startup,&child)!=0;
    CloseHandle(child_in);CloseHandle(child_out);if(quiet!=INVALID_HANDLE_VALUE)CloseHandle(quiet);
    if(!ok){error="cannot start process";stop();return false;}process=child.hProcess;if(!AssignProcessToJobObject(job,process)){CloseHandle(child.hThread);error="cannot assign process job";stop();return false;}ResumeThread(child.hThread);CloseHandle(child.hThread);return true;
#else
    int incoming[2],outgoing[2];if(::pipe(incoming)!=0){error="pipe failed";return false;}if(::pipe(outgoing)!=0){::close(incoming[0]);::close(incoming[1]);error="pipe failed";return false;}
    std::vector<char*> argv,environment;for(auto& arg:command)argv.push_back(arg.data());argv.push_back(nullptr);for(auto& item:env)environment.push_back(item.data());environment.push_back(nullptr);
#if defined(__APPLE__)
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT|POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attributes,0);
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);posix_spawn_file_actions_adddup2(&actions,incoming[0],STDIN_FILENO);posix_spawn_file_actions_adddup2(&actions,outgoing[1],STDOUT_FILENO);
    if(merge_stderr)posix_spawn_file_actions_adddup2(&actions,outgoing[1],STDERR_FILENO);else posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    posix_spawn_file_actions_addchdir_np(&actions,cwd.c_str());
#pragma clang diagnostic pop
    const int spawned=posix_spawn(&pid,argv.front(),&actions,&attributes,argv.data(),environment.data());posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);if(spawned){pid=-1;error=std::strerror(spawned);}
#else
    const long maximum=::sysconf(_SC_OPEN_MAX);pid=::fork();if(pid==0){::setsid();::dup2(incoming[0],STDIN_FILENO);::dup2(outgoing[1],STDOUT_FILENO);if(merge_stderr)::dup2(outgoing[1],STDERR_FILENO);else{int quiet=::open("/dev/null",O_WRONLY);if(quiet>=0)::dup2(quiet,STDERR_FILENO);}
#ifdef SYS_close_range
      if(::syscall(SYS_close_range,3u,~0u,0)!=0)
#endif
      for(int fd=3;fd<maximum;++fd)::close(fd);
      if(::chdir(cwd.c_str())!=0)_exit(126);::execve(argv.front(),argv.data(),environment.data());_exit(127);}
#endif
    ::close(incoming[0]);::close(outgoing[1]);if(pid<0){::close(incoming[1]);::close(outgoing[0]);error="fork failed";return false;}
    input=incoming[1];output=outgoing[0];::fcntl(input,F_SETFL,::fcntl(input,F_GETFL,0)|O_NONBLOCK);::fcntl(output,F_SETFL,::fcntl(output,F_GETFL,0)|O_NONBLOCK);return true;
#endif
  }
  // Positive: bytes; zero: timeout; -1: EOF; -2: error.
  int receive(char* data,size_t size,int timeout=100){if(eof)return -1;
#if defined(_WIN32)
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);do{DWORD available=0;if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)){eof=true;return -1;}
      if(available){DWORD n=0;if(!ReadFile(output,data,std::min<DWORD>(available,static_cast<DWORD>(size)),&n,nullptr))return -2;return n?static_cast<int>(n):-1;}if(timeout)Sleep(5);
    }while(std::chrono::steady_clock::now()<deadline);return 0;
#else
    pollfd fd{output,POLLIN,0};const int ready=::poll(&fd,1,timeout);if(ready<0)return errno==EINTR?0:-2;if(!ready)return 0;const auto n=::read(output,data,size);if(n==0){eof=true;return -1;}if(n<0)return errno==EAGAIN||errno==EINTR?0:-2;return static_cast<int>(n);
#endif
  }
  bool send(const std::string& data,std::string& error,int timeout_seconds=5){if(data.size()>1'000'000){error="process frame too large";return false;}const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(timeout_seconds);size_t pos=0;
    while(pos<data.size()){if(std::chrono::steady_clock::now()>=deadline){error="process write timed out";return false;}
#if defined(_WIN32)
      DWORD written=0;if(!WriteFile(input,data.data()+pos,static_cast<DWORD>(data.size()-pos),&written,nullptr)||!written){error="process input closed";return false;}pos+=written;
#else
      pollfd fd{input,POLLOUT,0};const int ready=::poll(&fd,1,100);if(ready<0&&errno!=EINTR){error="process write failed";return false;}if(ready>0){const auto n=::write(input,data.data()+pos,data.size()-pos);if(n>0)pos+=static_cast<size_t>(n);else if(n<0&&errno!=EAGAIN&&errno!=EINTR){error="process input closed";return false;}}
#endif
    }return true;}
  void close_input(){
#if defined(_WIN32)
    if(input){CloseHandle(input);input=nullptr;}
#else
    if(input>=0){::close(input);input=-1;}
#endif
  }
  int wait(int timeout=1000){
#if defined(_WIN32)
    if(!process)return exit_code;if(WaitForSingleObject(process,timeout)!=WAIT_OBJECT_0)return -1;DWORD code=0;GetExitCodeProcess(process,&code);exit_code=static_cast<int>(code);CloseHandle(process);process=nullptr;
#else
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);while(pid>0){int status=0;const auto done=::waitpid(pid,&status,WNOHANG);if(done==pid){exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);::kill(-pid,SIGKILL);pid=-1;break;}if(done<0&&errno!=EINTR){pid=-1;break;}if(std::chrono::steady_clock::now()>=end)return -1;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
#endif
    return exit_code;}
  void stop(){close_input();
#if defined(_WIN32)
    if(job){CloseHandle(job);job=nullptr;}if(process){TerminateProcess(process,1);wait(1000);}if(output){CloseHandle(output);output=nullptr;}
#else
    if(pid>0){::kill(-pid,SIGTERM);if(wait(200)<0&&pid>0){::kill(-pid,SIGKILL);wait(1000);}}if(output>=0){::close(output);output=-1;}
#endif
    std::error_code ec;if(!scratch.empty()){fs::remove_all(scratch,ec);scratch.clear();}
  }
};
static bool sandbox_available(){static const bool available=[](){ManagedProcess process;ProcessPolicy policy;policy.role=ProcessRole::Probe;std::string error;
#if defined(_WIN32)
  return false;
#else
  if(!process.start({"/usr/bin/true"},policy,error,true)){sandbox_failure=error;return false;}process.close_input();char data[1024];for(int i=0;i<30;++i){const int n=process.receive(data,sizeof data,100);if(n<0)break;if(n>0&&sandbox_failure.size()<2048)sandbox_failure.append(data,static_cast<size_t>(n));}const int status=process.wait(1000);if(status&&sandbox_failure.empty())sandbox_failure="probe exit "+std::to_string(status);return status==0;
#endif
}();return available;}
static std::vector<std::string> shell_argv(const std::string& command){
#if defined(_WIN32)
return {getenv_or("COMSPEC","C:\\Windows\\System32\\cmd.exe"),"/d","/s","/c",command};
#else
return {"/bin/sh","-c",command};
#endif
}

struct TerminalInputScope{
#if !defined(_WIN32)
  termios saved{};bool valid=false;explicit TerminalInputScope(bool on){if(on&&isatty(STDIN_FILENO)&&tcgetattr(STDIN_FILENO,&saved)==0){auto raw=saved;raw.c_lflag&=static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_iflag&=static_cast<tcflag_t>(~(ICRNL|IXON));raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;valid=tcsetattr(STDIN_FILENO,TCSANOW,&raw)==0;}}
  ~TerminalInputScope(){if(valid)tcsetattr(STDIN_FILENO,TCSANOW,&saved);}
#else
  explicit TerminalInputScope(bool){}
#endif
};
