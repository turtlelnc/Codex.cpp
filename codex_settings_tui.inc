namespace native_web {
static std::shared_ptr<portable::Lease> lock_provider_registry(const fs::path& home){
  private_dir(home);return std::make_shared<portable::Lease>(home,true,"providers.lock","Providers are being updated by another interface; retry");
}
static J save_preferences(const fs::path& home,const fs::path& root,const std::string& actor,const J& patch){
  const auto directory=assistance_directory(home,root)/"ui-preferences";private_dir(directory);
  portable::Lease lease(directory,true,"preferences.lock","Settings are being saved by another interface; retry");
  auto next=load_preferences(home,root,actor);
  for(const auto& entry:preference_definitions())if(auto value=patch.get(entry.key)){
    require(value->kind==J::Other&&(value->value=="true"||value->value=="false"),"Preference must be a boolean");next.fields[entry.key]=*value;
  }
  atomic_save(directory/(digest(actor)+".json"),next);return next;
}
}
// The local settings console uses the exact same RPC validation/storage as WebUI.
// No HTTP listener, model request or plugin loading is needed to open settings.
struct LocalModelSettings : RemoteTeamConsole {
  native_web::App& application;bool login_requested=false;
  explicit LocalModelSettings(native_web::App& value):application(value){}
  J rpc(const std::string& op,J values=native_web::obj(),bool=false)override{
    if(op=="provider_use"&&native_web::text(values,"id")=="cli"){native_web::require(application.active=="cli","Startup provider is no longer active");return native_web::obj({{"active",native_web::str("cli")}});}
    values.fields["op"]=native_web::str(op);return application.rpc(values);
  }
  J enqueue(TuiRenderer&,const std::string& op,J=native_web::obj(),bool=false)override{
    native_web::require(op=="login","Only device login is available from local model settings");login_requested=true;return native_web::obj();
  }
};
static void apply_settings_provider(const native_web::App& app,Options& options,ApiClient& api){
  const auto& next=app.options;
  options.provider=next.provider;options.base_url=next.base_url;options.model=next.model;
  options.api_style=next.api_style;options.api_key_env=next.api_key_env;options.no_api_key=next.no_api_key;options.reasoning_effort=next.reasoning_effort;
  api.base_url=next.base_url;api.model=next.model;api.style=next.api_style;api.no_api_key=next.no_api_key;api.reasoning_effort=next.reasoning_effort;
  auto secret=app.secrets.find(next.api_key_env);api.api_key=next.no_api_key?"":secret!=app.secrets.end()?secret->second:getenv_or(next.api_key_env.c_str());
  api.chatgpt_auth=next.provider=="codex";api.wait_quota=api.chatgpt_auth&&options.wait_quota;
  while(!api.base_url.empty()&&api.base_url.back()=='/')api.base_url.pop_back();
}
static std::string settings_console(TuiRenderer& ui,Options& options,ApiClient& api,TeamConsole& team,const fs::path& binary){
  using namespace native_web;
  options.model=api.model;options.reasoning_effort=api.reasoning_effort;
  App application(options.root,binary,own_auth_path().parent_path(),options,true);
  LocalModelSettings models(application);
  for(;;){try{
    int section=ui.settings_menu("Settings","Shared with WebUI for this local profile and workspace. No model usage to open settings.",
      {"General / assistance","Models and reasoning","Providers and account","Team","Plugins","About / data and updates","Back"});
    if(section<0||section==6)return {};
    if(section==0||section==1){
      for(;;){auto values=application.rpc(obj({{"op",str("preferences")}}));
        std::vector<const PreferenceDefinition*> entries;std::vector<std::string> labels;
        for(const auto& entry:preference_definitions())if((section==1)==(std::string(entry.key)=="carry_reasoning")){entries.push_back(&entry);labels.push_back(std::string(flag(values,entry.key)?"[on] ":"[off] ")+entry.label);}
        if(section==1)labels.push_back("Choose model / reasoning effort");labels.push_back("Back");
        int choice=ui.settings_menu(section==0?"General / assistance":"Models and reasoning",section==0?"Applies to WebUI assistance; TUI does not generate background suggestions.":"Only provider-returned Chat reasoning can be resent. Hidden reasoning is unavailable. Required tool reasoning stays automatic. Changes apply to the next task; explicit CLI overrides take precedence.",labels);
        if(choice<0||choice==static_cast<int>(labels.size()-1))break;
        if(static_cast<size_t>(choice)==entries.size())return "/model";
        const auto& entry=*entries[static_cast<size_t>(choice)];
        if(ui.settings_menu(entry.label,entry.help,{"Cancel",flag(values,entry.key)?"Turn off":"Turn on"})==1)
          application.rpc(obj({{"op",str("preferences_save")},{entry.key,boolean(!flag(values,entry.key))}}));
      }
    }else if(section==2){models.models(ui,false,true);apply_settings_provider(application,options,api);ui.set_provider(options.provider);ui.set_model(api.model,api.reasoning_effort);if(models.login_requested)return "/login";}
    else if(section==3)team.open(ui);
    else if(section==4){std::string body="Native plugins run with host process permissions. This screen never loads or enables code. Restart is required to change startup modules.\n\n";for(const auto& service:plugin_runtime().inspect())body+=service.name+" -> "+service.owner+"\n";RemoteTeamConsole::view(ui,"Plugin services",body);}
    else {int action=ui.settings_menu("About / data and updates","Profile: "+own_auth_path().parent_path().u8string()+"\nWorkspace: "+options.root.u8string()+"\nPreferences are shared with local WebUI. Remote account/project settings remain on their server. Updating requires explicit installation confirmation.",{"Open WebUI","Update status","Check updates","Third-party licenses","Back"});
      if(action==0)return "/web";if(action==1)return "/update status";if(action==2)return "/update check";if(action==3)RemoteTeamConsole::view(ui,"Licenses",std::string(codex_resource("licenses")));}
  }catch(const std::exception& e){RemoteTeamConsole::view(ui,"Settings error",e.what());}}
}
