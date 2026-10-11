// PTY fixture: local input only, no model requests or clipboard access.
#define main codex_runtime_main
#include "../codex.cpp"
#undef main
int main(int argc,char** argv){if(argc!=3)return 1;const fs::path root=argv[2];TuiRenderer tui(root.string(),"custom","mock","read-only");const auto mode=std::string(argv[1]);auto feed=[&](const std::string& keys){return tui.receive_busy_input(keys.data(),keys.size());};
  std::string result;const auto initial=slurp(root/"input");
  if(mode=="busy"){feed(initial);result=tui.take_queued();if(result.empty()){auto messages=tui.take_steering();if(!messages.empty())result=messages.front();}}
  else if(mode=="editor"){feed(initial);feed("\x1b[<0;5;21MEDIT ");feed("\r");auto messages=tui.take_steering();if(!messages.empty())result=messages.front();}
  else{if(mode=="transition")feed(initial);std::vector<std::string> history;if(mode=="history")history.push_back(initial);result=tui.read_line(history);}
  std::ofstream(root/"result")<<result;
}
