// Explicit startup-only native modules. The loader is never reachable by RPC or
// model tool calls. A module is trusted application code, not a sandboxed tool.
#include "plugin/native.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
struct PluginToolDefinition{std::string service,spec,name;bool mutates=true;};
struct PluginWebModule{std::string id,entry,style;fs::path root;};
static std::vector<PluginToolDefinition> plugin_tools;
static std::vector<PluginWebModule> plugin_web_modules;
struct PluginLibrary{
#if defined(_WIN32)
  HMODULE handle=nullptr;
#else
  void* handle=nullptr;
#endif
  PluginModuleV1 module{};
  ~PluginLibrary(){if(module.dispose){try{module.dispose(module.state);}catch(...){}}
    if(handle){
#if defined(_WIN32)
      FreeLibrary(handle);
#else
      dlclose(handle);
#endif
    }
  }
};
struct PluginNativeCall{
  const plugin::Emit& emit;const plugin::Next& next;
  std::string result,error;std::exception_ptr exception;size_t bytes=0;bool has_result=false;
  static void send(void* opaque,const char* channel,const char* bytes,size_t length) noexcept{
    auto& call=*static_cast<PluginNativeCall*>(opaque);if(call.exception)return;
    try{if(!channel||(!bytes&&length)||length>16*1024*1024||call.bytes+length>32*1024*1024)throw std::runtime_error("Plugin response exceeds limit");call.bytes+=length;
      std::string kind(channel),value(bytes?bytes:"",length);
      if(kind=="result"){if(call.has_result)throw std::runtime_error("Plugin returned multiple results");call.result=std::move(value);call.has_result=true;}
      else if(kind=="error")call.error=std::move(value);
      else if(kind=="text"||kind=="reasoning"||kind=="activity"){if(call.emit)call.emit(kind,value);}
      else throw std::runtime_error("Unknown Plugin event channel");
    }catch(...){call.exception=std::current_exception();}
  }
  static int poll(void* opaque) noexcept{auto& call=*static_cast<PluginNativeCall*>(opaque);if(call.exception)return 1;try{if(call.emit)call.emit("tick","");return 0;}catch(...){call.exception=std::current_exception();return 1;}}
  static int forward(void* opaque,const char* request,size_t length,PluginEmitV1 emit,void* context) noexcept{
    auto& call=*static_cast<PluginNativeCall*>(opaque);try{
      if(!request||length>16*1024*1024||!emit)throw std::runtime_error("Invalid Plugin next request");
      auto result=call.next(std::string(request,length),[&](const std::string& kind,const std::string& value){emit(context,kind.c_str(),value.data(),value.size());});
      emit(context,"result",result.data(),result.size());return 0;
    }catch(const std::exception& e){auto error=std::string(e.what());if(emit)emit(context,"error",error.data(),error.size());return 1;}
    catch(...){return 1;}
  }
};
static void plugin_load_manifest(const fs::path& path){
  std::error_code ec;if(!path.is_absolute()||!fs::is_regular_file(path,ec)||ec||fs::file_size(path,ec)>65536)throw std::runtime_error("Invalid Plugin manifest path/size");
  auto manifest=plugin_json(slurp(path,65537));if(!manifest||manifest->kind!=Json::Object||plugin_integer(*manifest,"api")!=1)throw std::runtime_error("Plugin manifest API 1 required");
  const auto id=plugin_text(*manifest,"id");if(!plugin_id(id)||id.rfind("builtin",0)==0)throw std::runtime_error("Invalid Plugin id");
  if(plugin_runtime().has_module(id))throw std::runtime_error("Invalid/duplicate Plugin module: "+id);
  const auto root=fs::weakly_canonical(path.parent_path());plugin::Module module;module.id=id;
  if(auto dependencies=manifest->get("requires")){if(dependencies->kind!=Json::Array)throw std::runtime_error("Plugin requires must be an array");for(const auto& dependency:dependencies->items){if(dependency.kind!=Json::String)throw std::runtime_error("Invalid Plugin dependency");module.dependencies.push_back(dependency.str());}}
  std::map<std::string,std::string> declared;
  if(auto services=manifest->get("services")){if(services->kind!=Json::Object||services->fields.size()>64)throw std::runtime_error("Invalid Plugin services");for(const auto& entry:services->fields){if(entry.second.kind!=Json::String)throw std::runtime_error("Plugin service replacement must name an owner (or empty for a new service)");declared.emplace(entry.first,entry.second.str());}}
  std::shared_ptr<PluginLibrary> library;
  if(auto libraries=manifest->get("libraries")){
    if(declared.empty())throw std::runtime_error("Native Plugin must declare at least one service");
#if defined(_WIN32)
    const char* platform="windows";
#elif defined(__APPLE__)
    const char* platform="macos";
#else
    const char* platform="linux";
#endif
    auto file=libraries->get(platform);if(!file||file->kind!=Json::String)throw std::runtime_error("Plugin library missing for this platform");const auto target=root/fs::u8path(file->str());
    if(!within_root(root,target)||!fs::is_regular_file(target,ec)||ec)throw std::runtime_error("Plugin library escapes bundle or is missing");
    library=std::make_shared<PluginLibrary>();PluginInitV1 init=nullptr;
#if defined(_WIN32)
    library->handle=LoadLibraryExW(target.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(library->handle)init=reinterpret_cast<PluginInitV1>(GetProcAddress(library->handle,"codex_plugin_init_v1"));
#else
    library->handle=dlopen(target.c_str(),RTLD_NOW|RTLD_LOCAL);if(!library->handle){auto reason=dlerror();throw std::runtime_error(std::string("Plugin native loader unavailable: ")+(reason?reason:"unknown error"));}
    init=reinterpret_cast<PluginInitV1>(dlsym(library->handle,"codex_plugin_init_v1"));
#endif
    if(!init)throw std::runtime_error("Plugin native API entrypoint missing");
    struct Registration{plugin::Module& module;const std::map<std::string,std::string>& declared;std::shared_ptr<PluginLibrary> library;std::string error;};
    Registration registration{module,declared,library,{}};
    PluginHostV1 host{1,sizeof(PluginHostV1),&registration,[](void* opaque,const char* service,const char* replaces,PluginHandlerV1 handler,void* state)->int{
      auto& r=*static_cast<Registration*>(opaque);try{if(!service||!replaces||!handler)throw std::runtime_error("Invalid Plugin binding");auto it=r.declared.find(service);if(it==r.declared.end()||it->second!=replaces)throw std::runtime_error("Undeclared Plugin binding");
        r.module.contributions.push_back({service,replaces,[library=r.library,handler,state](const std::string& request,const plugin::Emit& emit,const plugin::Next& next){
          PluginNativeCall call{emit,next,{},{},{},0,false};PluginCallV1 invocation{1,sizeof(PluginCallV1),PluginNativeCall::send,&call,PluginNativeCall::forward,&call,PluginNativeCall::poll};
          int code=handler(state,request.data(),request.size(),&invocation);if(call.exception)std::rethrow_exception(call.exception);if(!call.error.empty())throw std::runtime_error(call.error);if(code||!call.has_result)throw std::runtime_error("Plugin handler failed or returned no result");return call.result;
        }});return 0;
      }catch(const std::exception& e){r.error=e.what();return 1;}catch(...){r.error="Plugin registration failed";return 1;}
    }};
    library->module.version=1;library->module.size=sizeof(PluginModuleV1);
    if(init(&host,&library->module)||!registration.error.empty())throw std::runtime_error("Plugin init failed: "+registration.error);
    if(library->module.version!=1||library->module.size<sizeof(PluginModuleV1)||!library->module.id||id!=library->module.id)throw std::runtime_error("Plugin ABI/id mismatch");
  }else if(!declared.empty())throw std::runtime_error("Plugin services require a native library");
  if(module.contributions.size()!=declared.size())throw std::runtime_error("Plugin did not bind every declared service");
  std::vector<PluginToolDefinition> tools;
  if(auto definitions=manifest->get("tools")){if(definitions->kind!=Json::Array||definitions->items.size()>32)throw std::runtime_error("Invalid Plugin tools");std::set<std::string> names={"read_file","list_dir","read_skill","write_file","shell","apply_patch","git_read","git_write","design_review","plugin_ui","plugin_scaffold","plugin_check","plugin_build","plugin_test"};for(const auto& tool:plugin_tools)names.insert(tool.name);
    for(const auto& definition:definitions->items){auto spec=definition.get("spec");if(!spec||spec->kind!=Json::Object)throw std::runtime_error("Plugin tool spec missing");auto name=plugin_text(*spec,"name");if(!plugin_id(name)||!names.insert(name).second||!declared.count("tools."+name)||!spec->get("parameters"))throw std::runtime_error("Invalid/duplicate Plugin tool definition");tools.push_back({"tools."+name,json_dump(*spec),name,plugin_flag(definition,"mutates")});}}
  std::optional<PluginWebModule> web;
  if(auto definition=manifest->get("web")){auto entry=plugin_text(*definition,"entry");auto style=plugin_string(*definition,"style");
    for(const auto& file:{entry,style})if(!file.empty()&&(!within_root(root,root/fs::u8path(file))||!fs::is_regular_file(root/fs::u8path(file),ec)||ec))throw std::runtime_error("Plugin Web asset escapes bundle or is missing");
    if(fs::u8path(entry).extension()!=".js"&&fs::u8path(entry).extension()!=".mjs")throw std::runtime_error("Plugin Web entry must be JavaScript");if(!style.empty()&&fs::u8path(style).extension()!=".css")throw std::runtime_error("Plugin Web style must be CSS");web=PluginWebModule{id,entry,style,root};
  }
  plugin_runtime().mount(std::move(module));
  plugin_tools.insert(plugin_tools.end(),tools.begin(),tools.end());if(web)plugin_web_modules.push_back(*web);
}
static void plugin_load_reviewed(const fs::path& path,const std::string& fingerprint);
static void plugin_boot(){plugin_builtins();for(const auto& path:plugin_manifests){auto review=plugin_reviews.find(path);if(review!=plugin_reviews.end())plugin_load_reviewed(fs::u8path(path),review->second);else plugin_load_reviewed(fs::u8path(path),{});}plugin_runtime().freeze();}
class PluginTool final:public ToolRuntime{
  PluginToolDefinition definition_;
public:explicit PluginTool(PluginToolDefinition definition):definition_(std::move(definition)){}
  std::string name()const override{return definition_.name;}std::string spec_json()const override{return definition_.spec;}bool mutates()const override{return definition_.mutates;}
  ToolResult run(const std::string& args,const fs::path& root)override{
    auto input=plugin_object({{"arguments",plugin_string(args)},{"workspace",plugin_string(root.u8string())}});auto result=plugin_call(definition_.service,input,[](const Json&,const plugin::Emit&)->Json{throw std::runtime_error("Plugin tool has no implementation");});
    return {plugin_integer(result,"exit_code"),plugin_text(result,"output")};
  }
};
static Json plugin_web_catalog(){Json out;out.kind=Json::Array;for(const auto& module:plugin_web_modules)out.items.push_back(plugin_object({{"id",plugin_string(module.id)},{"entry",plugin_string("/plugin/"+module.id+"/entry.js")},{"style",plugin_string(module.style.empty()?"":"/plugin/"+module.id+"/style.css")}}));return out;}
