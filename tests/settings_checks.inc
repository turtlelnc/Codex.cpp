{
  using namespace native_web;
  const auto home=root/"settings-home",workspace=root/"settings-work";fs::create_directories(workspace);
  auto initial=load_preferences(home,workspace);
  check(flag(initial,"side_chat")&&!flag(initial,"auto_suggestions")&&!flag(initial,"carry_reasoning"),"Shared settings have conservative defaults");
  auto saved=save_preferences(home,workspace,"",obj({{"carry_reasoning",boolean(true)}}));
  saved=save_preferences(home,workspace,"",obj({{"side_chat",boolean(false)}}));
  check(flag(saved,"carry_reasoning")&&!flag(saved,"side_chat"),"Setting patches preserve other shared preferences");
  bool bad=false;try{save_preferences(home,workspace,"",obj({{"carry_reasoning",str("true")}}));}catch(...){bad=true;}
  check(bad&&flag(load_preferences(home,workspace),"carry_reasoning"),"Nonboolean settings fail without overwriting saved state");
  check(!flag(load_preferences(home,workspace,"other-user"),"carry_reasoning")&&!flag(load_preferences(home,root/"other-work"),"carry_reasoning"),"Settings isolate workspace and actor identity");
  const auto directory=assistance_directory(home,workspace)/"ui-preferences";
  {portable::Lease lease(directory,true,"preferences.lock");bool busy=false;try{save_preferences(home,workspace,"",obj({{"carry_reasoning",boolean(false)}}));}catch(...){busy=true;}check(busy,"Concurrent settings writer is rejected without lost updates");}
  Options options;options.root=workspace;App app(workspace,{},home,options,true);
  auto observed=app.rpc(obj({{"op",str("preferences")}}));
  check(flag(observed,"carry_reasoning")&&!flag(observed,"side_chat"),"WebUI RPC sees settings written by shared TUI store");
  app.rpc(obj({{"op",str("preferences_save")},{"carry_reasoning",boolean(false)}}));
  check(!flag(load_preferences(home,workspace),"carry_reasoning"),"TUI store sees preferences saved through WebUI RPC");
  App other(workspace,{},home,options,true);
  auto provider_one=app.rpc(obj({{"op",str("provider_save")},{"name",str("first")},{"base_url",str("http://127.0.0.1:9")},{"api",str("chat")},{"models",str("fixture")}}));
  other.rpc(obj({{"op",str("provider_save")},{"name",str("second")},{"base_url",str("http://127.0.0.1:9")},{"api",str("chat")},{"models",str("fixture")}}));
  auto providers=app.rpc(obj({{"op",str("providers")}}));
  check(field(providers,"providers",arr()).items.size()==4,"Provider editors reload saved registry instead of losing other interface changes");
  {auto lease=lock_provider_registry(home);bool busy=false;try{app.rpc(obj({{"op",str("provider_use")},{"id",field(provider_one,"id")}}));}catch(...){busy=true;}check(busy,"Provider writes respect cross-interface lease");}
  Session history;history.items={R"({"type":"codex_reasoning","text":"returned reasoning"})",R"({"role":"assistant","content":"answer"})",R"({"role":"user","content":"next"})"};
  auto automatic=chat_history(history,"test"),carried=chat_history(history,"test",true);
  check(!json_string_field(automatic[1],"reasoning_content")&&json_string_field(carried[1],"reasoning_content")==std::optional<std::string>("returned reasoning")&&!json_string_field(carried[2],"reasoning_content"),"Optional reasoning attaches only to its returned assistant reply");
  history.items={R"({"type":"codex_reasoning","text":"tool reasoning"})",R"({"role":"assistant","content":"preamble"})",R"({"type":"function_call","name":"read_file","call_id":"r1","arguments":"{}"})",R"({"type":"function_call_output","call_id":"r1","output":"ok"})"};
  automatic=chat_history(history,"test",false);
  check(json_string_field(automatic[2],"reasoning_content")==std::optional<std::string>("tool reasoning"),"Required tool reasoning survives an assistant preamble with optional carry disabled");
  history.items={R"({"type":"codex_reasoning","text":"abandoned response"})",R"({"role":"user","content":[{"type":"input_text","text":"new image question"},{"type":"input_image","image_url":"data:image/png;base64,eA=="}]})",R"({"role":"assistant","content":"new answer without reasoning"})"};
  carried=chat_history(history,"test",true);
  check(!json_string_field(carried.back(),"reasoning_content"),"New image user turn clears reasoning from an interrupted response");
  check(find_slash_command("/settings")!=nullptr,"Settings appears in TUI slash command registry");
}
