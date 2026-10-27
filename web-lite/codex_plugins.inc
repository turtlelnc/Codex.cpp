// User plugins: declarative contributions; executable tools stay behind MCP.
struct PluginAction{std::string label,command;};
struct PluginCommand{std::string name,description,prompt;};
struct Plugin{std::string id,version,description;fs::path dir;Json manifest;bool enabled=false;std::string error;std::vector<PluginCommand> commands;std::vector<PluginAction> actions;std::map<std::string,std::string> settings;};
static fs::path plugin_home(){return fs::path(getenv_or("CODEX_CPP_HOME",getenv_or("HOME",getenv_or("USERPROFILE"))+"/.codex-cpp"));}
static bool plugin_id(const std::string& id){return !id.empty()&&id.size()<=32&&id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos;}
static std::string plugin_string(const Json& j,const std::string& key){auto v=j.get(key);return v&&v->kind==Json::String?v->value:"";}
static bool plugin_save(const fs::path& path,const std::string& text,std::string& error){
  if(!within_root(plugin_home(),path)){error="configuration path escapes user directory";return false;}
  const auto temp=temp_path_in(path.parent_path(),".plugin-write");std::error_code ec;
  if(!write_all(temp,text,error)){fs::remove(temp,ec);return false;}
  fs::permissions(temp,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace,ec);
#if defined(_WIN32)
  if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){error="cannot replace plugin configuration: "+std::to_string(GetLastError());fs::remove(temp,ec);return false;}
#else
  fs::rename(temp,path,ec);if(ec){error=ec.message();fs::remove(temp,ec);return false;}
#endif
  return true;
}
static std::optional<Json> plugin_json(const std::string& text){JsonReader reader{text};reader.unique_keys=true;return reader.complete();}
struct PluginRegistry{
  std::vector<Plugin> entries;std::vector<std::string> errors;std::set<std::string> enabled_ids;
  std::function<void(const std::string&,const std::string&,const std::string&)> display;
  static bool validate(Plugin& p){auto fail=[&](const std::string& why){p.error=why;return false;};
    p.commands.clear();p.actions.clear();p.settings.clear();p.error.clear();
    if(p.manifest.kind!=Json::Object)return fail("manifest must be an object");
    auto api=p.manifest.get("api");if(!api||api->kind!=Json::Other||api->value!="1")return fail("requires plugin API 1");
    p.id=plugin_string(p.manifest,"id");p.version=plugin_string(p.manifest,"version");p.description=plugin_string(p.manifest,"description");
    if(!plugin_id(p.id)||p.version.empty()||p.version.size()>64||p.description.size()>512)return fail("invalid id/version/description");
    auto commands=p.manifest.get("commands");if(commands){if(commands->kind!=Json::Object||commands->fields.size()>32)return fail("invalid commands");
      for(const auto&[name,c]:commands->fields){auto prompt=plugin_string(c,"prompt"),description=plugin_string(c,"description");
        if(!plugin_id(name)||prompt.empty()||prompt.size()>16384||description.size()>256)return fail("invalid command: "+name);
        p.commands.push_back({"/"+p.id+":"+name,description,prompt});}}
    if(auto settings=p.manifest.get("settings")){if(settings->kind!=Json::Object||settings->fields.size()>16)return fail("invalid settings");for(const auto&[key,value]:settings->fields){if(!plugin_id(key)||value.kind!=Json::String||value.value.size()>4096)return fail("invalid setting: "+key);p.settings[key]=value.value;}}
    if(auto actions=p.manifest.get("actions")){if(actions->kind!=Json::Array||actions->items.size()>8)return fail("invalid actions");for(const auto& action:actions->items){const auto label=plugin_string(action,"label"),command=plugin_string(action,"command");if(label.empty()||label.size()>80||!commands||!commands->get(command))return fail("action must reference a declared command");p.actions.push_back({label,"/"+p.id+":"+command});}}
    auto ui=p.manifest.get("ui");if(ui){if(ui->kind!=Json::Array||ui->items.size()>3)return fail("invalid ui");for(const auto& kind:ui->items)if(kind.kind!=Json::String||(kind.value!="panel"&&kind.value!="status"&&kind.value!="card"))return fail("unknown ui kind");}
    auto events=p.manifest.get("events");if(events){if(events->kind!=Json::Object||events->fields.size()>4)return fail("invalid events");
      for(const auto&[event,actions]:events->fields){if(event!="session.started"&&event!="turn.started"&&event!="tool.completed"&&event!="turn.completed")return fail("unknown event: "+event);
        if(actions.kind!=Json::Array||actions.items.size()>4)return fail("invalid event actions");for(const auto& action:actions.items){auto kind=plugin_string(action,"kind"),text=plugin_string(action,"text");if(!action.get("text")||action.get("text")->kind!=Json::String||text.size()>4096||!has_ui(p,kind))return fail("invalid event UI action");}}}
    if(auto frontend=p.manifest.get("frontend")){const auto protocol=frontend->get("protocol"),args=frontend->get("args");const auto command=plugin_string(*frontend,"command");
      if(frontend->kind!=Json::Object||!protocol||protocol->kind!=Json::Other||protocol->value!="1"||command.empty()||command.size()>4096||!args||args->kind!=Json::Array||args->items.size()>64)return fail("invalid frontend protocol/command/args");
      for(const auto& arg:args->items)if(arg.kind!=Json::String||arg.value.size()>4096)return fail("invalid frontend argument");}
    if(auto m=p.manifest.get("mcp")){if(m->kind!=Json::String||m->value.empty())return fail("mcp must name a config file");if(!within_root(p.dir,p.dir/m->value))return fail("MCP config escapes plugin");}
    return true;
  }
  static bool has_ui(const Plugin& p,const std::string& kind){auto ui=p.manifest.get("ui");if(ui)for(const auto& item:ui->items)if(item.str()==kind)return true;return false;}
  static std::optional<Plugin> read(const fs::path& dir,std::string& error){std::error_code ec;auto file=dir/"plugin.json";
    if(!within_root(dir,file)||!fs::is_regular_file(file,ec)||fs::file_size(file,ec)>65536||ec){error="missing/oversized plugin.json";return{};}
    auto manifest=plugin_json(slurp(file,65537));if(!manifest){error="invalid plugin.json";return{};}Plugin p;p.dir=dir;p.manifest=*manifest;
    if(!validate(p)){error=p.error;return{};}return p;
  }
  void load(){entries.clear();errors.clear();enabled_ids.clear();std::error_code state_ec;const auto state_file=plugin_home()/"plugins-enabled.json";auto state=plugin_json(slurp(state_file,16384));
    bool valid_state=state&&state->kind==Json::Array;if(valid_state)for(const auto& item:state->items)if(item.kind!=Json::String||!plugin_id(item.value)){valid_state=false;break;}
    if(valid_state)for(const auto& item:state->items)enabled_ids.insert(item.value); // keep malformed state fail-closed
    if(fs::exists(state_file,state_ec)&&!valid_state)errors.push_back("Invalid plugins-enabled.json; no plugins enabled");
    auto base=plugin_home()/"plugins";std::error_code ec;if(!fs::is_directory(base,ec))return;
    for(const auto& entry:fs::directory_iterator(base,ec)){if(entries.size()+errors.size()>=64){errors.push_back("plugin limit reached");break;}
      if(entry.path().filename().string().front()=='.'||entry.is_symlink(ec)||!entry.is_directory(ec))continue;std::string error;auto p=read(entry.path(),error);
      if(!p){errors.push_back(entry.path().filename().string()+": "+error);continue;}
      if(p->id!=entry.path().filename().string()){errors.push_back(p->id+": folder must match plugin id");continue;}
      p->enabled=enabled_ids.count(p->id)!=0;if(!configure(*p,error)){p->enabled=false;p->error=error;errors.push_back(p->id+": "+error);}entries.push_back(std::move(*p));}
    if(ec)errors.push_back(ec.message());std::sort(entries.begin(),entries.end(),[](const Plugin& a,const Plugin& b){return a.id<b.id;});
  }
  const Plugin* find(const std::string& id)const{for(const auto& p:entries)if(p.id==id)return &p;return nullptr;}
  std::string list()const{std::ostringstream out;for(const auto& p:entries){out<<p.id<<" "<<p.version<<" ["<<(p.enabled?"active":"inactive")<<"]";
      if(p.enabled!=(enabled_ids.count(p.id)!=0))out<<" ["<<(enabled_ids.count(p.id)?"enable":"disable")<<" after restart]";
      out<<" — "<<p.description<<"\n  commands="<<p.commands.size()<<" skills="<<(fs::is_directory(p.dir/"skills")?"yes":"no")<<" MCP="<<(p.manifest.get("mcp")?"LOCAL PROCESS":"none")<<"\n";}
    for(const auto& e:errors)out<<"ERROR "<<e<<'\n';return out.str().empty()?"No plugins installed.":out.str();}
  bool set_enabled(const std::string& id,bool value,std::string& error){if(!find(id)){error="plugin not found";return false;}auto next=enabled_ids;if(value)next.insert(id);else next.erase(id);
    std::string json="[";for(const auto& name:next){if(json.size()>1)json+=",";json+="\""+json_escape(name)+"\"";}json+="]\n";
    if(!plugin_save(plugin_home()/"plugins-enabled.json",json,error))return false;enabled_ids=std::move(next);return true;}
  static fs::path settings_path(const std::string& id){return plugin_home()/"plugin-settings"/(id+".json");}
  static bool configure(Plugin& p,std::string& error){const auto file=settings_path(p.id);std::error_code ec;if(!fs::exists(file,ec))return true;
    if(!within_root(plugin_home(),file)||!fs::is_regular_file(file,ec)||fs::file_size(file,ec)>65536||ec){error="invalid settings file";return false;}
    auto data=plugin_json(slurp(file,65537));if(!data||data->kind!=Json::Object){error="settings must be a JSON object";return false;}
    for(const auto&[key,value]:data->fields){if(!p.settings.count(key)||value.kind!=Json::String||value.value.size()>4096){error="unknown/invalid setting: "+key;return false;}p.settings[key]=value.value;}return true;
  }
  bool config(const std::string& id,const std::string& key,const std::optional<std::string>& value,std::string& error){auto p=find(id);if(!p||!plugin_id(key)||(value&&!p->settings.count(key))){error="unknown plugin/setting";return false;}
    if(value&&value->size()>4096){error="setting exceeds 4096 bytes";return false;}
    auto json=plugin_json(slurp(settings_path(id),65537));if(!json||json->kind!=Json::Object)json=Json{Json::Object,{}, {},{}};
    if(value){Json entry;entry.kind=Json::String;entry.value=*value;json->fields[key]=std::move(entry);}else json->fields.erase(key);
    return plugin_save(settings_path(id),json_dump(*json)+"\n",error);
  }
  std::string describe(const std::string& id)const{auto p=find(id);if(!p)return "Plugin not found.";std::ostringstream out;out<<p->id<<" "<<p->version<<"\n"<<p->description<<"\n"<<p->dir.string()<<"\n";
    if(!p->error.empty())out<<"ERROR: "<<p->error<<'\n';for(const auto& c:p->commands)out<<c.name<<"  "<<c.description<<'\n';
    for(const auto&[key,value]:p->settings)out<<"setting "<<key<<" = "<<value<<'\n';if(p->manifest.get("mcp"))out<<"MCP: local child processes use the OS sandbox; unavailable backends fail closed.\n";return out.str();}
  std::string install(const fs::path& source,bool update=false){std::string error;auto p=read(source,error);if(!p)return error;
    auto dest=plugin_home()/"plugins"/p->id;std::error_code ec;const auto prior=find(p->id);
    if(update&&(!prior||prior->enabled))return "Update requires an installed inactive plugin; disable and restart first";
    if(!update&&fs::exists(dest,ec))return "plugin already installed";
    std::vector<fs::path> files;uintmax_t bytes=0;size_t nodes=0;for(const auto& e:fs::recursive_directory_iterator(source,ec)){
      if(++nodes>1024)return "plugin exceeds 1024 directory entries";
      if(e.is_symlink(ec))return "plugin links are not supported";if(e.is_directory(ec))continue;if(!e.is_regular_file(ec))return "plugin contains a special file";
      bytes+=e.file_size(ec);files.push_back(e.path().lexically_relative(source));if(ec||files.size()>256||bytes>4*1024*1024)return "plugin exceeds 256 files / 4 MiB";}
    if(ec)return ec.message();const auto stage=plugin_home()/"plugins"/(".install-"+random_id("stage"));if(!within_root(plugin_home(),stage)||fs::exists(stage,ec))return "unsafe installation path";
    fs::create_directories(stage,ec);if(ec)return ec.message();
    for(const auto& file:files){fs::create_directories((stage/file).parent_path(),ec);if(ec)break;fs::copy_file(source/file,stage/file,fs::copy_options::none,ec);if(ec)break;}
    auto staged=read(stage,error);if(ec||!staged||staged->id!=p->id){const auto why=ec?ec.message():"copied manifest failed validation: "+error;fs::remove_all(stage,ec);return why;}
    if(prior){if(!set_enabled(p->id,false,error)){fs::remove_all(stage,ec);return error;}}
    else{entries.push_back(*p);const bool saved=set_enabled(p->id,false,error);entries.pop_back();if(!saved){fs::remove_all(stage,ec);return error;}}
    fs::path backup;if(update){backup=plugin_home()/"plugin-backups"/(p->id+"-"+random_id("version"));if(!within_root(plugin_home(),dest)||fs::is_symlink(dest,ec)){fs::remove_all(stage,ec);return "unsafe update destination";}
      fs::create_directories(backup.parent_path(),ec);if(!ec)fs::rename(dest,backup,ec);if(ec){auto why=ec.message();fs::remove_all(stage,ec);return why;}}
    fs::rename(stage,dest,ec);if(ec){auto why=ec.message();if(update)fs::rename(backup,dest,ec);fs::remove_all(stage,ec);return why;}
    entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const Plugin& item){return item.id==p->id;}),entries.end());staged->dir=dest;staged->enabled=false;configure(*staged,error);entries.push_back(std::move(*staged));
    return std::string(update?"Updated ":"Installed ")+p->id+" (disabled). Inspect /plugins info "+p->id+", then /plugins enable "+p->id+" and restart."+(backup.empty()?"":" Previous version: "+backup.string());
  }
  std::string uninstall(const std::string& id){if(!plugin_id(id))return "invalid plugin id";auto p=find(id);if(p&&p->enabled)return "Disable and restart before uninstalling an active plugin.";
    auto path=plugin_home()/"plugins"/id;std::error_code ec;if(!fs::is_directory(path,ec)||!within_root(plugin_home(),path)||fs::is_symlink(path,ec))return "Plugin not found or unsafe path.";
    if(!p){Plugin broken;broken.id=id;entries.push_back(broken);}std::string error;if(!set_enabled(id,false,error)){if(!p)entries.pop_back();return error;}
    fs::remove_all(path,ec);if(ec)return ec.message();entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const Plugin& item){return item.id==id;}),entries.end());return "Uninstalled "+id+". Settings and version backups retained.";
  }
  std::vector<PluginCommand> commands()const{std::vector<PluginCommand> result;for(const auto& p:entries)if(p.enabled)result.insert(result.end(),p.commands.begin(),p.commands.end());return result;}
  std::string expand(const std::string& token,const std::string& args)const{for(const auto& c:commands())if(c.name==token)return c.prompt+(args.empty()?"":"\n\nUser arguments:\n"+args);return{};}
  std::string ui(const std::string& id,const std::string& kind,const std::string& text){auto p=find(id);if(!p||!p->enabled||!has_ui(*p,kind))return "plugin/UI contribution is not enabled";
    if(text.size()>4096)return "UI text exceeds 4096 bytes";if(display)display(id,kind,text);return{};}
  void event(const std::string& name,const fs::path& workspace,const std::string& tool="",int code=0,const std::string& status=""){for(const auto& p:entries)if(p.enabled){auto events=p.manifest.get("events"),actions=events?events->get(name):nullptr;if(!actions)continue;
      for(const auto& action:actions->items){std::string text=plugin_string(action,"text");for(const auto&[key,value]:std::map<std::string,std::string>{{"{{workspace}}",workspace.string()},{"{{tool}}",tool},{"{{exit_code}}",std::to_string(code)},{"{{status}}",status}}){size_t pos=0;while((pos=text.find(key,pos))!=std::string::npos){text.replace(pos,key.size(),value);pos+=value.size();}}
        ui(p.id,plugin_string(action,"kind"),text);}}}
};
static PluginRegistry plugins;
class PluginUiTool final:public ToolRuntime{public:std::string name()const override{return "plugin_ui";}std::string spec_json()const override{return R"({"type":"function","name":"plugin_ui","description":"Update an enabled plugin's declared UI contribution. Text only; empty panel/status clears it.","parameters":{"type":"object","properties":{"plugin":{"type":"string"},"kind":{"type":"string","enum":["panel","status","card"]},"text":{"type":"string"}},"required":["plugin","kind","text"],"additionalProperties":false},"strict":true})";}
  ToolResult run(const std::string& args,const fs::path&)override{auto p=json_string_field(args,"plugin"),k=json_string_field(args,"kind"),t=json_string_field(args,"text");if(!p||!k||!t)return{2,"missing plugin/kind/text"};auto error=plugins.ui(*p,*k,*t);return{error.empty()?0:2,error.empty()?"UI updated":error};}
};
