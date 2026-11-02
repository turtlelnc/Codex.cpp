// Explicit, host-only updater. Downloads never execute code or replace a program.
static void host_detached(const std::vector<std::string>& args){
  if(args.empty())throw std::runtime_error("Missing host command");
#if defined(_WIN32)
  std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=L'"';size_t slashes=0;for(wchar_t c:fs::u8path(arg).wstring()){if(c==L'\\'){++slashes;continue;}command.append(c==L'"'?slashes*2+1:slashes,L'\\');command+=c;slashes=0;}command.append(slashes*2,L'\\');command+=L'"';}STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION child{};if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child))throw std::runtime_error("Cannot start host helper");CloseHandle(child.hThread);CloseHandle(child.hProcess);
#else
  std::vector<char*> pointers;for(const auto& arg:args)pointers.push_back(const_cast<char*>(arg.c_str()));pointers.push_back(nullptr);posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);for(int fd=0;fd<3;++fd)posix_spawn_file_actions_addopen(&actions,fd,"/dev/null",fd==0?O_RDONLY:O_WRONLY,0);posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);short flags=POSIX_SPAWN_SETPGROUP;
#if defined(__APPLE__)
  flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&attributes,flags);posix_spawnattr_setpgroup(&attributes,0);extern char** environ;pid_t pid=-1;const int result=posix_spawnp(&pid,pointers[0],&actions,&attributes,pointers.data(),environ);posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);if(result)throw std::runtime_error("Cannot start host helper: "+std::string(std::strerror(result)));std::thread([pid](){int status;while(::waitpid(pid,&status,0)<0&&errno==EINTR){}}).detach();
#endif
}
namespace updater {
using native_web::require;
static fs::path self;
static bool hash_valid(const std::string& value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
static std::string asset(){
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
  return "codex-cpp-windows-arm64.exe";
#elif defined(_WIN64)
  return "codex-cpp-windows-x64.exe";
#else
  return "codex-cpp-windows-x86.exe";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
  return "codex-cpp-macos-arm64";
#else
  return "codex-cpp-macos-x64";
#endif
#else
#if defined(__aarch64__)
  const std::string base="codex-cpp-linux-arm64";
#elif defined(__x86_64__)
  const std::string base="codex-cpp-linux-x64";
#else
  throw std::runtime_error("No update asset for this CPU");
#endif
#if defined(__GLIBC__)
  return base+"-native";
#else
  return base;
#endif
#endif
}
static fs::path directory(){require(!portable_config.root.empty(),"Updater requires --portable-dir DIR");native_web::private_dir(portable_config.root);auto root=portable_config.root/"updates";native_web::private_dir(root);return root;}
static uint32_t little(const std::string& bytes,size_t offset){require(offset+4<=bytes.size(),"Truncated executable header");uint32_t value=0;for(int i=0;i<4;++i)value|=static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset+i]))<<(i*8);return value;}
static void binary(const std::string& bytes){require(bytes.size()>=64,"Update is not an executable");auto name=asset();
#if defined(_WIN32)
  require(bytes.substr(0,2)=="MZ","Update is not PE");auto pe=little(bytes,0x3c);require(pe<=1024*1024&&pe+24<=bytes.size()&&bytes.substr(pe,4)==std::string("PE\0\0",4),"Invalid PE header");auto cpu=little(bytes,pe+4)&65535;require(cpu==(name.find("arm64")!=std::string::npos?0xaa64:name.find("x64")!=std::string::npos?0x8664:0x14c),"Update CPU mismatch");
#elif defined(__APPLE__)
  require(little(bytes,0)==0xfeedfacf&&little(bytes,4)==(name.find("arm64")!=std::string::npos?0x0100000cu:0x01000007u),"Update Mach-O CPU mismatch");
#else
  require(bytes.substr(0,4)==std::string("\x7f" "ELF",4)&&static_cast<unsigned char>(bytes[4])==2&&static_cast<unsigned char>(bytes[5])==1,"Update is not ELF64 little endian");require((little(bytes,16)>>16)==(name.find("arm64")!=std::string::npos?183u:62u),"Update CPU mismatch");
#endif
}
static Json candidate(const std::string& tag,const std::shared_ptr<std::atomic<bool>>& cancel={}){
  require(!tag.empty()&&tag.size()<128&&tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"Invalid release tag");const std::string base="https://api.github.com/repos/turtlelnc/Codex.cpp/releases/";auto reply=native_http(base+(tag=="latest"?"latest":"tags/"+tag),{}, {"Accept: application/vnd.github+json","User-Agent: Codex.cpp-Updater"},true,{},[&](){if(cancel&&cancel->load())throw std::runtime_error("Update cancelled");},false,false);require(reply.error.empty()&&reply.status==200,"Release metadata unavailable: "+reply.error);auto release=plugin_json(reply.body);require(release&&release->kind==Json::Object,"Invalid release metadata");auto assets=release->get("assets");require(assets&&assets->kind==Json::Array,"Release has no assets");
  for(const auto& file:assets->items)if(plugin_string(file,"name")==asset()){auto hash=plugin_string(file,"digest"),url=plugin_string(file,"browser_download_url");require(hash.rfind("sha256:",0)==0&&hash_valid(hash.substr(7)),"Release asset has no SHA-256 digest");require(url.rfind("https://github.com/turtlelnc/Codex.cpp/releases/download/",0)==0,"Unexpected update origin");auto size=file.get("size");require(size&&size->kind==Json::Other,"Missing update size");size_t consumed=0;size_t count=std::stoull(size->value,&consumed);require(consumed==size->value.size(),"Invalid update size");require(count>=64&&count<=portable::limit,"Update exceeds size limit");return plugin_object({{"asset",plugin_string(asset())},{"tag",plugin_string(plugin_string(*release,"tag_name"))},{"sha256",plugin_string(hash.substr(7))},{"url",plugin_string(url)},{"size",plugin_string(std::to_string(count))},{"status",plugin_string("available")}});}
  throw std::runtime_error("No matching platform/CPU/runtime asset in this release");
}
static void download(const Json& info,const fs::path& file,const std::shared_ptr<std::atomic<bool>>& cancel={}){
  static const bool initialized=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;require(initialized,"HTTP init failed");CURL* easy=curl_easy_init();require(easy!=nullptr,"HTTP allocation failed");struct Cleanup{CURL* h;~Cleanup(){curl_easy_cleanup(h);}} cleanup{easy};std::ofstream stream(file,std::ios::binary);require(bool(stream),"Cannot stage update");const auto expected=static_cast<size_t>(std::stoull(plugin_text(info,"size")));struct Sink{std::ofstream& file;size_t count=0,expected;std::shared_ptr<std::atomic<bool>> cancel;} sink{stream,0,expected,cancel};
  const auto url=plugin_text(info,"url");curl_easy_setopt(easy,CURLOPT_URL,url.c_str());curl_easy_setopt(easy,CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(easy,CURLOPT_REDIR_PROTOCOLS_STR,"https");curl_easy_setopt(easy,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(easy,CURLOPT_MAXREDIRS,5L);curl_easy_setopt(easy,CURLOPT_CONNECTTIMEOUT,20L);curl_easy_setopt(easy,CURLOPT_TIMEOUT,300L);curl_easy_setopt(easy,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(easy,CURLOPT_USERAGENT,"Codex.cpp-Updater");auto ca=getenv_or("CURL_CA_BUNDLE",getenv_or("SSL_CERT_FILE"));
#if defined(__linux__)
  if(ca.empty())for(const char* path:{"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/cert.pem"})if(fs::is_regular_file(path)){ca=path;break;}
#endif
  if(!ca.empty())curl_easy_setopt(easy,CURLOPT_CAINFO,ca.c_str());auto capath=getenv_or("SSL_CERT_DIR");if(!capath.empty())curl_easy_setopt(easy,CURLOPT_CAPATH,capath.c_str());
  curl_easy_setopt(easy,CURLOPT_WRITEFUNCTION,+[](char* p,size_t size,size_t count,void* opaque)->size_t{auto& s=*static_cast<Sink*>(opaque);size_t n=size*count;if((s.cancel&&s.cancel->load())||n>s.expected-s.count)return 0;s.file.write(p,static_cast<std::streamsize>(n));if(!s.file)return 0;s.count+=n;return n;});curl_easy_setopt(easy,CURLOPT_WRITEDATA,&sink);curl_easy_setopt(easy,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(easy,CURLOPT_XFERINFOFUNCTION,+[](void* p,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int{auto& s=*static_cast<Sink*>(p);return s.cancel&&s.cancel->load()?1:0;});curl_easy_setopt(easy,CURLOPT_XFERINFODATA,&sink);auto result=curl_easy_perform(easy);long status=0;curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);stream.flush();require(result==CURLE_OK&&status==200&&sink.count==expected&&bool(stream),"Update download failed, cancelled or truncated");
}
static Json stage_bytes(const fs::path& root,Json info,const std::string& bytes){const auto hash=plugin_text(info,"sha256");require(hash_valid(hash)&&native_web::digest(bytes)==hash,"Update SHA-256 mismatch");binary(bytes);auto file=root/("payload-"+hash+".bin");if(fs::exists(file))require(native_web::digest(native_web::read_bytes(file,portable::limit))==hash,"Cached update changed");else portable::save_new(file,bytes);info.fields["status"]=plugin_string("ready");info.fields["target"]=plugin_string(self.u8string());native_web::atomic_save(root/"pending.json",info);return info;}
static Json status(){auto root=directory();auto info=fs::exists(root/"pending.json")?native_web::read_json(root/"pending.json"):plugin_object({{"status",plugin_string("idle")},{"asset",plugin_string(asset())}});if(fs::exists(root/"result.json"))info.fields["last_install"]=native_web::read_json(root/"result.json");return info;}
static CommandResult fetch(const std::string& tag,bool get,const std::shared_ptr<std::atomic<bool>>& cancel={}){try{auto root=directory();portable::Lease operation(root,true,".operation.lock");auto info=candidate(tag,cancel);native_web::atomic_save(root/"available.json",info);if(get){portable::Stage stage(root);download(info,stage.path/"download",cancel);auto bytes=native_web::read_bytes(stage.path/"download",portable::limit);info=stage_bytes(root,info,bytes);}return {0,json_dump(info)};}catch(const std::exception& e){return {2,e.what()};}}
static long process_id(){
#if defined(_WIN32)
  return static_cast<long>(GetCurrentProcessId());
#else
  return static_cast<long>(::getpid());
#endif
}
static bool alive(long pid){
#if defined(_WIN32)
  HANDLE h=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(pid));if(!h)return false;bool running=WaitForSingleObject(h,0)==WAIT_TIMEOUT;CloseHandle(h);return running;
#else
  return ::kill(static_cast<pid_t>(pid),0)==0||errno==EPERM;
#endif
}
static Json rollback_receipt(const fs::path& root,const fs::path& target){
  const auto hash=native_web::digest(creator_read(target.parent_path(),target.filename().u8string(),portable::limit));
  for(const auto& name:{"recovery.json","installed.json"})if(fs::is_regular_file(root/name)){auto info=native_web::read_json(root/name);if(plugin_string(info,"target")==target.u8string()&&plugin_string(info,"installed_sha256")==hash)return info;}
  throw std::runtime_error("No matching rollback receipt for this executable");
}
static void install(const fs::path& target,bool rollback,const std::string& expected){auto root=directory();portable::Lease operation(root,true,".operation.lock");portable::Lease lease(portable_config.root,true);auto info=rollback?rollback_receipt(root,target):status();const auto hash=plugin_text(info,rollback?"backup_sha256":"sha256");require(hash_valid(hash)&&hash==expected,"Update changed after confirmation");require(fs::absolute(fs::u8path(plugin_text(info,"target"))).lexically_normal()==fs::absolute(target).lexically_normal(),"Update target mismatch");fs::path source;
  if(rollback){auto name=plugin_text(info,"backup");require(portable::safe_name(name)&&fs::u8path(name).filename()==fs::u8path(name),"Invalid rollback path");source=target.parent_path()/fs::u8path(name);require(native_web::digest(creator_read(target.parent_path(),target.filename().u8string(),portable::limit))==plugin_text(info,"installed_sha256"),"Installed executable changed; refusing rollback");}else source=root/("payload-"+hash+".bin");
  auto bytes=creator_read(source.parent_path(),source.filename().u8string(),portable::limit);require(native_web::digest(bytes)==hash,"Staged update changed");binary(bytes);auto previous=creator_read(target.parent_path(),target.filename().u8string(),portable::limit);const auto old_hash=native_web::digest(previous);require(old_hash!=hash,"Executable already matches the selected update");portable::Stage stage(target.parent_path());std::string error;auto replacement=stage.path/target.filename();require(write_all(replacement,bytes,error),error);
#if !defined(_WIN32)
  const auto permissions=fs::status(target).permissions()&(fs::perms::owner_all|fs::perms::group_all|fs::perms::others_all);fs::permissions(replacement,permissions,fs::perm_options::replace);
#endif
  auto capabilities=plugin_json(native_web::command({replacement.u8string(),"portable","capabilities"},15,{{"CODEX_CPP_PORTABLE_DIR",""},{"CODEX_CPP_PORTABLE_PROJECT",""}}));require(capabilities&&plugin_string(*capabilities,"application")=="codex-cpp"&&plugin_integer(*capabilities,"format")==1&&plugin_integer(*capabilities,"update_protocol")==1,"Update cannot read this portable data format");
  const auto backup=target.parent_path()/(target.filename().u8string()+".backup-"+random_id("update"));require(native_web::digest(creator_read(target.parent_path(),target.filename().u8string(),portable::limit))==old_hash,"Executable changed during update");
native_web::atomic_save(root/"recovery.json",plugin_object({{"target",plugin_string(target.u8string())},{"backup",plugin_string(backup.filename().u8string())},{"backup_sha256",plugin_string(old_hash)},{"installed_sha256",plugin_string(hash)},{"status",plugin_string("prepared")}}));
#if defined(_WIN32)
  if(!ReplaceFileW(target.c_str(),replacement.c_str(),backup.c_str(),REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr)){if(!fs::exists(target)&&fs::exists(backup))MoveFileExW(backup.c_str(),target.c_str(),0);throw std::runtime_error("Cannot replace executable; old version retained or restored");}
#else
  portable::save_new(backup,previous);fs::permissions(backup,permissions,fs::perm_options::replace);fs::rename(replacement,target);
#endif
  try{
    native_web::atomic_save(root/"installed.json",plugin_object({{"target",plugin_string(target.u8string())},{"backup",plugin_string(backup.filename().u8string())},{"backup_sha256",plugin_string(old_hash)},{"installed_sha256",plugin_string(hash)},{"status",plugin_string(rollback?"rolled_back":"installed")}}));
    native_web::atomic_save(root/"pending.json",plugin_object({{"status",plugin_string(rollback?"rolled_back":"installed")},{"sha256",plugin_string(hash)}}));
    native_web::atomic_save(root/"result.json",plugin_object({{"status",plugin_string(rollback?"rolled_back":"installed")},{"sha256",plugin_string(hash)}}));
  }catch(...){bool restored=false;try{if(native_web::digest(creator_read(target.parent_path(),target.filename().u8string(),portable::limit))==hash&&native_web::digest(creator_read(backup.parent_path(),backup.filename().u8string(),portable::limit))==old_hash){
#if defined(_WIN32)
    restored=ReplaceFileW(target.c_str(),backup.c_str(),nullptr,REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr)!=0;
#else
    fs::rename(backup,target);restored=true;
#endif
  }}catch(...){}throw std::runtime_error(restored?"Update metadata failed; previous executable restored":"Update metadata failed; recovery receipt and backup retained for manual recovery");}

}
static void schedule(bool rollback){auto root=directory();auto info=rollback?rollback_receipt(root,self):status();auto hash=plugin_text(info,rollback?"backup_sha256":"sha256");require(hash_valid(hash)&&(rollback||plugin_string(info,"status")=="ready"),"No verified update is ready");auto helper=root/(random_id("installer")+
#if defined(_WIN32)
  ".exe"
#else
  ".bin"
#endif
  );portable::save_new(helper,creator_read(self.parent_path(),self.filename().u8string(),portable::limit));
#if !defined(_WIN32)
  fs::permissions(helper,fs::perms::owner_read|fs::perms::owner_exec,fs::perm_options::replace);
#endif
  host_detached({helper.u8string(),"--portable-dir",portable_config.root.u8string(),"update","apply",self.u8string(),std::to_string(process_id()),rollback?"rollback":"install",hash});
}
static int cli(int argc,char** argv){try{require(argc>=3,"Usage: update check|download [TAG] | stage FILE --sha256 HASH | status | install|rollback --yes");std::string action=argv[2];directory();
  if((action=="check"||action=="download")&&(argc==3||argc==4)){auto result=fetch(argc==4?argv[3]:"latest",action=="download");std::cout<<result.output<<'\n';return result.exit_code;}
  if(action=="status"&&argc==3){std::cout<<json_dump(status())<<'\n';return 0;}
  if(action=="stage"&&argc==6&&std::string(argv[4])=="--sha256"){auto root=directory();portable::Lease operation(root,true,".operation.lock");auto bytes=native_web::read_bytes(fs::absolute(fs::u8path(argv[3])),portable::limit);auto info=plugin_object({{"sha256",plugin_string(argv[5])},{"tag",plugin_string("offline-user-selected")},{"asset",plugin_string(asset())}});std::cout<<json_dump(stage_bytes(root,info,bytes))<<'\n';return 0;}
  if((action=="install"||action=="rollback")&&argc==4&&std::string(argv[3])=="--yes"){
#if defined(_WIN32)
    {portable::Lease lease(portable_config.root,true);}schedule(action=="rollback");std::cout<<"Installer scheduled; this process will exit before replacement. Inspect update status.\n";
#else
    auto info=action=="rollback"?rollback_receipt(directory(),self):status();install(self,action=="rollback",plugin_text(info,action=="rollback"?"backup_sha256":"sha256"));std::cout<<"Executable replaced; portable data preserved and backup retained\n";
#endif
    return 0;}
  if(action=="apply"&&argc==7){auto target=fs::absolute(fs::u8path(argv[3]));long parent=std::stol(argv[4]);require(parent>1&&parent!=process_id()&&(std::string(argv[5])=="install"||std::string(argv[5])=="rollback"),"Invalid installer request");auto until=std::chrono::steady_clock::now()+std::chrono::seconds(120);while(alive(parent)){require(std::chrono::steady_clock::now()<until,"Parent has not exited; update not installed");std::this_thread::sleep_for(std::chrono::milliseconds(100));}install(target,std::string(argv[5])=="rollback",argv[6]);return 0;}
  throw std::runtime_error("Invalid update command; installation requires explicit --yes");
}catch(const std::exception& e){std::cerr<<"Update: "<<e.what()<<'\n';if(argc>2&&(std::string(argv[2])=="apply"||std::string(argv[2])=="install"||std::string(argv[2])=="rollback"))try{native_web::atomic_save(directory()/"result.json",plugin_object({{"status",plugin_string("failed")},{"error",plugin_string(e.what())}}));}catch(...){}return 2;}}
struct Background{std::shared_ptr<std::atomic<bool>> cancel=std::make_shared<std::atomic<bool>>(false);std::future<CommandResult> task;~Background(){cancel->store(true);if(task.valid())task.wait();}std::string start(const std::string& tag,bool download){require(!portable_config.root.empty(),"Restart with --portable-dir DIR to use updates");require(!task.valid()||task.wait_for(std::chrono::seconds(0))==std::future_status::ready,"Update job already running");if(task.valid())task.get();cancel->store(false);task=std::async(std::launch::async,[tag,download,cancel=cancel](){return fetch(tag,download,cancel);});return "Update job started in background; continue working, use /update status to inspect. Nothing will be installed automatically.";}std::string report(){if(task.valid()){if(task.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return "Update download/check in progress; current task is unaffected.";auto result=task.get();return result.output+(result.exit_code?"":"\nUse /update install to review and confirm installation.");}return json_dump(status());}};
} // namespace updater
