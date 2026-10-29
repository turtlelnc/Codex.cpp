struct RemoteTeamConsole {
  using J=native_web::J;
  TeamConnection connection;
  std::string project;
  J rpc(const std::string& op,J values=native_web::obj(),bool personal=false){
    values.fields["project"]=native_web::str(project);
    values.fields["scope"]=native_web::str(personal?"personal":"shared");
    return connection.rpc(op,values);
  }
  static bool confirm(TuiRenderer& ui,const std::string& title,const std::string& body={}){
    return ui.settings_menu(title,body,{"Cancel","Confirm"})==1;
  }
  static std::string pretty(const J& value,int indent=0){
    if(value.kind!=J::Object&&value.kind!=J::Array)return json_dump(value);
    std::string out;const std::string pad(static_cast<size_t>(indent),' ');
    if(value.kind==J::Object)for(const auto& field:value.fields)out+=pad+field.first+": "+pretty(field.second,indent+2)+"\n";
    else for(const auto& item:value.items)out+=pad+"- "+pretty(item,indent+2)+"\n";
    return out;
  }
  static void view(TuiRenderer& ui,const std::string& title,const std::string& body){
    const size_t capacity=std::max<size_t>(240,std::min<size_t>(2000,(terminal_rows()>16?terminal_rows()-16:4)*std::max<size_t>(20,std::min<size_t>(90,terminal_columns())-16)));
    std::vector<std::string> pages;
    for(size_t start=0;start<body.size();){size_t end=std::min(body.size(),start+capacity);
      if(end<body.size()){auto line=body.rfind('\n',end);if(line!=std::string::npos&&line>start+capacity/2)end=line+1;
        else while(end>start&&(static_cast<unsigned char>(body[end])&0xc0)==0x80)--end;}
      pages.push_back(body.substr(start,end-start));start=end;
    }
    if(pages.empty())pages.push_back("No entries");size_t page=0;
    for(;;){int selected=ui.settings_menu(title+" ("+std::to_string(page+1)+"/"+std::to_string(pages.size())+")",pages[page],{"Next page","Previous page","Back"});
      if(selected<0||selected==2)return;if(selected==0&&page+1<pages.size())++page;if(selected==1&&page)--page;}
  }
  J enqueue(TuiRenderer& ui,const std::string& op,J values=native_web::obj(),bool personal=false){
    values.fields["request_id"]=native_web::str(native_web::entropy());auto result=rpc(op,values,personal);
    view(ui,"Task queued",pretty(result)+"\nCheck Activity for completion or errors. Conflicts do not overwrite the target branch.");return result;
  }
  void members(TuiRenderer& ui,const J& info){using namespace native_web;
    const bool owner=text(info,"owner")==connection.username;
    std::string body="Creator: "+text(info,"owner")+"\nAll members have equal development and approval rights.\n";
    for(const auto& user:field(info,"members",arr()).items)body+="\n"+user.str();
    if(!owner){view(ui,"Project members",body);return;}
    const int action=ui.settings_menu("Project members",body,{"Invite enrolled account","Remove member","Back"});
    if(action==0){auto candidates=rpc("member_candidates");std::vector<std::string> names;
      for(const auto& user:field(candidates,"users",arr()).items)names.push_back(user.str());
      require(!names.empty(),"No available accounts. Register accounts on the server first.");
      int selected=ui.choose("Invite member (Esc cancels)",names);if(selected>=0)rpc("invite",obj({{"username",str(names[static_cast<size_t>(selected)])}}));
    }else if(action==1){std::vector<std::string> names;
      for(const auto& user:field(info,"members",arr()).items)if(user.str()!=text(info,"owner"))names.push_back(user.str());
      require(!names.empty(),"No removable members");int selected=ui.choose("Remove member (Esc cancels)",names);
      if(selected>=0&&confirm(ui,"Remove "+names[static_cast<size_t>(selected)]+"?","Their project files are preserved. Queued tasks may be cancelled."))
        rpc("remove",obj({{"username",str(names[static_cast<size_t>(selected)])}}));
    }
  }
  void save_provider(TuiRenderer& ui,const J& old,bool personal){using namespace native_web;
    auto name=ui.form_input("Provider name",text(old,"name"));if(!name)return;
    auto url=ui.form_input("Provider API base URL",text(old,"base_url","https://api.openai.com"));if(!url)return;
    int api=ui.choose("Provider API",{"Responses","Chat Completions"},text(old,"api")=="chat"?1:0);if(api<0)return;
    std::string models;for(const auto& model:field(old,"models",arr()).items){if(!models.empty())models+=' ';models+=model.str();}
    auto list=ui.form_input("Model IDs (space separated)",models);if(!list)return;
    auto key=ui.form_input("API key (blank keeps existing key)",{},true);if(!key)return;
    struct Wipe {std::string& value;~Wipe(){std::fill(value.begin(),value.end(),'\0');}} wipe{*key};
    J values=obj({{"id",str(text(old,"id"))},{"name",str(*name)},{"base_url",str(*url)},
      {"api",str(api==0?"responses":"chat")},{"models",str(*list)},{"key",str(*key)}});
    rpc("provider_save",values,personal);
  }
  void models(TuiRenderer& ui,bool personal,bool owner){using namespace native_web;
    const bool editable=personal||owner;
    auto catalog=rpc("providers",obj(),personal);auto providers=field(catalog,"providers",arr()).items;
    std::vector<std::string> names;for(const auto& p:providers)names.push_back(text(p,"name")+(text(p,"id")==text(catalog,"active")?" (active)":""));
    if(editable)names.push_back("Add provider");names.push_back("Back");
    int selected=ui.settings_menu(personal?"Personal models":"Shared models",editable?"Credentials stay on the server; secret inputs are masked.":"Only the project creator can configure shared credentials. You can use them.",names);
    if(selected<0||selected==static_cast<int>(names.size()-1))return;
    if(static_cast<size_t>(selected)==providers.size()){save_provider(ui,obj(),personal);return;}
    const auto provider=providers[static_cast<size_t>(selected)];const auto id=text(provider,"id");
    std::vector<std::string> actions={"Use provider","Choose model","Set reasoning effort"};
    if(editable&&!flag(provider,"builtin")){actions.push_back("Edit provider / replace API key");actions.push_back("Delete provider");actions.push_back("Clear API key");}
    else if(editable&&id=="codex")actions.push_back("Start Codex device login");
    actions.push_back("Back");int action=ui.settings_menu("Provider: "+text(provider,"name"),"API key: "+std::string(flag(provider,"has_key")?"configured (hidden)":"not configured or subscription login"),actions);
    if(action<0||action==static_cast<int>(actions.size()-1))return;
    if(action==0){rpc("provider_use",obj({{"id",str(id)}}),personal);}
    else if(action==1){
      std::vector<std::string> choices;for(const auto& model:field(provider,"models",arr()).items)choices.push_back(model.str());
      if(choices.empty()&&id==text(catalog,"active")){auto result=rpc("models",obj(),personal);for(const auto& model:field(result,"models",arr()).items)choices.push_back(text(model,"id"));}
      choices.push_back("Enter model ID");int index=ui.choose("Choose model",choices);if(index<0)return;
      std::optional<std::string> model;if(index==static_cast<int>(choices.size()-1))model=ui.form_input("Model ID");else model=choices[static_cast<size_t>(index)];
      if(model){rpc("provider_use",obj({{"id",str(id)}}),personal);rpc("model",obj({{"model",str(*model)}}),personal);}
    }else if(action==2){auto effort=ui.form_input("Reasoning effort (blank uses default)");if(effort){rpc("provider_use",obj({{"id",str(id)}}),personal);rpc("reasoning",obj({{"effort",str(*effort)}}),personal);}}
    else if(flag(provider,"builtin")){rpc("provider_use",obj({{"id",str(id)}}),personal);enqueue(ui,"login",obj(),personal);}
    else if(action==3){save_provider(ui,provider,personal);}
    else if(action==4){if(confirm(ui,"Delete provider?",text(provider,"name")))rpc("provider_delete",obj({{"id",str(id)}}),personal);}
    else if(action==5&&confirm(ui,"Clear stored API key?",text(provider,"name"))){J values=provider;values.fields["clear_key"]=boolean(true);rpc("provider_save",values,personal);}
  }
  void branches(TuiRenderer& ui){using namespace native_web;
    auto info=rpc("project_status");
    auto body=pretty(field(info,"branches",obj()));
    int action=ui.settings_menu("Project branches",body,{"Publish personal branch","Sync shared into personal","Review / commit shared changes","Merge a published member branch","View full branch list","Back"});
    if(action==0){auto message=ui.form_input("Personal commit message");if(message&&confirm(ui,"Publish personal branch?",*message))enqueue(ui,"publish",obj({{"message",str(*message)}}),true);}
    else if(action==1){if(confirm(ui,"Sync shared into personal?","Your personal workspace must be clean. Conflicts abort without overwriting the target."))enqueue(ui,"sync",obj(),true);}
    else if(action==2){auto preview=rpc("diff");view(ui,"Review shared changes",pretty(preview));
      auto message=ui.form_input("Shared commit message");if(message&&confirm(ui,"Commit the reviewed changes?",*message))enqueue(ui,"checkpoint",obj({{"message",str(*message)},{"fingerprint",field(preview,"fingerprint")}}));
    }else if(action==3){std::vector<std::string> names;for(const auto& item:field(info,"members",arr()).items)
        if(field(info,"branches",obj()).get("users/"+item.str()))names.push_back(item.str());
      require(!names.empty(),"No member has published a branch yet");int index=ui.choose("Choose published branch (Esc cancels)",names);if(index<0)return;
      auto latest=rpc("project_status");auto name=names[static_cast<size_t>(index)];auto source=field(field(latest,"branches",obj()),"users/"+name),target=field(latest,"head");
      require(source.kind==J::String,"Published branch no longer exists");
      if(confirm(ui,"Merge exact commits?","Source: "+source.str()+"\nTarget: "+target.str()+"\nConflicts abort; changed heads require a new confirmation."))
        enqueue(ui,"merge",obj({{"username",str(name)},{"source",source},{"target",target},{"confirmed",boolean(true)}}));
    }else if(action==4)view(ui,"Project branches",body);
  }
  void activity(TuiRenderer& ui,const J& info,bool owner){using namespace native_web;
    std::vector<std::string> options={"Recent tasks","Operation audit","Shared workspace status","Personal workspace status"};
    if(owner&&flag(info,"blocked"))options.push_back("Review and recover interrupted queue");options.push_back("Back");
    int action=ui.choose("Project activity",options);if(action<0||action==static_cast<int>(options.size()-1))return;
    if(action<2)view(ui,action==0?"Recent tasks":"Operation audit",pretty(field(info,action==0?"jobs":"audit",arr())));
    else if(action<4)view(ui,action==2?"Shared workspace status":"Personal workspace status",pretty(rpc("snapshot",obj(),action==3)));
    else{auto preview=rpc("diff");view(ui,"Review files before queue recovery",pretty(preview));
      if(confirm(ui,"Recover interrupted queue?","First inspect the files and ensure old processes have exited. Affected Agent conversations restart."))
        rpc("recover",obj({{"fingerprint",field(preview,"fingerprint")},{"confirmed",boolean(true)}}));}
  }
  void project_menu(TuiRenderer& ui){using namespace native_web;
    for(;;){try{auto info=rpc("project_status");bool owner=text(info,"owner")==connection.username;
      int action=ui.settings_menu("Project: "+project,"Server workspace | Signed in: "+connection.username+"\nCreator: "+text(info,"owner")+(flag(info,"blocked")?"\nQueue requires recovery":""),
        {"Members","Shared models","Personal models","Branches","Activity","Back","Agent tasks on server"});
      if(action<0||action==5)return;
      if(action==0)members(ui,rpc("project_status"));else if(action==1||action==2)models(ui,action==2,owner);else if(action==3)branches(ui);else if(action==4)activity(ui,rpc("project_status"),owner);else agent(ui);
    }catch(const std::exception& e){view(ui,"Team request failed",e.what());return;}}
  }
  void agent(TuiRenderer& ui){using namespace native_web;
    int scope=ui.choose("Server Agent workspace",{"Shared","Personal","Back"});if(scope<0||scope==2)return;bool personal=scope==1;
    int action=ui.settings_menu("Server Agent",personal?"Personal server workspace and credentials":"Shared server workspace and project credentials",
      {"Send task","View conversation / refresh","Review pending approval","Stop current task","Execution permission","Back"});
    if(action<0||action==5)return;
    if(action==0){auto prompt=ui.form_input("Task for server Agent");if(prompt)enqueue(ui,"send",obj({{"text",str(*prompt)}}),personal);}
    else if(action==1)view(ui,"Server conversation",pretty(rpc("snapshot",obj(),personal)));
    else if(action==2){auto pending=field(rpc("snapshot",obj(),personal),"pending");require(!text(pending,"call_id").empty(),"No pending approval");
      view(ui,"Review Agent operation",pretty(pending));
      int decision=ui.settings_menu("Approval decision",pretty(pending),{"Deny","Allow","Back"});
      if(decision==0||decision==1)rpc("approve",obj({{"approval",str(text(pending,"call_id"))},{"allow",boolean(decision==1)}}),personal);
    }else if(action==3){if(confirm(ui,"Stop current server task?"))rpc("stop",obj(),personal);}
    else {int permission=ui.choose("Server execution permission",{"Read only","Workspace write","Back"});
      if(permission==0||permission==1)rpc("permission",obj({{"permission",str(permission==0?"readonly":"workspace")}}),personal);}
  }
  void open(TuiRenderer& ui,const std::string& default_origin){using namespace native_web;
    if(!connection.connected()){
      auto url=ui.form_input("Team server URL (HTTP or HTTPS)",connection.origin.empty()?default_origin:connection.origin);if(!url)return;
      auto user=ui.form_input("Team username");if(!user)return;
      auto token=ui.form_input("Team access token",{},true);if(!token)return;
      struct Wipe {std::string& value;~Wipe(){std::fill(value.begin(),value.end(),'\0');}} wipe{*token};
      ui.render("connecting to team server...");connection.login(*url,*user,*token);ui.render();
    }
    while(connection.connected()){
      auto result=connection.rpc("projects");std::vector<std::string> projects;
      for(const auto& item:field(result,"projects",arr()).items)projects.push_back(item.str());
      auto choices=projects;choices.insert(choices.end(),{"Create project","Sign out","Back"});
      int selected=ui.settings_menu("Team projects",connection.origin+"\nSigned in: "+connection.username,choices);
      if(selected<0||selected==static_cast<int>(choices.size()-1))return;
      if(static_cast<size_t>(selected)<projects.size()){project=projects[static_cast<size_t>(selected)];project_menu(ui);}
      else if(static_cast<size_t>(selected)==projects.size()){
        auto name=ui.form_input("New team project name");if(name){auto created=connection.rpc("create",obj({{"name",str(*name)}}));project=text(created,"project");project_menu(ui);}
      }else{connection.logout();return;}
    }
  }
};
