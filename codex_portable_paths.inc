// Explicit local portable profile. These paths are host configuration, not RPC.
namespace native_web { std::string digest(std::string); }
struct PortableConfig { fs::path root;std::string project; };
static PortableConfig portable_config;
static void host_env(const char* key,const std::string& value){
#if defined(_WIN32)
  if(_putenv_s(key,value.c_str()))throw std::runtime_error("Cannot set process configuration");
#else
  if(setenv(key,value.c_str(),1))throw std::runtime_error("Cannot set process configuration");
#endif
}
static bool portable_id(const std::string& id){return !id.empty()&&id.size()<=64&&id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos;}
static void portable_arguments(int& argc,char**& argv,std::vector<char*>& storage){
  auto root=getenv_or("CODEX_CPP_PORTABLE_DIR"),project=getenv_or("CODEX_CPP_PORTABLE_PROJECT");int i=1;
  while(i<argc&&(std::string(argv[i])=="--portable-dir"||std::string(argv[i])=="--portable-project")){
    if(i+1>=argc)throw std::runtime_error("Portable option requires a value");if(std::string(argv[i])=="--portable-dir")root=argv[i+1];else project=argv[i+1];i+=2;
  }
  if(!project.empty()&&!portable_id(project))throw std::runtime_error("Invalid portable project ID");
  if(root.empty()&&!project.empty())throw std::runtime_error("--portable-project requires --portable-dir");
  if(!root.empty()){auto configured=fs::weakly_canonical(fs::absolute(fs::u8path(root)));if(configured==configured.root_path())throw std::runtime_error("Do not use a filesystem root as a portable directory");while(configured.filename().empty())configured=configured.parent_path();portable_config.root=configured;portable_config.project=project;host_env("CODEX_CPP_PORTABLE_DIR",portable_config.root.u8string());host_env("CODEX_CPP_PORTABLE_PROJECT",project);host_env("CODEX_CPP_HOME",(portable_config.root/"user").u8string());}
  if(i>1){storage.clear();storage.push_back(argv[0]);for(;i<argc;++i)storage.push_back(argv[i]);argc=static_cast<int>(storage.size());storage.push_back(nullptr);argv=storage.data();}
}
static std::string portable_project_key(const fs::path& root){return portable_config.project.empty()?native_web::digest(fs::weakly_canonical(root).generic_u8string()):portable_config.project;}
static fs::path project_state_root(const fs::path& root){return portable_config.root.empty()?root/".codex_cpp":portable_config.root/"projects"/portable_project_key(root);}
