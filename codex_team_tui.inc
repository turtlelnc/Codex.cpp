// Shares the same native manager, persistence and stopped-only rules as WebUI.
struct TeamConsole {
  native_web::LocalService service;
  explicit TeamConsole(const char* argv0){
    service.home=own_auth_path().parent_path();service.binary=native_web::executable(argv0);
    native_web::private_dir(service.home);
    service.port=0; // No local HTTP listener is created by this console.
  }
  native_web::J rpc(const std::string& op,native_web::J values=native_web::obj()){
    values.fields["op"]=native_web::str(op);return service.dispatch("local",values);
  }
  void open(TuiRenderer& ui){using namespace native_web;
    for(;;){try{
      auto status=rpc("team_get");std::string body=flag(status,"running")?"Team service running":"Team service stopped";
      body+="\nHTTPS: "+text(status,"origin","not configured")+"\nPort: "+json_dump(field(status,"port"));
      body+="\nData: "+text(status,"state_dir")+"\nAccounts:";
      for(const auto& user:field(status,"users",arr()).items)body+=" "+user.str();
      body+="\n"+text(status,"error");
      int choice=ui.settings_menu("Team settings",body,{"Configure HTTPS address and port","Manage accounts","Start team service","Stop team service","Connection and deployment","Back"});
      if(choice<0||choice==5)return;
      if(choice==0){
        auto origin=ui.form_input("Team HTTPS address",text(status,"origin"));if(!origin)continue;
        auto port=ui.form_input("Loopback backend port",json_dump(field(status,"port")));if(!port)continue;
        require(!port->empty()&&port->size()<=5&&std::all_of(port->begin(),port->end(),[](unsigned char c){return std::isdigit(c);}),"Port must be an integer");
        rpc("team_save",obj({{"origin",str(*origin)},{"port",number(std::stoi(*port))}}));
      }else if(choice==1){
        service.stopped_only();std::vector<std::string> names;
        for(const auto& user:field(status,"users",arr()).items)names.push_back(user.str());
        std::vector<std::string> options={"Register account"};options.insert(options.end(),names.begin(),names.end());options.push_back("Back");
        int user=ui.choose("Team accounts (service must be stopped)",options);
        if(user<0||user==static_cast<int>(options.size()-1))continue;
        std::optional<std::string> name;bool reset=false,revoke=false;
        if(user==0){name=ui.form_input("New username");if(!name)continue;
          require(std::find(names.begin(),names.end(),*name)==names.end(),"Account exists; select it to reset its token");
        }else{name=names[static_cast<size_t>(user-1)];int action=ui.choose("Account: "+*name,{"Reset access token","Revoke account","Back"});
          if(action<0||action==2)continue;revoke=action==1;reset=true;
          if(ui.choose(revoke?"Revoke account? Project files will be kept":"Invalidate old token and issue a new one?",{"Cancel","Confirm"})!=1)continue;
        }
        auto result=rpc(revoke?"team_revoke":"team_account",obj({{"username",str(*name)},{"confirmed",boolean(reset)}}));
        if(!revoke){std::string token=text(result,"token");
          // Never add tokens to transcript/history/session logs.
          int action=ui.settings_menu("Save access token for "+*name,"Token is shown once; closing clears it.\n"+token,{"Close","Copy to clipboard"});
          if(action==1){auto error=clipboard_copy(token);if(!error.empty())ui.settings_menu("Clipboard unavailable",error,{"Close"});}
          std::fill(token.begin(),token.end(),'\0');
        }
      }else if(choice==2){rpc("team_start");}
      else if(choice==3){if(ui.choose("Stop team service? Active tasks will be interrupted",{"Cancel","Stop"})==1)rpc("team_stop",obj({{"confirmed",boolean(true)}}));}
      else {auto origin=text(status,"origin");ui.settings_menu("Team connection and deployment",
        "Open "+origin+" in a browser; sign in with username and access token.\n"
        "Configure an HTTPS reverse proxy to 127.0.0.1:"+json_dump(field(status,"port"))+".\n"
        "Project members and shared models are managed in the team's WebUI.\n"
        "Closing this TUI stops the team service it started.\n"
        "This console and the local WebUI use the same configuration; stop the other instance before changes.",{"Back"});}
    }catch(const std::exception& e){ui.settings_menu("Team settings error",e.what(),{"Back"});}}
  }
};
