// Process-local capabilities: owned by this tool instance, never persisted or
// shared across Agent processes. Destruction stops workers before deleting files.
class PreviewSessionTool final:public ToolRuntime {
  using Clock=std::chrono::steady_clock;
  struct Session {
    fs::path root,scratch;ManagedProcess process;int width=1280,height=800,sequence=0;size_t bytes=0,logs=0;
    Clock::time_point opened=Clock::now(),touched=opened;
    ~Session(){process.stop();std::error_code ec;fs::remove_all(scratch,ec);}
  };
  std::map<std::string,std::unique_ptr<Session>> sessions;
  static int integer(const Json& request,const char* key,int fallback,int low,int high){
    auto p=request.get(key);if(!p)return fallback;if(p->kind!=Json::Other)throw std::runtime_error("Expected integer: "+std::string(key));size_t used=0;int n=std::stoi(p->value,&used);if(used!=p->value.size()||n<low||n>high)throw std::runtime_error("Session value outside bounds: "+std::string(key));return n;
  }
  static void await(Session& s,int sequence,const std::function<void()>& tick,bool watch){
    auto deadline=Clock::now()+std::chrono::seconds(25),next_tick=Clock::now();TerminalInputScope input(watch);
    for(;;){
      if(watch&&busy_input&&busy_input("",0))throw std::runtime_error("Session interrupted");
#if !defined(_WIN32)
      if(tui_interrupt_requested)throw std::runtime_error("Session interrupted");
#endif
      if(Clock::now()>=deadline)throw std::runtime_error("Session action timed out");
      if(tick&&Clock::now()>=next_tick){tick();next_tick=Clock::now()+std::chrono::seconds(1);}
      char bytes[4096];const int n=s.process.receive(bytes,sizeof bytes,20);if(n>0){s.logs+=static_cast<size_t>(n);if(s.logs>200000)throw std::runtime_error("Session console output budget exceeded");}
      auto reply=s.scratch/"reply";std::error_code ec;
      if(fs::exists(reply,ec)){
        if(!fs::is_regular_file(fs::symlink_status(reply))||fs::hard_link_count(reply)!=1||fs::file_size(reply)>32)throw std::runtime_error("Invalid session reply artifact");
        std::ifstream stream(reply);int received=-1,status=-1;std::string extra;if(!(stream>>received>>status)||stream>>extra||received!=sequence||(status!=0&&status!=1))throw std::runtime_error("Invalid session acknowledgement");
        fs::remove(reply);if(!status)throw std::runtime_error("Session operation failed; focused control may be absent, disabled or unsupported");return;
      }
      if(n<0||s.process.wait(0)>=0)throw std::runtime_error("Session worker exited (expired, crashed or resource limit); open a new session");
    }
  }
  ToolResult perform(const std::string& args,const fs::path& root,const std::function<void()>& tick,bool watch){
    std::string token;try{
      auto request=parse_json(args);if(!request||request->kind!=Json::Object)throw std::runtime_error("Invalid session arguments");
      auto field=[&](const char* name){auto p=request->get(name);if(!p)return std::string{};if(p->kind!=Json::String)throw std::runtime_error("Expected string: "+std::string(name));return p->value;};
      const auto operation=field("operation");token=field("session");const auto owner=fs::canonical(root);const auto now=Clock::now();
      for(auto it=sessions.begin();it!=sessions.end();){if(it->second->process.wait(0)>=0||now-it->second->touched>=std::chrono::seconds(60)||now-it->second->opened>=std::chrono::seconds(300))it=sessions.erase(it);else ++it;}
      if(operation=="open"){
        if(!token.empty())throw std::runtime_error("Open requires an empty session token");
        if(execution.unsafe||!sandbox_available())return {126,"Preview sessions require an OS sandbox; no unsafe fallback. Windows restricted execution is not supported."};
        if(sessions.size()>=2)throw std::runtime_error("At most two preview sessions; close an existing session first");
        const auto path=field("path");auto page=fs::weakly_canonical(owner/fs::u8path(path));if(path.empty()||!within_root(owner,page)||sensitive_path(page)||!fs::is_regular_file(page))return {126,"Preview path outside workspace, protected or missing"};
        auto s=std::make_unique<Session>();s->root=owner;s->width=integer(*request,"width",1280,64,1920);s->height=integer(*request,"height",800,64,1920);int wait=integer(*request,"wait_ms",500,0,3000);
        if(execution.private_dir.empty())execution.initialize(root);
        s->scratch=execution.private_dir/random_id("preview-session");fs::create_directory(s->scratch);fs::permissions(s->scratch,fs::perms::owner_all,fs::perm_options::replace);
        auto policy=ProcessPolicy::tool(owner,false);policy.no_fork=true;policy.writes.push_back(s->scratch);policy.denies.push_back(own_auth_path().parent_path());const auto home=fs::path(getenv_or("HOME",getenv_or("USERPROFILE")));for(const auto& secret:{home/".ssh",home/".aws",home/".codex",home/"Library/Keychains"})policy.denies.push_back(secret);policy.reads.push_back("/Library/Fonts");policy.reads.push_back("/System/Library/Fonts");
        std::string error;if(!s->process.start({preview_self.u8string(),"--miniweb-session-worker",owner.u8string(),page.u8string(),s->scratch.u8string(),std::to_string(s->width),std::to_string(s->height),std::to_string(wait)},policy,error,true))return {126,error};
        await(*s,0,tick,watch);token=random_id("preview");sessions.emplace(token,std::move(s));
        return {0,"Opened preview session "+token+". DOM/JS state persists in this Agent process. Idle limit 60s, lifetime 300s, 128 actions. Local files only; no scrolling or navigation. Close when finished."};
      }
      auto it=sessions.find(token);if(it==sessions.end()||it->second->root!=owner)return {2,"Unknown, expired or foreign preview session; open a new session"};
      if(operation=="close"){sessions.erase(it);return {0,"Preview session closed"};}
      auto& s=*it->second;std::string frame=std::to_string(s.sequence+1)+" "+operation;
      if(operation=="click")frame+=" "+std::to_string(integer(*request,"x",-1,0,s.width-1))+" "+std::to_string(integer(*request,"y",-1,0,s.height-1));
      else if(operation=="type"||operation=="key"){
        const auto text=field("value");if(text.empty()||text.size()>8192)throw std::runtime_error("Text/key must contain 1–8192 UTF-8 bytes");
        static const char* hex="0123456789abcdef";frame+=' ';for(unsigned char c:text){frame+=hex[c>>4];frame+=hex[c&15];}
      }else if(operation=="wait")frame+=" "+std::to_string(integer(*request,"wait_ms",0,0,3000));
      else if(operation!="screenshot")throw std::runtime_error("Unknown preview session operation");
      if(s.sequence>=128||s.bytes+frame.size()>128*1024)throw std::runtime_error("Session cumulative action/input budget exceeded");
      ++s.sequence;s.bytes+=frame.size();s.touched=now;std::string error;
      if(!s.process.send(frame+"\n",error))throw std::runtime_error(error);
      await(s,s.sequence,tick,watch);
      if(operation!="screenshot")return {0,"Preview session action completed: "+operation};
      const auto output=s.scratch/"screenshot.png";std::error_code ec;if(!fs::is_regular_file(fs::symlink_status(output,ec))||ec||fs::hard_link_count(output)!=1)throw std::runtime_error("Invalid session image artifact");
      std::string why;auto image=image_data_url(output,why);if(!image)throw std::runtime_error(why);auto metadata=preview_metadata(s.scratch/"screenshot.png.report.json");
      return {0,metadata.first+" Experimental MiniWeb persistent session; not Chromium verification.",{*image}};
    }catch(const std::exception& e){const bool closed=!token.empty()&&sessions.erase(token)>0;return {std::string(e.what())=="Session interrupted"?130:2,std::string("Preview session: ")+e.what()+(closed?"; session closed":"")};}
  }
public:
  std::string name()const override{return "preview_session";}
  std::string spec_json()const override{return R"({"type":"function","name":"preview_session","description":"Persistent isolated MiniWeb page. open a local HTML path, then click coordinates, type into focused input/textarea, key, wait (virtual milliseconds), screenshot, close. Use token returned by open. State persists only within this Agent process; not across task restarts. No network, scroll, navigation or full browser compatibility. Text inputs/textarea, checkbox and buttons only; no native select, radio-group, IME or rich editor interaction. Keys: Tab, Shift+Tab, Enter, Space as a literal space, Backspace, Delete, ArrowLeft, ArrowRight, Home, End, Escape. Type emits beforeinput/input; blur emits change; submit events supported, actual form navigation blocked. Set unused string fields empty, coordinates zero, dimensions 1280x800. Two sessions max, idle 60s/lifetime 300s/128 actions; close promptly.","parameters":{"type":"object","properties":{"operation":{"type":"string","enum":["open","click","type","key","wait","screenshot","close"]},"session":{"type":"string"},"path":{"type":"string"},"value":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},"wait_ms":{"type":"integer"}},"required":["operation","session","path","value","x","y","width","height","wait_ms"],"additionalProperties":false},"strict":true})";}
  ToolResult run(const std::string& args,const fs::path& root)override{return perform(args,root,{},false);}
  ToolResult run_with_ui(const std::string& args,const fs::path& root,const std::function<void()>& tick,bool watch)override{return perform(args,root,tick,watch);}
};
