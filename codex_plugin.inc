// Plugin composition root. Infrastructure (OS isolation/authorization) stays in
// the host; product behavior is supplied through named, replaceable services.
#include "plugin/runtime.hpp"
static plugin::Runtime& plugin_runtime(){static plugin::Runtime runtime;return runtime;}
static void plugin_builtins(){static const bool initialized=[](){auto& r=plugin_runtime();
  r.mount({"builtin.model",{},{{"model.request",{},{}},{"model.catalog",{},{}}}});
  r.mount({"builtin.agent",{"builtin.model"},{{"agent.turn",{},{}}}});
  r.mount({"builtin.prompt",{},{{"prompt.build",{},{}}}});
  r.mount({"builtin.journal",{},{{"journal.append",{},{}},{"journal.replace",{},{}}}});
  r.mount({"builtin.tools",{},{{"tool.execute",{},{}}}});
  r.mount({"builtin.web",{},{}});
  return true;}();(void)initialized;
}
static std::string plugin_inspect(){plugin_builtins();std::string out="[";for(const auto& service:plugin_runtime().inspect()){
  if(out.size()>1)out+=',';out+="{\"service\":\""+json_escape(service.name)+"\",\"owner\":\""+json_escape(service.owner)+"\",\"layers\":[";
  for(const auto& layer:service.layers){if(out.back()!='[')out+=',';out+='"'+json_escape(layer)+'"';}out+="]}";
}return out+"]";}
static Json plugin_call(const std::string& service,const Json& input,const std::function<Json(const Json&,const plugin::Emit&)>& builtin,const plugin::Emit& emit={}){
  plugin_builtins();auto result=plugin_runtime().call(service,json_dump(input),emit,[&](const std::string& request,const plugin::Emit& sink){
    auto parsed=parse_json(request);if(!parsed)throw std::runtime_error("Invalid Plugin request JSON: "+service);return json_dump(builtin(*parsed,sink));
  });auto parsed=parse_json(result);if(!parsed)throw std::runtime_error("Invalid Plugin result JSON: "+service);return *parsed;
}
static Json plugin_string(const std::string& value){Json out;out.kind=Json::String;out.value=value;return out;}
static Json plugin_bool(bool value){Json out;out.kind=Json::Other;out.value=value?"true":"false";return out;}
static Json plugin_number(int value){Json out;out.kind=Json::Other;out.value=std::to_string(value);return out;}
static Json plugin_object(std::initializer_list<std::pair<const std::string,Json>> fields){Json out;out.kind=Json::Object;out.fields=fields;return out;}
static std::string plugin_text(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::String)throw std::runtime_error(std::string("Plugin string required: ")+key);return value->str();}
static bool plugin_flag(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::Other||(value->value!="true"&&value->value!="false"))throw std::runtime_error(std::string("Plugin boolean required: ")+key);return value->value=="true";}
static int plugin_integer(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::Other||value->value.find_first_not_of("-0123456789")!=std::string::npos)throw std::runtime_error(std::string("Plugin integer required: ")+key);size_t used=0;int n=std::stoi(value->value,&used);if(used!=value->value.size())throw std::runtime_error("Invalid Plugin integer");return n;}
static std::vector<std::string> plugin_manifests;
static std::map<std::string,std::string> plugin_reviews;
// Reviewed WebUI workers inherit the parent's immutable bundle, never the
// original writable workspace path. Legacy --plugin retains manual trust.
static std::map<std::string,std::string> plugin_worker_manifests;
static void plugin_arguments(int& argc,char**& argv,std::vector<char*>& storage){
  int i=1;while(i<argc && (std::string(argv[i])=="--plugin"||std::string(argv[i])=="--plugin-reviewed")){
    const bool reviewed=std::string(argv[i])=="--plugin-reviewed";
    if(i+1>=argc)throw std::runtime_error("--plugin requires an absolute manifest path");auto path=fs::u8path(argv[i+1]);
    if(!path.is_absolute())throw std::runtime_error("--plugin requires an absolute manifest path");
    const auto name=path.u8string();
    if(reviewed){if(i+2>=argc)throw std::runtime_error("--plugin-reviewed requires a review SHA-256");const std::string hash=argv[i+2];if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid Plugin review SHA-256");if(plugin_reviews.count(name)&&plugin_reviews.at(name)!=hash)throw std::runtime_error("Conflicting Plugin review fingerprints");plugin_reviews[name]=hash;}
    if(std::find(plugin_manifests.begin(),plugin_manifests.end(),name)==plugin_manifests.end())plugin_manifests.push_back(name);i+=reviewed?3:2;
  }
  if(i==1)return;storage.clear();storage.push_back(argv[0]);for(;i<argc;++i)storage.push_back(argv[i]);argc=static_cast<int>(storage.size());storage.push_back(nullptr);argv=storage.data();
}
