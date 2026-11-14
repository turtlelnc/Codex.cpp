#include "miniweb/worker.h"
static fs::path preview_self;
static std::pair<std::string,std::string> preview_metadata(const fs::path& path){
  std::error_code ec;
  if(!fs::is_regular_file(fs::symlink_status(path,ec))||ec||fs::hard_link_count(path)!=1||fs::file_size(path)>256*1024)
    throw std::runtime_error("Invalid preview capability artifact");
  std::ifstream input(path,std::ios::binary);
  const std::string bytes(std::istreambuf_iterator<char>(input),{});
  auto doc=parse_json(bytes);
  const auto version=doc?doc->get("version"):nullptr,summary=doc?doc->get("summary"):nullptr,report=doc?doc->get("report"):nullptr;
  if(!input||!doc||doc->kind!=Json::Object||!version||version->kind!=Json::Other||version->value!="1"||
     !summary||summary->kind!=Json::String||summary->value.rfind("Capability report: ",0)!=0||summary->value.size()>8192||
     !report||report->kind!=Json::Object)throw std::runtime_error("Invalid preview capability metadata");
  for(const auto* key:{"ignored_css","approximate","structural","missing_js"}){
    const auto list=report->get(key);
    if(!list||list->kind!=Json::Array)throw std::runtime_error("Incomplete preview capability report");
  }
  return {summary->value,json_dump(*report)};
}
static ToolResult preview_capture(const std::string& args,const fs::path& root,const std::function<void()>& tick={},bool watch=false,std::string* png=nullptr,std::string* report=nullptr){try{
  auto request=parse_json(args);if(!request||request->kind!=Json::Object)return {2,"Invalid preview arguments"};
  auto path=json_string_field(args,"path");if(!path||path->empty())return {2,"Preview requires a workspace HTML path"};
  auto page=fs::weakly_canonical(root/fs::u8path(*path));if(!within_root(root,page)||sensitive_path(page)||!fs::is_regular_file(page))return {126,"Preview path outside workspace, protected or missing"};
  auto bounded=[&](const char* key,int fallback,int low,int high){auto v=request->get(key);if(!v)return fallback;if(v->kind!=Json::Other)throw std::runtime_error("Preview dimensions/wait must be integers");size_t used=0;int n=std::stoi(v->value,&used);if(used!=v->value.size()||n<low||n>high)throw std::runtime_error("Preview value outside limits");return n;};
  const int width=bounded("width",1280,64,1920),height=bounded("height",800,64,1920),wait=bounded("wait_ms",500,0,3000);
  if(execution.unsafe||!sandbox_available())return {126,"Preview requires an OS sandbox; no unsafe fallback. Windows restricted execution is not yet supported."};
  if(execution.private_dir.empty())execution.initialize(root);
  const auto scratch=execution.private_dir/random_id("preview");fs::create_directory(scratch);fs::permissions(scratch,fs::perms::owner_all,fs::perm_options::replace);
  struct Cleanup{fs::path p;~Cleanup(){std::error_code ec;fs::remove_all(p,ec);}}cleanup{scratch};
  const auto output=scratch/"screenshot.png";std::vector<std::string> command={preview_self.u8string(),"--miniweb-worker",fs::canonical(root).u8string(),page.u8string(),output.u8string(),std::to_string(width),std::to_string(height),std::to_string(wait)};
  if(auto clicks=request->get("clicks")){if(clicks->kind!=Json::Array||clicks->items.size()>16)return {2,"Preview supports at most 16 replayed clicks"};for(const auto& click:clicks->items){auto x=click.get("x"),y=click.get("y");if(!x||!y||x->kind!=Json::Other||y->kind!=Json::Other)return {2,"Click requires integer x/y"};size_t nx=0,ny=0;const int px=std::stoi(x->value,&nx),py=std::stoi(y->value,&ny);if(nx!=x->value.size()||ny!=y->value.size()||px<0||py<0||px>=width||py>=height)return {2,"Click outside viewport"};command.push_back(std::to_string(px));command.push_back(std::to_string(py));}}
  auto policy=ProcessPolicy::tool(root,false);policy.writes.push_back(scratch);policy.denies.push_back(own_auth_path().parent_path());const auto home=fs::path(getenv_or("HOME",getenv_or("USERPROFILE")));for(const auto& secret:{home/".ssh",home/".aws",home/".codex",home/"Library/Keychains"})policy.denies.push_back(secret);policy.reads.push_back("/Library/Fonts");policy.reads.push_back("/System/Library/Fonts");
  auto result=creator_process(command,policy,25,tick,watch);if(result.exit_code)return {result.exit_code,"Preview failed: "+result.output};
  std::error_code ec;if(!fs::is_regular_file(fs::symlink_status(output,ec))||ec||fs::hard_link_count(output)!=1)return {2,"Invalid preview artifact"};
  std::string why;auto image=image_data_url(output,why);if(!image)return {2,"Preview: "+why};
  if(png){std::ifstream input(output,std::ios::binary);*png=std::string(std::istreambuf_iterator<char>(input),{});}
  const auto [capability,capability_json]=preview_metadata(fs::u8path(output.u8string()+miniweb_report_suffix));
  if(report)*report=capability_json;
  const std::string diagnostics=result.output.find("[miniweb] error:")!=std::string::npos?"Renderer reported script/errors; this image may be incomplete. ":result.output.find("[miniweb] warning:")!=std::string::npos?"Renderer reported missing/unsupported resources; this image may be incomplete. ":"";
  return {0,(capability.empty()?std::string{}:capability+" ")+diagnostics+"Experimental MiniWeb screenshot; local files only, no network. Each call reloads the page and replays clicks. Limited CSS/DOM support: this is not Chrome or full-browser verification.",{*image}};
}catch(const std::exception& e){return {2,std::string("Preview: ")+e.what()};}}
class PreviewTool final:public ToolRuntime{
public:
  std::string name()const override{return "preview_screenshot";}
  std::string spec_json()const override{return R"({"type":"function","name":"preview_screenshot","description":"Render a local workspace HTML file with the experimental embedded MiniWeb engine. Returns a PNG image plus a capability report naming unsupported or approximate CSS/JS features detected in the page; treat layout near those features as unreliable. No network, no external browser, limited CSS/DOM compatibility. Each call reloads the page and replays clicks; never claim Chrome compatibility or keyboard/scroll verification. Requires OS sandbox (currently Linux/macOS).","parameters":{"type":"object","properties":{"path":{"type":"string"},"width":{"type":"integer","minimum":64,"maximum":1920},"height":{"type":"integer","minimum":64,"maximum":1920},"wait_ms":{"type":"integer","minimum":0,"maximum":3000},"clicks":{"type":"array","maxItems":16,"items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"}},"required":["x","y"],"additionalProperties":false}}},"required":["path","width","height","wait_ms","clicks"],"additionalProperties":false},"strict":true})";}
  ToolResult run(const std::string& args,const fs::path& root)override{return preview_capture(args,root);}
  ToolResult run_with_ui(const std::string& args,const fs::path& root,const std::function<void()>& tick,bool watch)override{return preview_capture(args,root,tick,watch);}
};
static int preview_cli(int argc,char** argv){try{
  if(argc<4||std::string(argv[2])!="shot"){std::cerr<<"Usage: codex-cpp preview shot FILE [--width N] [--height N] [--wait N] [--click X,Y] [-o PNG] [--report JSON]\n";return 2;}
  Json request=plugin_object({{"path",plugin_string(argv[3])}});Json clicks;clicks.kind=Json::Array;std::string output="miniweb.png",report_path;
  for(int i=4;i<argc;++i){std::string flag=argv[i];if(i+1>=argc)throw std::runtime_error("Missing preview option value");std::string value=argv[++i];if(flag=="-o"||flag=="--out")output=value;else if(flag=="--report")report_path=value;else if(flag=="--width"||flag=="--height"||flag=="--wait"){auto n=parse_json(value);if(!n||n->kind!=Json::Other)throw std::runtime_error("Expected integer");request.fields[flag=="--wait"?"wait_ms":flag.substr(2)]=*n;}else if(flag=="--click"){const auto comma=value.find(',');if(comma==std::string::npos)throw std::runtime_error("Click requires X,Y");auto x=parse_json(value.substr(0,comma)),y=parse_json(value.substr(comma+1));if(!x||!y)throw std::runtime_error("Invalid click");clicks.items.push_back(plugin_object({{"x",*x},{"y",*y}}));}else throw std::runtime_error("Unsupported preview option");}
  request.fields["clicks"]=clicks;const auto root=fs::current_path();const auto target=root/fs::u8path(output);if(!within_root(root,target)||sensitive_path(target))throw std::runtime_error("Preview output must be inside workspace and unprotected");if(fs::exists(target)&&fs::hard_link_count(target)>1)throw std::runtime_error("Hardlinked preview output denied");
  fs::path report_target;
  if(!report_path.empty()){report_target=root/fs::u8path(report_path);if(!within_root(root,report_target)||sensitive_path(report_target))throw std::runtime_error("Preview report must be inside workspace and unprotected");if(fs::exists(report_target)&&fs::hard_link_count(report_target)>1)throw std::runtime_error("Hardlinked preview report denied");}
  std::string png,report;auto result=preview_capture(json_dump(request),root,{},false,&png,&report);if(result.exit_code){std::cerr<<result.output<<'\n';return result.exit_code;}auto saved=file_undo.write(root,target,png);if(saved.exit_code){std::cerr<<saved.output<<'\n';return saved.exit_code;}
  if(!report_path.empty()){auto saved_report=file_undo.write(root,report_target,report);if(saved_report.exit_code){std::cerr<<saved_report.output<<'\n';return saved_report.exit_code;}}
  std::cout<<result.output<<"\nSaved "<<output<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
