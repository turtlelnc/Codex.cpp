// Exercise the actual hosted authorization/serialization boundary with injected
// device-login events. No OAuth request, real code, Git child or model is used.
static int web_boundary_checks(const fs::path& input,const std::string& subset){
  using namespace native_web;
  try{
    const auto root=fs::absolute(input);private_dir(root);
    host_env("CODEX_CPP_CONTEXT_WINDOW","16384");host_env("CODEX_CPP_OUTPUT_RESERVE","2048");
    char name[]="boundary-test";char* args[]={name,nullptr};std::string why;
    auto parsed=parse_options(1,args,why);require(bool(parsed),why);Options initial=*parsed;
    if(subset=="all"||subset=="access"){
      HostedService service(root/"hosted",initial);service.binary=executable(name);
      for(const auto& user:{"owner","member","outsider"})service.add_user(user);
      service.database.fields["projects"].fields["fixture"]=obj({{"owner",str("owner")},{"members",arr({str("owner"),str("member")})}});
      auto shared=service.app("owner","fixture",false);
      // Snapshot dispatch does not operate on Git. A fixture marker avoids
      // invoking repository initialization unrelated to authorization.
      for(const auto& user:{"owner","member"})for(bool personal:{false,true}){
        auto repository=service.repository(user,"fixture",personal);std::ofstream(repository/"HEAD")<<"fixture\n";
      }
      auto request=[&](const std::string& scope,const std::optional<std::string>& since){auto message=obj({{"op",str("snapshot")},{"project",str("fixture")},{"scope",str(scope)}});if(since)message.fields["since"]=str(*since);return message;};
      auto public_event=obj({{"type",str("model.delta")},{"payload",obj({{"text",str("PUBLIC_PROGRESS")}})}});
      shared->busy=true;shared->events=arr({public_event});
      const auto member_base=service.dispatch("member",request("shared",std::string()));
      shared->events.items.push_back(obj({{"type",str("login.notice")},{"payload",obj({{"text",str("DEVICE_CODE_OWNER_SECRET_1")}})}}));
      auto assert_private=[&](const J& value){auto raw=json_dump(value);require(raw.find("login.notice")==std::string::npos&&raw.find("DEVICE_CODE_")==std::string::npos,"Member received creator login information");};
      assert_private(service.dispatch("member",request("shared",std::nullopt)));
      auto delta=service.dispatch("member",request("shared",text(member_base,"revision")));
      require(flag(delta,"partial"),"Expected a negotiated member delta");assert_private(delta);
      for(int i=2;i<=20;++i){shared->events.items.push_back(obj({{"type",str("login.notice")},{"payload",obj({{"text",str("DEVICE_CODE_OWNER_SECRET_"+std::to_string(i))}})}}));
        shared->events.items.push_back(public_event);
        auto next=service.dispatch("member",request("shared",text(delta,"revision")));assert_private(next);delta=next;
        auto owner=service.dispatch("owner",request("shared",std::string()));require(json_dump(owner).find("DEVICE_CODE_OWNER_SECRET_")!=std::string::npos,"Creator cannot see their login notice");
        // Even reusing the other viewer's revision must use the caller's view.
        assert_private(service.dispatch("member",request("shared",text(owner,"revision"))));
        auto owner_delta=service.dispatch("owner",request("shared",text(delta,"revision")));require(json_dump(owner_delta).find("DEVICE_CODE_OWNER_SECRET_")!=std::string::npos,"Creator delta lost authorized login notice");
      }
      assert_private(service.dispatch("member",request("shared",std::string("expired-view"))));
      auto private_app=service.app("member","fixture",true);private_app->events=arr({obj({{"type",str("login.notice")},{"payload",obj({{"text",str("DEVICE_CODE_PERSONAL_SECRET")}})}})});
      require(json_dump(service.dispatch("member",request("personal",std::string()))).find("DEVICE_CODE_PERSONAL_SECRET")!=std::string::npos,"Member cannot see their own personal login");
      require(json_dump(service.dispatch("owner",request("personal",std::string()))).find("DEVICE_CODE_PERSONAL_SECRET")==std::string::npos,"Creator received another user's personal login");
      bool denied=false;try{service.dispatch("outsider",request("shared",std::string()));}catch(const std::exception&){denied=true;}require(denied,"Nonmember snapshot was allowed");shared->busy=false;
      std::cout<<"PASS creator/member/nonmember full and delta login privacy, polling, cross-view revisions, stale bases and personal isolation\n";
    }
    if(subset=="all"||subset=="budget"){
      auto workspace=root/"provider-workspace",home=root/"provider-home";fs::create_directories(workspace);
      auto verify=[&](App& app,size_t window,size_t output){require(app.options.context_window==window&&app.options.output_reserve==output,"Provider change lost deployment context budget");auto argv=app.agent_args();for(const auto& setting:{std::make_pair("--context-window",window),std::make_pair("--output-reserve",output)}){auto found=std::find(argv.begin(),argv.end(),setting.first);require(found!=argv.end()&&found+1!=argv.end()&&*(found+1)==std::to_string(setting.second),"Agent child received the wrong context budget");}};
      std::string id;
      {
        App app(workspace,executable(name),home,initial,false);verify(app,16384,2048);
        auto saved=app.rpc(obj({{"op",str("provider_save")},{"name",str("fixture")},{"base_url",str("http://127.0.0.1:9")},{"api",str("chat")},{"models",arr({str("fixture")})}}));id=text(saved,"id");
        app.rpc(obj({{"op",str("provider_use")},{"id",str(id)}}));verify(app,16384,2048);
        app.use_provider("codex");verify(app,16384,2048);app.use_provider(id);verify(app,16384,2048);
      }
      {App restarted(workspace,executable(name),home,initial,false);require(restarted.active==id,"Saved provider not restored");verify(restarted,16384,2048);restarted.rpc(obj({{"op",str("provider_delete")},{"id",str(id)}}));verify(restarted,16384,2048);}
      {auto explicit_options=initial;explicit_options.context_window=32768;explicit_options.output_reserve=4096;App app(workspace,executable(name),root/"explicit-home",explicit_options,true);verify(app,32768,4096);app.use_provider("codex");verify(app,32768,4096);}
      std::cout<<"PASS environment and explicit context budgets through provider selection/deletion/restart and actual Agent argv construction\n";
    }
    return 0;
  }catch(const std::exception& e){std::cerr<<"Web boundary regression: "<<e.what()<<'\n';return 2;}
}
