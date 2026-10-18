// Independent UI process: data-only frames; authority stays with the host.
class FrontendBridge{public:
  ManagedProcess process;TuiRenderer& renderer;std::string id,buffer,error;bool active=false,initialized=false,busy=false,cancel=false,quit=false,switch_builtin=false,pumping=false;
  size_t sequence=0;std::vector<std::pair<std::string,std::string>> actions,steering;
  std::function<std::string(const std::string&,const Json&)> dispatch;
  std::function<std::string()> state;
  size_t width=0,height=0;std::optional<std::vector<std::string>> pending_rows;size_t pending_row=0,pending_column=0;std::chrono::steady_clock::time_point last_frame{};
  explicit FrontendBridge(TuiRenderer& tui):renderer(tui){}~FrontendBridge(){stop();}
  void stop(){active=false;process.stop();renderer.external_mode("",false);}
  static std::string clean(std::string text){text.erase(std::remove_if(text.begin(),text.end(),[](unsigned char c){return c<32||c==127;}),text.end());return text;}
  bool start(const Plugin& plugin){id=plugin.id;auto definition=plugin.manifest.get("frontend");if(!definition){error="plugin has no frontend";return false;}
    std::vector<std::string> command={plugin_string(*definition,"command")};if(auto args=definition->get("args"))for(const auto& arg:args->items)command.push_back(arg.str());
    std::map<std::string,std::string> variables={{"${plugin}",plugin.dir.string()}};for(const auto&[key,value]:plugin.settings)variables["${config."+key+"}"]=value;
    for(auto& arg:command)for(const auto&[key,value]:variables){size_t pos=0;while((pos=arg.find(key,pos))!=std::string::npos){arg.replace(pos,key.size(),value);pos+=value.size();}}
    ProcessPolicy policy;policy.role=ProcessRole::Frontend;policy.reads={plugin.dir};const auto storage=plugin_home()/"frontend-state"/id;std::error_code ec;fs::create_directories(storage,ec);
    if(ec||fs::is_symlink(storage,ec)||fs::is_symlink(storage.parent_path(),ec)||!within_root(plugin_home(),storage)){error="unsafe frontend state directory";return false;}policy.writes.push_back(storage);
    if(!process.start(command,policy,error,false))return false;active=true;renderer.external_mode(id,true);return true;
  }
  bool send(const std::string& frame){if(!active)return false;std::string why;if(frame.size()>65536||!process.send(frame+"\n",why,1)){error=why.empty()?"frontend outbound frame too large":why;cancel=true;stop();return false;}return true;}
  void notify(const std::string& method,const std::string& params){send("{\"jsonrpc\":\"2.0\",\"method\":\""+json_escape(method)+"\",\"params\":"+params+"}");}
  void event(const std::string& kind,const std::string& payload){if(!active||!initialized)return;++sequence;const auto data=payload.size()<48000?payload:"{\"truncated\":true}";
    notify("session.event","{\"seq\":"+std::to_string(sequence)+",\"kind\":\""+json_escape(kind)+"\",\"data\":"+data+"}");}
  void reply(const Json* request,const std::string& result,bool failed=false){if(!request)return;const auto rid=json_dump(*request);if(rid.size()>128){error="invalid RPC id";cancel=true;stop();return;}
    send("{\"jsonrpc\":\"2.0\",\"id\":"+rid+(failed?",\"error\":{\"code\":-32602,\"message\":\""+json_escape(result)+"\"}":",\"result\":"+result)+"}");}
  bool frame(const Json& params){auto rows=params.get("rows");if(!rows||rows->kind!=Json::Array||rows->items.size()>200)return false;std::vector<std::string> lines;
    const std::map<std::string,std::string> colors={{"black","30"},{"red","31"},{"green","32"},{"yellow","33"},{"blue","34"},{"magenta","35"},{"cyan","36"},{"white","37"},{"gray","90"}};
    for(const auto& row:rows->items){std::string line;if(row.kind==Json::String)line=clean(row.value);else{auto spans=row.get("spans");if(!spans||spans->kind!=Json::Array||spans->items.size()>128)return false;
      for(const auto& span:spans->items){auto text=span.get("text");if(!text||text->kind!=Json::String)return false;const auto color=colors.find(plugin_string(span,"fg"));if(color!=colors.end())line+="\x1b["+color->second+"m";
        if(auto bold=span.get("bold");bold&&bold->value=="true")line+="\x1b[1m";line+=clean(text->value)+"\x1b[0m";}}
      lines.push_back(std::move(line));}
    size_t row=0,column=0;if(auto cursor=params.get("cursor")){try{auto r=cursor->get("row"),c=cursor->get("column");if(!r||!c||r->kind!=Json::Other||c->kind!=Json::Other)return false;row=static_cast<size_t>(std::stoul(r->value));column=static_cast<size_t>(std::stoul(c->value));}catch(...){return false;}}
    pending_rows=std::move(lines);pending_row=row;pending_column=column;return true;
  }
  void pump(int timeout=0){if(!active||pumping)return;struct Scope{bool& flag;explicit Scope(bool& f):flag(f){flag=true;}~Scope(){flag=false;}} guard(pumping);
    for(int i=0;i<32&&active;++i){const auto newline=buffer.find('\n');if(buffer.size()>65536){error="frontend inbound frame exceeds 64 KiB";cancel=true;stop();break;}
      if(newline==std::string::npos){char bytes[4096];const int n=process.receive(bytes,sizeof bytes,i==0?timeout:0);if(n==-1||n==-2){error="frontend disconnected";cancel=true;stop();break;}if(!n)break;buffer.append(bytes,static_cast<size_t>(n));continue;}
      auto message=plugin_json(buffer.substr(0,newline));buffer.erase(0,newline+1);if(!message||message->kind!=Json::Object){error="invalid frontend JSON-RPC frame";cancel=true;stop();break;}
      const auto request=message->get("id");const auto method=plugin_string(*message,"method");auto params=message->get("params");Json empty;empty.kind=Json::Object;if(!params)params=&empty;
      if(plugin_string(*message,"jsonrpc")!="2.0"||params->kind!=Json::Object||(request&&request->kind!=Json::String&&request->kind!=Json::Other)){reply(request,"invalid JSON-RPC request",true);continue;}
      if(method=="initialize"){auto protocol=params->get("protocol");if(initialized||!protocol||protocol->value!="1"){reply(request,"frontend protocol 1 required",true);continue;}initialized=true;reply(request,"{\"protocol\":1,\"state\":"+(state?state():"{}")+"}");continue;}
      if(!initialized){reply(request,"initialize first",true);continue;}
      if(method=="ui.frame"){const bool ok=frame(*params);reply(request,ok?"true":"invalid frame",!ok);continue;}
      if(!request)continue;
      if(method=="session.get"){reply(request,state?state():"{}");continue;}
      if(method=="session.cancel"){cancel=busy;reply(request,"true");continue;}
      if(method=="session.send"||method=="session.queue"||method=="session.steer"){const auto text=plugin_string(*params,"text");if(text.empty()||text.size()>48000){reply(request,"text must be 1..48000 bytes",true);continue;}
        if(method=="session.steer"&&busy){if(steering.size()>=16){reply(request,"steering queue full",true);continue;}steering.emplace_back("steer",text);}else{if(actions.size()>=16){reply(request,"turn queue full",true);continue;}actions.emplace_back("send",text);}reply(request,"{\"accepted\":true}");continue;}
      if(method=="session.close"){quit=true;cancel=busy;reply(request,"true");continue;}
      if(busy){reply(request,"session busy",true);continue;}
      if(dispatch){try{reply(request,dispatch(method,*params));}catch(const std::exception& why){reply(request,why.what(),true);}}else reply(request,"method not available",true);
    }
    if(!active)return;if(pending_rows&&std::chrono::steady_clock::now()-last_frame>=std::chrono::milliseconds(33)){renderer.external_frame(std::move(*pending_rows),pending_row,pending_column);pending_rows.reset();last_frame=std::chrono::steady_clock::now();}
    const auto columns=terminal_columns(),rows=terminal_rows();if(width!=columns||height!=rows){width=columns;height=rows;notify("ui.viewport","{\"columns\":"+std::to_string(width)+",\"rows\":"+std::to_string(height>2?height-2:0)+"}");renderer.render_idle();}
    for(int i=0;i<16&&active;++i){auto key=renderer.external_key();if(!key)break;
      if(key->first=="host.stop"){if(busy)cancel=true;else quit=true;continue;}
      if(key->first=="host.menu"){const int choice=renderer.choose("Trusted host · "+id,{"Continue frontend","Return to built-in TUI","Stop session"},0);if(choice==1){switch_builtin=true;cancel=busy;stop();}if(choice==2){quit=true;cancel=busy;}continue;}
      if(key->second.size()>48000){renderer.choose("Paste exceeds frontend 48 KiB limit",{"Continue"});continue;}
      if((key->first=="click"||key->first=="hover")&&key->second.find(',')!=std::string::npos){const auto row=std::stoul(key->second);if(row>=height-std::min<size_t>(2,height))continue;}
      notify("ui.input","{\"key\":\""+key->first+"\",\"text\":\""+json_escape(key->second)+"\"}");
    }
  }
};
