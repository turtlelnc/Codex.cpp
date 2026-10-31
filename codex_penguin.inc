// Penguin composition root. Infrastructure (OS isolation/authorization) stays in
// the host; product behavior is supplied through named, replaceable services.
#include "penguin/runtime.hpp"
static penguin::Runtime& penguin_runtime(){static penguin::Runtime runtime;return runtime;}
static void penguin_builtins(){static const bool initialized=[](){auto& r=penguin_runtime();
  r.mount({"builtin.model",{},{{"model.request",{},{}},{"model.catalog",{},{}}}});
  r.mount({"builtin.agent",{"builtin.model"},{{"agent.turn",{},{}}}});
  r.mount({"builtin.prompt",{},{{"prompt.build",{},{}}}});
  r.mount({"builtin.journal",{},{{"journal.append",{},{}},{"journal.replace",{},{}}}});
  r.mount({"builtin.tools",{},{{"tool.execute",{},{}}}});
  r.mount({"builtin.web",{},{}});
  return true;}();(void)initialized;
}
static std::string penguin_inspect(){penguin_builtins();std::string out="[";for(const auto& service:penguin_runtime().inspect()){
  if(out.size()>1)out+=',';out+="{\"service\":\""+json_escape(service.name)+"\",\"owner\":\""+json_escape(service.owner)+"\",\"layers\":[";
  for(const auto& layer:service.layers){if(out.back()!='[')out+=',';out+='"'+json_escape(layer)+'"';}out+="]}";
}return out+"]";}
static Json penguin_call(const std::string& service,const Json& input,const std::function<Json(const Json&,const penguin::Emit&)>& builtin,const penguin::Emit& emit={}){
  penguin_builtins();auto result=penguin_runtime().call(service,json_dump(input),emit,[&](const std::string& request,const penguin::Emit& sink){
    auto parsed=parse_json(request);if(!parsed)throw std::runtime_error("Invalid Penguin request JSON: "+service);return json_dump(builtin(*parsed,sink));
  });auto parsed=parse_json(result);if(!parsed)throw std::runtime_error("Invalid Penguin result JSON: "+service);return *parsed;
}
static Json penguin_string(const std::string& value){Json out;out.kind=Json::String;out.value=value;return out;}
static Json penguin_bool(bool value){Json out;out.kind=Json::Other;out.value=value?"true":"false";return out;}
static Json penguin_number(int value){Json out;out.kind=Json::Other;out.value=std::to_string(value);return out;}
static Json penguin_object(std::initializer_list<std::pair<const std::string,Json>> fields){Json out;out.kind=Json::Object;out.fields=fields;return out;}
static std::string penguin_text(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::String)throw std::runtime_error(std::string("Penguin string required: ")+key);return value->str();}
static bool penguin_flag(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::Other||(value->value!="true"&&value->value!="false"))throw std::runtime_error(std::string("Penguin boolean required: ")+key);return value->value=="true";}
static int penguin_integer(const Json& object,const char* key){auto value=object.get(key);if(!value||value->kind!=Json::Other||value->value.find_first_not_of("-0123456789")!=std::string::npos)throw std::runtime_error(std::string("Penguin integer required: ")+key);size_t used=0;int n=std::stoi(value->value,&used);if(used!=value->value.size())throw std::runtime_error("Invalid Penguin integer");return n;}
static std::vector<std::string> penguin_manifests;
static void penguin_arguments(int& argc,char**& argv,std::vector<char*>& storage){
  int i=1;while(i<argc && std::string(argv[i])=="--penguin"){
    if(i+1>=argc)throw std::runtime_error("--penguin requires an absolute manifest path");auto path=fs::u8path(argv[i+1]);
    if(!path.is_absolute())throw std::runtime_error("--penguin requires an absolute manifest path");
    const auto name=path.u8string();if(std::find(penguin_manifests.begin(),penguin_manifests.end(),name)==penguin_manifests.end())penguin_manifests.push_back(name);i+=2;
  }
  if(i==1)return;storage.clear();storage.push_back(argv[0]);for(;i<argc;++i)storage.push_back(argv[i]);argc=static_cast<int>(storage.size());storage.push_back(nullptr);argv=storage.data();
}
