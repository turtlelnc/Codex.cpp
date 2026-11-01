// Explicit startup-only native modules. The loader is never reachable by RPC or
// model tool calls. A module is trusted application code, not a sandboxed tool.
#include "penguin/native.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
struct PenguinToolDefinition{std::string service,spec,name;bool mutates=true;};
struct PenguinWebModule{std::string id,entry,style;fs::path root;};
static std::vector<PenguinToolDefinition> penguin_tools;
static std::vector<PenguinWebModule> penguin_web_modules;
struct PenguinLibrary{
#if defined(_WIN32)
  HMODULE handle=nullptr;
#else
  void* handle=nullptr;
#endif
  PenguinModuleV1 module{};
  ~PenguinLibrary(){if(module.dispose){try{module.dispose(module.state);}catch(...){}}
    if(handle){
#if defined(_WIN32)
      FreeLibrary(handle);
#else
      dlclose(handle);
#endif
    }
  }
};
struct PenguinNativeCall{
  const penguin::Emit& emit;const penguin::Next& next;
  std::string result,error;std::exception_ptr exception;size_t bytes=0;bool has_result=false;
  static void send(void* opaque,const char* channel,const char* bytes,size_t length) noexcept{
    auto& call=*static_cast<PenguinNativeCall*>(opaque);if(call.exception)return;
    try{if(!channel||(!bytes&&length)||length>16*1024*1024||call.bytes+length>32*1024*1024)throw std::runtime_error("Penguin response exceeds limit");call.bytes+=length;
      std::string kind(channel),value(bytes?bytes:"",length);
      if(kind=="result"){if(call.has_result)throw std::runtime_error("Penguin returned multiple results");call.result=std::move(value);call.has_result=true;}
      else if(kind=="error")call.error=std::move(value);
      else if(kind=="text"||kind=="reasoning"){if(call.emit)call.emit(kind,value);}
      else throw std::runtime_error("Unknown Penguin event channel");
    }catch(...){call.exception=std::current_exception();}
  }
  static int poll(void* opaque) noexcept{auto& call=*static_cast<PenguinNativeCall*>(opaque);if(call.exception)return 1;try{if(call.emit)call.emit("tick","");return 0;}catch(...){call.exception=std::current_exception();return 1;}}
  static int forward(void* opaque,const char* request,size_t length,PenguinEmitV1 emit,void* context) noexcept{
    auto& call=*static_cast<PenguinNativeCall*>(opaque);try{
      if(!request||length>16*1024*1024||!emit)throw std::runtime_error("Invalid Penguin next request");
      auto result=call.next(std::string(request,length),[&](const std::string& kind,const std::string& value){emit(context,kind.c_str(),value.data(),value.size());});
      emit(context,"result",result.data(),result.size());return 0;
    }catch(const std::exception& e){auto error=std::string(e.what());if(emit)emit(context,"error",error.data(),error.size());return 1;}
    catch(...){return 1;}
  }
};
static void penguin_load_manifest(const fs::path& path){
  std::error_code ec;if(!path.is_absolute()||!fs::is_regular_file(path,ec)||ec||fs::file_size(path,ec)>65536)throw std::runtime_error("Invalid Penguin manifest path/size");
  auto manifest=plugin_json(slurp(path,65537));if(!manifest||manifest->kind!=Json::Object||penguin_integer(*manifest,"api")!=1)throw std::runtime_error("Penguin manifest API 1 required");
  const auto id=penguin_text(*manifest,"id");if(!plugin_id(id)||id.rfind("builtin",0)==0)throw std::runtime_error("Invalid Penguin id");
  if(penguin_runtime().has_module(id))throw std::runtime_error("Invalid/duplicate Penguin module: "+id);
  const auto root=fs::weakly_canonical(path.parent_path());penguin::Module module;module.id=id;
  if(auto dependencies=manifest->get("requires")){if(dependencies->kind!=Json::Array)throw std::runtime_error("Penguin requires must be an array");for(const auto& dependency:dependencies->items){if(dependency.kind!=Json::String)throw std::runtime_error("Invalid Penguin dependency");module.dependencies.push_back(dependency.str());}}
  std::map<std::string,std::string> declared;
  if(auto services=manifest->get("services")){if(services->kind!=Json::Object||services->fields.size()>64)throw std::runtime_error("Invalid Penguin services");for(const auto& entry:services->fields){if(entry.second.kind!=Json::String)throw std::runtime_error("Penguin service replacement must name an owner (or empty for a new service)");declared.emplace(entry.first,entry.second.str());}}
  std::shared_ptr<PenguinLibrary> library;
  if(auto libraries=manifest->get("libraries")){
    if(declared.empty())throw std::runtime_error("Native Penguin must declare at least one service");
#if defined(_WIN32)
    const char* platform="windows";
#elif defined(__APPLE__)
    const char* platform="macos";
#else
    const char* platform="linux";
#endif
    auto file=libraries->get(platform);if(!file||file->kind!=Json::String)throw std::runtime_error("Penguin library missing for this platform");const auto target=root/fs::u8path(file->str());
    if(!within_root(root,target)||!fs::is_regular_file(target,ec)||ec)throw std::runtime_error("Penguin library escapes bundle or is missing");
    library=std::make_shared<PenguinLibrary>();PenguinInitV1 init=nullptr;
#if defined(_WIN32)
    library->handle=LoadLibraryExW(target.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(library->handle)init=reinterpret_cast<PenguinInitV1>(GetProcAddress(library->handle,"codex_penguin_init_v1"));
#else
    library->handle=dlopen(target.c_str(),RTLD_NOW|RTLD_LOCAL);if(!library->handle){auto reason=dlerror();throw std::runtime_error(std::string("Penguin native loader unavailable: ")+(reason?reason:"unknown error"));}
    init=reinterpret_cast<PenguinInitV1>(dlsym(library->handle,"codex_penguin_init_v1"));
#endif
    if(!init)throw std::runtime_error("Penguin native API entrypoint missing");
    struct Registration{penguin::Module& module;const std::map<std::string,std::string>& declared;std::shared_ptr<PenguinLibrary> library;std::string error;};
    Registration registration{module,declared,library,{}};
    PenguinHostV1 host{1,sizeof(PenguinHostV1),&registration,[](void* opaque,const char* service,const char* replaces,PenguinHandlerV1 handler,void* state)->int{
      auto& r=*static_cast<Registration*>(opaque);try{if(!service||!replaces||!handler)throw std::runtime_error("Invalid Penguin binding");auto it=r.declared.find(service);if(it==r.declared.end()||it->second!=replaces)throw std::runtime_error("Undeclared Penguin binding");
        r.module.contributions.push_back({service,replaces,[library=r.library,handler,state](const std::string& request,const penguin::Emit& emit,const penguin::Next& next){
          PenguinNativeCall call{emit,next,{},{},{},0,false};PenguinCallV1 invocation{1,sizeof(PenguinCallV1),PenguinNativeCall::send,&call,PenguinNativeCall::forward,&call,PenguinNativeCall::poll};
          int code=handler(state,request.data(),request.size(),&invocation);if(call.exception)std::rethrow_exception(call.exception);if(!call.error.empty())throw std::runtime_error(call.error);if(code||!call.has_result)throw std::runtime_error("Penguin handler failed or returned no result");return call.result;
        }});return 0;
      }catch(const std::exception& e){r.error=e.what();return 1;}catch(...){r.error="Penguin registration failed";return 1;}
    }};
    library->module.version=1;library->module.size=sizeof(PenguinModuleV1);
    if(init(&host,&library->module)||!registration.error.empty())throw std::runtime_error("Penguin init failed: "+registration.error);
    if(library->module.version!=1||library->module.size<sizeof(PenguinModuleV1)||!library->module.id||id!=library->module.id)throw std::runtime_error("Penguin ABI/id mismatch");
  }else if(!declared.empty())throw std::runtime_error("Penguin services require a native library");
  if(module.contributions.size()!=declared.size())throw std::runtime_error("Penguin did not bind every declared service");
  std::vector<PenguinToolDefinition> tools;
  if(auto definitions=manifest->get("tools")){if(definitions->kind!=Json::Array||definitions->items.size()>32)throw std::runtime_error("Invalid Penguin tools");std::set<std::string> names={"read_file","list_dir","read_skill","write_file","shell","apply_patch","git_read","git_write","design_review","plugin_ui","penguin_scaffold","penguin_check","penguin_build","penguin_test"};for(const auto& tool:penguin_tools)names.insert(tool.name);
    for(const auto& definition:definitions->items){auto spec=definition.get("spec");if(!spec||spec->kind!=Json::Object)throw std::runtime_error("Penguin tool spec missing");auto name=penguin_text(*spec,"name");if(!plugin_id(name)||!names.insert(name).second||!declared.count("tools."+name)||!spec->get("parameters"))throw std::runtime_error("Invalid/duplicate Penguin tool definition");tools.push_back({"tools."+name,json_dump(*spec),name,penguin_flag(definition,"mutates")});}}
  std::optional<PenguinWebModule> web;
  if(auto definition=manifest->get("web")){auto entry=penguin_text(*definition,"entry");auto style=plugin_string(*definition,"style");
    for(const auto& file:{entry,style})if(!file.empty()&&(!within_root(root,root/fs::u8path(file))||!fs::is_regular_file(root/fs::u8path(file),ec)||ec))throw std::runtime_error("Penguin Web asset escapes bundle or is missing");
    if(fs::u8path(entry).extension()!=".js"&&fs::u8path(entry).extension()!=".mjs")throw std::runtime_error("Penguin Web entry must be JavaScript");if(!style.empty()&&fs::u8path(style).extension()!=".css")throw std::runtime_error("Penguin Web style must be CSS");web=PenguinWebModule{id,entry,style,root};
  }
  penguin_runtime().mount(std::move(module));
  penguin_tools.insert(penguin_tools.end(),tools.begin(),tools.end());if(web)penguin_web_modules.push_back(*web);
}
static void penguin_load_reviewed(const fs::path& path,const std::string& fingerprint);
static void penguin_boot(){penguin_builtins();for(const auto& path:penguin_manifests){auto review=penguin_reviews.find(path);if(review!=penguin_reviews.end())penguin_load_reviewed(fs::u8path(path),review->second);else penguin_load_manifest(fs::u8path(path));}penguin_runtime().freeze();}
class PenguinTool final:public ToolRuntime{
  PenguinToolDefinition definition_;
public:explicit PenguinTool(PenguinToolDefinition definition):definition_(std::move(definition)){}
  std::string name()const override{return definition_.name;}std::string spec_json()const override{return definition_.spec;}bool mutates()const override{return definition_.mutates;}
  ToolResult run(const std::string& args,const fs::path& root)override{
    auto input=penguin_object({{"arguments",penguin_string(args)},{"workspace",penguin_string(root.u8string())}});auto result=penguin_call(definition_.service,input,[](const Json&,const penguin::Emit&)->Json{throw std::runtime_error("Penguin tool has no implementation");});
    return {penguin_integer(result,"exit_code"),penguin_text(result,"output")};
  }
};
static Json penguin_web_catalog(){Json out;out.kind=Json::Array;for(const auto& module:penguin_web_modules)out.items.push_back(penguin_object({{"id",penguin_string(module.id)},{"entry",penguin_string("/penguin/"+module.id+"/entry.js")},{"style",penguin_string(module.style.empty()?"":"/penguin/"+module.id+"/style.css")}}));return out;}
