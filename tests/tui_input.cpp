// PTY fixture: local input only, no model requests or clipboard access.
#define main codex_runtime_main
#include "../codex.cpp"
#undef main
int main(int argc,char** argv){if(argc!=3)return 1;const fs::path root=argv[2];const auto mode=std::string(argv[1]);if(mode.rfind("plugin",0)==0){Plugin p;p.manifest=*parse_json(R"({"api":1,"id":"fixture","version":"1","commands":{"run":{"description":"PLUGIN_PICKER","prompt":"fixture"}},"ui":["panel","status","card"],"actions":[{"label":"Run","command":"run"}]})");if(!PluginRegistry::validate(p))return 2;p.enabled=true;plugins.entries.push_back(p);}
  TuiRenderer tui(root.string(),"custom","mock","read-only");auto feed=[&](const std::string& keys){return tui.receive_busy_input(keys.data(),keys.size());};
  if(mode.rfind("plugin",0)==0){tui.plugin_ui("fixture","panel","PLUGIN_PANEL");tui.plugin_ui("fixture","status","PLUGIN_STATUS");tui.plugin_ui("fixture","card","PLUGIN_CARD");}
  std::string result;const auto initial=slurp(root/"input");
  if(mode=="busy"){feed(initial);result=tui.take_queued();if(result.empty()){auto messages=tui.take_steering();if(!messages.empty())result=messages.front();}}
  else if(mode=="editor"){feed(initial);feed("\x1b[<0;5;21MEDIT ");feed("\r");auto messages=tui.take_steering();if(!messages.empty())result=messages.front();}
  else{if(mode=="transition")feed(initial);std::vector<std::string> history;if(mode=="history")history.push_back(initial);result=tui.read_line(history);}
  std::ofstream(root/"result")<<result;
}
