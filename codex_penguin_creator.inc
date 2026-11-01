// Creator tools never dlopen generated code in the Agent/server process.
static fs::path penguin_creator_self;
static fs::path creator_path(const fs::path& root,const std::string& input){
  if(input.empty())throw std::runtime_error("Plugin path is required");auto path=fs::absolute(root/fs::u8path(input)).lexically_normal();
  if(!within_root(root,path)||sensitive_path(path))throw std::runtime_error("Plugin path escapes workspace or is protected");return path;
}
static Json creator_array(const std::vector<std::string>& values){Json out;out.kind=Json::Array;for(const auto& value:values)out.items.push_back(penguin_string(value));return out;}
static bool creator_id(const std::string& id){return plugin_id(id)&&id.rfind("builtin",0)!=0&&((id.front()>='a'&&id.front()<='z')||(id.front()>='0'&&id.front()<='9'));}
static bool creator_service_name(const std::string& name){return !name.empty()&&name.size()<=128&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-")==std::string::npos;}
static std::string creator_platform(){
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}
static std::string creator_command(const std::vector<std::string>& args){std::string out;
#if defined(_WIN32)
  out="&";for(const auto& arg:args){out+=" '";for(char c:arg){out+=c;if(c=='\'')out+='\'';}out+='\'';}
#else
  for(const auto& arg:args){if(!out.empty())out+=' ';out+=shell_quote(arg);}
#endif
  return out;
}
static Json creator_check(const fs::path& root,const std::string& input){
  auto manifest=creator_path(root,input);std::error_code ec;if(fs::is_directory(manifest,ec))manifest/="penguin.json";
  if(!within_root(root,manifest)||!fs::is_regular_file(manifest,ec)||ec||fs::file_size(manifest,ec)>65536)throw std::runtime_error("Plugin manifest missing or too large");
  auto json=plugin_json(native_web::read_bytes(manifest,65536));if(!json||json->kind!=Json::Object||penguin_integer(*json,"api")!=1)throw std::runtime_error("Penguin manifest API 1 required");
  const auto id=penguin_text(*json,"id");if(!creator_id(id))throw std::runtime_error("Invalid Penguin id");
  const auto bundle=fs::weakly_canonical(manifest.parent_path());
  auto file=[&](const std::string& name,bool required,size_t limit)->fs::path{
    if(name.empty())throw std::runtime_error("Empty plugin asset path");auto path=bundle/fs::u8path(name);
    if(!within_root(bundle,path)||!within_root(root,path)||sensitive_path(path))throw std::runtime_error("Plugin asset escapes bundle or is protected");
    if(!fs::exists(path,ec)){if(required)throw std::runtime_error("Missing plugin asset: "+name);return path;}
    if(ec||!fs::is_regular_file(path,ec)||ec||fs::hard_link_count(path,ec)!=1||ec||fs::file_size(path,ec)>limit||ec)throw std::runtime_error("Invalid/oversized plugin asset: "+name);return path;
  };
  std::vector<std::string> dependencies,services,files={manifest.filename().u8string()};
  if(auto values=json->get("requires")){if(values->kind!=Json::Array)throw std::runtime_error("Invalid plugin dependencies");for(const auto& value:values->items){if(value.kind!=Json::String||!creator_service_name(value.str()))throw std::runtime_error("Invalid dependency id");dependencies.push_back(value.str());}}
  auto declarations=json->get("services");if(declarations){if(declarations->kind!=Json::Object||declarations->fields.size()>64)throw std::runtime_error("Invalid plugin services");for(const auto& entry:declarations->fields){if(!creator_service_name(entry.first)||entry.second.kind!=Json::String||(!entry.second.str().empty()&&!creator_service_name(entry.second.str())))throw std::runtime_error("Invalid plugin service declaration");services.push_back(entry.first);}}
  bool native=false,built=false;std::string library;
  if(auto libraries=json->get("libraries")){native=true;if(services.empty())throw std::runtime_error("Native plugin must declare services");library=penguin_text(*libraries,creator_platform().c_str());auto path=file(library,false,32*1024*1024);built=fs::is_regular_file(path,ec);if(built)files.push_back(library);}
  else if(!services.empty())throw std::runtime_error("Native service declarations require a library");
  if(auto tools=json->get("tools")){if(tools->kind!=Json::Array||tools->items.size()>32)throw std::runtime_error("Invalid plugin tools");std::set<std::string> names={"read_file","list_dir","read_skill","write_file","shell","apply_patch","git_read","git_write","design_review","plugin_ui","penguin_scaffold","penguin_check","penguin_build","penguin_test"};for(const auto& tool:tools->items){auto spec=tool.get("spec");if(!spec||spec->kind!=Json::Object)throw std::runtime_error("Plugin tool spec missing");auto name=penguin_text(*spec,"name");auto parameters=spec->get("parameters");if(!plugin_id(name)||!names.insert(name).second||!declarations||!declarations->get("tools."+name)||!parameters||parameters->kind!=Json::Object)throw std::runtime_error("Invalid/duplicate plugin tool");(void)penguin_flag(tool,"mutates");}}
  if(auto web=json->get("web")){auto entry=penguin_text(*web,"entry");file(entry,true,4*1024*1024);if(fs::u8path(entry).extension()!=".js"&&fs::u8path(entry).extension()!=".mjs")throw std::runtime_error("Web entry must be JavaScript");files.push_back(entry);if(auto style=web->get("style")){if(style->kind!=Json::String||fs::u8path(style->str()).extension()!=".css")throw std::runtime_error("Web style must be CSS");file(style->str(),true,4*1024*1024);files.push_back(style->str());}}
  if(!native&&!json->get("web"))throw std::runtime_error("Creator plugin has no native or WebUI contribution");
  for(const auto& name:{"plugin.cpp","native.h","CMakeLists.txt","native.def","README.md"})if(fs::exists(bundle/name,ec)){file(name,true,4*1024*1024);files.push_back(name);}
  Json hashes;hashes.kind=Json::Object;for(const auto& name:files){auto path=file(name,true,32*1024*1024);hashes.fields[name]=penguin_string(native_web::digest(native_web::read_bytes(path,32*1024*1024)));}
  std::vector<std::string> launch={penguin_creator_self.u8string(),"--penguin",fs::weakly_canonical(manifest).u8string(),"web","--root",fs::weakly_canonical(root).u8string()};
  auto report=penguin_object({{"creator",penguin_bool(true)},{"id",penguin_string(id)},{"kind",penguin_string(native?"native":"web")},{"status",penguin_string(native&&!built?"needs_build":"static_checked")},{"built",penguin_bool(built)},{"enabled",penguin_bool(penguin_runtime().has_module(id))},{"manifest",penguin_string(fs::weakly_canonical(manifest).u8string())},{"manifest_relative",penguin_string(manifest.lexically_relative(fs::absolute(root)).generic_u8string())},{"files",creator_array(files)},{"sha256",hashes},{"services",creator_array(services)},{"requires",creator_array(dependencies)},{"native_executed",penguin_bool(false)},{"functional_tested",penguin_bool(false)},{"requires_restart",penguin_bool(true)},{"launch_args",creator_array(launch)},{"launch_command",penguin_string(creator_command(launch))},{"notes",penguin_string("This check did not activate or change any plugin. Command is a local WebUI example. For hosted services the administrator must add --penguin to the original startup configuration and restart. No activation has occurred.")}});
  report.fields["library"]=penguin_string(library);return report;
}
static Json creator_scaffold(const fs::path& root,const std::string& path,const std::string& id,const std::string& kind){
  if(!creator_id(id))throw std::runtime_error("Invalid Penguin id");if(kind!="native"&&kind!="web")throw std::runtime_error("Plugin kind must be native or web");
  auto target=creator_path(root,path);std::error_code ec;if(fs::exists(target,ec))throw std::runtime_error("Plugin target already exists; existing files will not be overwritten");if(auto risk=external_hardlink_risk(root))throw std::runtime_error("DENIED: "+*risk);
  const auto tool="pg_"+id.substr(0,20)+"_"+native_web::digest(id).substr(0,8);
  auto replace=[&](std::string text){for(const auto& entry:std::map<std::string,std::string>{{"@PENGUIN_ID@",id},{"@PENGUIN_TOOL@",tool}}){size_t pos=0;while((pos=text.find(entry.first,pos))!=std::string::npos){text.replace(pos,entry.first.size(),entry.second);pos+=entry.second.size();}}return text;};
  std::map<std::string,std::string> output;output["client.js"]=replace(std::string(codex_resource("penguin-creator/client.js")));
  Json manifest=penguin_object({{"api",penguin_number(1)},{"id",penguin_string(id)},{"web",penguin_object({{"entry",penguin_string("client.js")}})}});
  if(kind=="native"){
    output["native.h"]=std::string(codex_resource("penguin-creator/native.h"));output["plugin.cpp"]=replace(std::string(codex_resource("penguin-creator/plugin.cpp")));output["CMakeLists.txt"]=replace(std::string(codex_resource("penguin-creator/CMakeLists.txt")));output["native.def"]="EXPORTS\n  codex_penguin_init_v1\n";
    manifest.fields["libraries"]=penguin_object({{"linux",penguin_string("build/"+id+".so")},{"macos",penguin_string("build/"+id+".dylib")},{"windows",penguin_string("build/"+id+".dll")}});
    manifest.fields["services"]=penguin_object({{"tools."+tool,penguin_string("")}});Json tools;tools.kind=Json::Array;tools.items.push_back(penguin_object({{"mutates",penguin_bool(false)},{"spec",penguin_object({{"type",penguin_string("function")},{"name",penguin_string(tool)},{"description",penguin_string("Starter status tool; implement the requested feature.")},{"parameters",*parse_json("{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}")},{"strict",penguin_bool(true)}})}}));manifest.fields["tools"]=tools;
  }
  output["penguin.json"]=json_dump(manifest)+"\n";output["README.md"]="# "+id+"\n\nGenerated starter, not a completed feature. Implement and test the requested behavior.\n\nNative: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\ncmake --build build --config Release\n\nCheck: codex-cpp penguin check .\nProbe native ABI in isolation: codex-cpp penguin test .\nActivation is manual after review and requires restarting the service.\n";
  for(const auto& entry:output)if(entry.second.empty())throw std::runtime_error("Embedded Creator SDK resource missing: "+entry.first);
  fs::create_directories(target.parent_path(),ec);if(ec)throw std::runtime_error(ec.message());auto stage=target.parent_path()/random_id(".penguin-create");if(!fs::create_directory(stage,ec)||ec)throw std::runtime_error("Cannot stage plugin");
  try{std::string error;for(const auto& entry:output)if(!write_all(stage/entry.first,entry.second,error))throw std::runtime_error(error);if(!within_root(root,target)||fs::exists(target,ec))throw std::runtime_error("Plugin destination changed");fs::rename(stage,target,ec);if(ec)throw std::runtime_error(ec.message());}catch(...){fs::remove_all(stage,ec);throw;}
  return creator_check(root,path);
}
static CommandResult creator_process(const std::vector<std::string>& command,const ProcessPolicy& policy,int seconds,const std::function<void()>& tick,bool watch){
  if(execution.unsafe)return {126,"Creator verification requires OS isolation; --unsafe-execution is not supported."};
  ManagedProcess process;std::string error;if(!process.start(command,policy,error,true))return {126,error};process.close_input();CommandResult result;char bytes[4096];const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);auto tick_at=std::chrono::steady_clock::now();TerminalInputScope input(watch);
  for(;;){if(watch&&busy_input&&busy_input("",0))return {130,"Interrupted by Esc"};
#if !defined(_WIN32)
    if(tui_interrupt_requested)return {130,"Interrupted by Esc"};
#endif
    const int n=process.receive(bytes,sizeof bytes,100);if(n<0)break;if(n>0){result.output.append(bytes,static_cast<size_t>(n));if(result.output.size()>200000)return {2,"Creator process output exceeded 200 KB"};}
    const auto now=std::chrono::steady_clock::now();if(now>=deadline)return {124,"Creator process timed out"};if(tick&&now-tick_at>=std::chrono::seconds(1)){tick();tick_at=now;}
  }
  result.exit_code=process.wait(1000);if(result.exit_code<0)result.exit_code=124;return result;
}
static ToolResult creator_build(const fs::path& root,const std::string& path,const std::function<void()>& tick={},bool watch=false){try{
  auto report=creator_check(root,path);if(penguin_text(report,"kind")!="native")return {2,"Web-only plugins have no native build step"};if(penguin_flag(report,"enabled"))return {2,"Native plugin id is already loaded; stop the active instance before rebuilding it"};auto source=fs::u8path(penguin_text(report,"manifest")).parent_path();if(!fs::is_regular_file(source/"CMakeLists.txt"))return {2,"CMakeLists.txt missing"};
  auto build=source/"build";if(!within_root(source,build)||!within_root(root,build))return {2,"Build directory escapes plugin"};auto policy=ProcessPolicy::tool(root,true);policy.reads.push_back("/usr/include");policy.reads.push_back("/usr/local/include");policy.reads.push_back("/usr/libexec");policy.reads.push_back("/usr/local/libexec");
#if defined(__APPLE__)
  for(const auto& path:{"/usr/local/Cellar","/usr/local/share","/usr/local/opt","/Applications/CMake.app/Contents"})policy.reads.push_back(path);
#endif
  std::vector<std::string> configure={"cmake","-S",source.u8string(),"-B",build.u8string(),"-DCMAKE_BUILD_TYPE=Release"};
#if defined(__APPLE__) && defined(__aarch64__)
  configure.push_back("-DCMAKE_OSX_ARCHITECTURES=arm64");
#elif defined(__APPLE__)
  configure.push_back("-DCMAKE_OSX_ARCHITECTURES=x86_64");
#endif
  auto configured=creator_process(configure,policy,120,tick,watch);if(configured.exit_code)return {configured.exit_code,configured.output};auto compiled=creator_process({"cmake","--build",build.u8string(),"--config","Release","--parallel","2"},policy,120,tick,watch);if(compiled.exit_code)return {compiled.exit_code,compiled.output};report=creator_check(root,path);if(!penguin_flag(report,"built"))return {2,"Build completed but manifest library was not produced"};report.fields["status"]=penguin_string("build_passed");report.fields["build_log"]=penguin_string(configured.output+compiled.output);return {0,json_dump(report)};
}catch(const std::exception& e){return {2,e.what()};}}
static ToolResult creator_test(const fs::path& root,const std::string& path,const std::function<void()>& tick={},bool watch=false){try{
  auto report=creator_check(root,path);if(penguin_text(report,"kind")!="native")return {2,"Web-only behavior requires browser verification; no native ABI to probe"};if(!penguin_flag(report,"built"))return {2,"Build the native plugin first"};
  const auto manifest=fs::u8path(penguin_text(report,"manifest"));auto bytes=native_web::read_bytes(manifest,65536);auto staged=plugin_json(bytes);if(!staged)throw std::runtime_error("Invalid probe manifest");auto fingerprints=report.get("sha256");
  if(!fingerprints||plugin_string(*fingerprints,manifest.filename().u8string())!=native_web::digest(bytes))throw std::runtime_error("Plugin changed before probe");
  std::error_code ec;auto directory=fs::temp_directory_path()/random_id("penguin-probe");if(!fs::create_directory(directory,ec)||ec)throw std::runtime_error("Cannot stage isolated probe");
  struct Cleanup{fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);}} cleanup{directory};fs::permissions(directory,fs::perms::owner_all,fs::perm_options::replace);
  const auto bundle=manifest.parent_path();auto copy=[&](const std::string& from,const std::string& to){auto data=native_web::read_bytes(bundle/fs::u8path(from),32*1024*1024);if(plugin_string(*fingerprints,from)!=native_web::digest(data))throw std::runtime_error("Plugin changed before probe");std::string error;if(!write_all(directory/to,data,error))throw std::runtime_error(error);};
  const auto library=penguin_text(report,"library");const auto filename="plugin"+fs::u8path(library).extension().u8string();copy(library,filename);staged->fields["libraries"]=penguin_object({{creator_platform(),penguin_string(filename)}});
  if(auto web=staged->get("web")){auto entry=penguin_text(*web,"entry");copy(entry,"entry.js");auto replacement=penguin_object({{"entry",penguin_string("entry.js")}});if(auto style=web->get("style")){copy(style->str(),"style.css");replacement.fields["style"]=penguin_string("style.css");}staged->fields["web"]=replacement;}
  std::string error;if(!write_all(directory/"penguin.json",json_dump(*staged),error))throw std::runtime_error(error);
  ProcessPolicy policy;policy.role=ProcessRole::Helper;policy.workspace=directory;auto tested=creator_process({penguin_creator_self.u8string(),"--penguin",(directory/"penguin.json").u8string(),"penguin","inspect"},policy,10,tick,watch);if(tested.exit_code)return {tested.exit_code,tested.output};auto graph=parse_json(trim(tested.output));if(!graph||graph->kind!=Json::Array)return {2,"Native probe did not return a valid service graph"};bool found=false;for(const auto& entry:graph->items)if(plugin_string(entry,"owner")==penguin_text(report,"id"))found=true;if(!found)return {2,"Native probe registered no plugin services"};
  if(json_dump(creator_check(root,path).fields.at("sha256"))!=json_dump(*fingerprints))throw std::runtime_error("Plugin changed during probe; rerun verification");
  report.fields["status"]=penguin_string("native_probe_passed");report.fields["native_executed"]=penguin_bool(true);report.fields["probe_scope"]=penguin_string("read-only isolated subprocess; ABI registration only");report.fields["notes"]=penguin_string("ABI registration passed in an isolated process. Tool/UI behavior remains unverified; plugin is not enabled in this service.");return {0,json_dump(report)};
}catch(const std::exception& e){return {2,e.what()};}}
class PenguinScaffoldTool final:public ToolRuntime{public:std::string name()const override{return "penguin_scaffold";}bool mutates()const override{return true;}std::string spec_json()const override{return R"({"type":"function","name":"penguin_scaffold","description":"Create a new Penguin plugin starter with embedded SDK. Read penguin-creator skill first. Never overwrites or enables a plugin.","parameters":{"type":"object","properties":{"path":{"type":"string"},"id":{"type":"string"},"kind":{"type":"string","enum":["native","web"]}},"required":["path","id","kind"],"additionalProperties":false},"strict":true})";}ToolResult run(const std::string& args,const fs::path& root)override{try{auto request=parse_json(args);if(!request)throw std::runtime_error("Invalid arguments");return {0,json_dump(creator_scaffold(root,penguin_text(*request,"path"),penguin_text(*request,"id"),penguin_text(*request,"kind")))};}catch(const std::exception& e){return {2,e.what()};}}};
class PenguinCheckTool final:public ToolRuntime{public:std::string name()const override{return "penguin_check";}std::string spec_json()const override{return one_arg_tool_spec("penguin_check","Statically review a workspace Penguin manifest/files and prepare manual activation instructions. Does not execute native/JavaScript code or enable anything.","path");}ToolResult run(const std::string& args,const fs::path& root)override{try{auto path=json_string_field(args,"path");if(!path)throw std::runtime_error("Missing path");return {0,json_dump(creator_check(root,*path))};}catch(const std::exception& e){return {2,e.what()};}}};
class PenguinBuildTool final:public ToolRuntime{public:std::string name()const override{return "penguin_build";}bool mutates()const override{return true;}std::string spec_json()const override{return one_arg_tool_spec("penguin_build","Configure/build a workspace native Penguin with CMake inside the OS sandbox. Requires CMake/compiler; never installs or enables the result.","path");}ToolResult run(const std::string& args,const fs::path& root)override{return run_with_ui(args,root,{},false);}ToolResult run_with_ui(const std::string& args,const fs::path& root,const std::function<void()>& tick,bool watch)override{auto path=json_string_field(args,"path");return path?creator_build(root,*path,tick,watch):ToolResult{2,"Missing path"};}};
class PenguinTestTool final:public ToolRuntime{public:std::string name()const override{return "penguin_test";}bool mutates()const override{return true;}bool is_shell()const override{return true;}std::string spec_json()const override{return one_arg_tool_spec("penguin_test","Probe native Penguin ABI/registration from a staged bundle in a separate read-only OS sandbox without network or inherited credentials. Does not prove feature behavior and does not enable the plugin. Fails closed if isolation is unavailable.","path");}ToolResult run(const std::string& args,const fs::path& root)override{return run_with_ui(args,root,{},false);}ToolResult run_with_ui(const std::string& args,const fs::path& root,const std::function<void()>& tick,bool watch)override{auto path=json_string_field(args,"path");return path?creator_test(root,*path,tick,watch):ToolResult{2,"Missing path"};}};
static int penguin_creator_cli(int argc,char** argv){try{const auto root=fs::current_path();Json result;ToolResult tested;
  if(argc==4&&std::string(argv[2])=="check")result=creator_check(root,argv[3]);
  else if((argc==5||argc==6)&&std::string(argv[2])=="scaffold")result=creator_scaffold(root,argv[3],argv[4],argc==6?argv[5]:"native");
  else if(argc==4&&(std::string(argv[2])=="build"||std::string(argv[2])=="test")){tested=std::string(argv[2])=="build"?creator_build(root,argv[3]):creator_test(root,argv[3]);std::cout<<tested.output<<'\n';return tested.exit_code;}
  else throw std::runtime_error("Usage: codex-cpp penguin scaffold PATH ID [native|web] | check PATH | build PATH | test PATH (paths inside current workspace)");
  std::cout<<json_dump(result)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"Creator: "<<e.what()<<'\n';return 2;}}
