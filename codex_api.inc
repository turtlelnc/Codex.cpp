// Included by codex.cpp after its shared types and helpers.
// ---------- API client ----------
enum class ApiStyle{Responses,ChatCompletions};static const char* api_style_name(ApiStyle s){return s == ApiStyle::Responses ? "responses" : "chat"; }
struct ApiClient{std::string api_key;std::string base_url = "https://api.openai.com";std::string model = "gpt-5.6-sol";std::string reasoning_effort;
ApiStyle style = ApiStyle::Responses;bool no_api_key = false;bool chatgpt_auth = false;
std::optional<std::string> post_codex(const std::string& body,std::string& err,const std::function<void(const std::string&)>& on_delta ={}, const std::function<void()>& on_tick ={},bool watch_escape = false)const{
  auto a=load_auth();if(!a){err="Not signed in. Run --provider codex --login.";return{};}if(a->expires<=epoch_now()+90 && !refresh_auth(*a,err))return{};
  auto request=[&](){WebReply result;const fs::path req=temp_path("codex-stream-req"),hdr=temp_path("codex-stream-hdr"), stderr_path=temp_path("codex-stream-stderr");
  {std::ofstream f(req,std::ios::binary);f<<body;std::ofstream h(hdr,std::ios::binary); h<<"Authorization: Bearer "<<a->access<<"\nAccept: text/event-stream\nContent-Type: application/json\n";
    if(!a->account.empty())h<<"ChatGPT-Account-ID: "<<a->account<<"\n";
  }
  const std::string marker="__CODEX_HTTP_"+random_id("status")+"__:";std::string cmd="curl -sS -N --connect-timeout 20 --max-time 300 --max-redirs 0"+
    std::string(" -w ")+shell_quote("\n"+marker+"%{http_code}\n")+ " -H "+shell_quote("@"+hdr.string())+" --data-binary @"+shell_quote(req.string())+
    " "+shell_quote("https://chatgpt.com/backend-api/codex/responses")+ " 2>"+shell_quote(stderr_path.string());
#if defined(_WIN32)
  FILE* pipe=_popen(cmd.c_str(),"r");
#else
  int channels[2]={-1,-1};pid_t child=-1;if(::pipe(channels)==0){child=::fork();if(child==0){
    ::setsid();// own process group, so Esc stops the shell and curl
    ::close(channels[0]);::dup2(channels[1],STDOUT_FILENO);::close(channels[1]);::execl("/bin/sh","sh","-c",cmd.c_str(),static_cast<char*>(nullptr)); _exit(127); }
    ::close(channels[1]);if(child<0){::close(channels[0]);channels[0]=-1;}
  }
#endif
#if defined(_WIN32)
  if(!pipe)result.error="could not start curl";
#else
  if(child<0 || channels[0]<0)result.error="could not start curl";
#endif
  else{std::array<char,4096> buf{};std::string line;auto consume_line=[&](){if(line.rfind(marker,0)==0){ try{result.status=std::stol(line.substr(marker.size()));}catch(...){}
    }else{
      if(result.body.size()+line.size()<=8'000'000)result.body+=line;else result.error="Codex stream exceeded 8 MB"; if(result.error.empty()&& line.rfind("data: ",0)==0 && on_delta){auto event=parse_json(line.substr(6));
      if(event && at(&*event,"type")&& at(&*event,"type")->str()=="response.output_text.delta"){auto delta=event->get("delta"); if(delta && delta->kind==Json::String)on_delta(delta->value);
      }
      }
    }
    line.clear(); }; auto consume_bytes=[&](const char* data,size_t count){for(size_t i=0;i<count;++i){line+=data[i];if(data[i]=='\n')consume_line();
      else if(line.size()>1'000'000){result.error="Codex stream line exceeded 1 MB";line.clear(); }
    }
    };
#if defined(_WIN32)
    while(fgets(buf.data(),static_cast<int>(buf.size()),pipe))consume_bytes(buf.data(),std::strlen(buf.data()));
#else
    termios saved_terminal{};bool raw_input=false;if(watch_escape && isatty(STDIN_FILENO)&& tcgetattr(STDIN_FILENO,&saved_terminal)==0){
    termios raw=saved_terminal;raw.c_lflag &= static_cast<tcflag_t>(~(ICANON|ECHO));raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0; raw_input=tcsetattr(STDIN_FILENO,TCSANOW,&raw)==0; }
    auto last_tick=std::chrono::steady_clock::now();const int fd=channels[0];bool interrupted=false;for(;;){
#if !defined(_WIN32)
    if(tui_interrupt_requested){interrupted=true;result.error="Interrupted by Ctrl+C";break;}
#endif
    fd_set readers;FD_ZERO(&readers);FD_SET(fd,&readers);if(raw_input)FD_SET(STDIN_FILENO,&readers);timeval timeout{0,250000};
    int ready=select(std::max(fd,raw_input?STDIN_FILENO:fd)+1,&readers,nullptr,nullptr,&timeout);if(ready>0){
      if(raw_input && FD_ISSET(STDIN_FILENO,&readers)){char keys[64];ssize_t count=::read(STDIN_FILENO,keys,sizeof(keys));
      if(count>0 && contains_plain_escape(keys,count)){interrupted=true;result.error="Interrupted by Esc";break; } }
      if(FD_ISSET(fd,&readers)){ssize_t n=::read(fd,buf.data(),buf.size());if(n>0)consume_bytes(buf.data(),static_cast<size_t>(n));else if(n==0)break; else if(errno!=EINTR){result.error="stream read failed";break;} }
    }else if(ready<0 && errno!=EINTR){result.error="stream wait failed";break;}
    auto now=std::chrono::steady_clock::now();if(on_tick && now-last_tick>=std::chrono::seconds(1)){on_tick();last_tick=now; } } if(raw_input)tcsetattr(STDIN_FILENO,TCSANOW,&saved_terminal);if(interrupted){
    // The forked shell has its own session. Kill its whole group,
    // including curl, before waiting so pclose-like hangs cannot occur.
    if(::getpgid(child)==child)::kill(-child,SIGTERM);else::kill(child,SIGTERM); } ::close(fd);
#endif
    if(!line.empty()&& result.error.empty())consume_line();
#if defined(_WIN32)
    int status=_pclose(pipe);if(status!=0 && result.error.empty())result.error="curl failed: "+trim(slurp(stderr_path,2000));
#else
    int status=0;while(::waitpid(child,&status,0)<0 && errno==EINTR){}if((!WIFEXITED(status)||WEXITSTATUS(status)!=0)&& result.error.empty()) result.error="curl failed: "+trim(slurp(stderr_path,2000));
#endif
  }
  std::error_code ec;fs::remove(req,ec);fs::remove(hdr,ec);fs::remove(stderr_path,ec);return result; };
  auto r=request();if(r.error=="Interrupted by Esc"){err=r.error;return{};}if(r.status==401 && refresh_auth(*a,err))r=request();
  if(!r.error.empty()|| r.status!=200){err="Codex backend request failed (HTTP "+std::to_string(r.status)+"): "+r.error;
  // Avoid placing backend error bodies in logs: they may contain tokens.
  return{}; } return r.body;
}
std::optional<ApiResponse> parse_codex_stream(const std::string& raw,std::string& err)const{std::map<std::string,Json> done;
  std::string id;bool completed=false;std::istringstream lines(raw);std::string line;while(std::getline(lines,line)){if(line.rfind("data: ",0)!=0)continue;
  auto evt=parse_json(line.substr(6));if(!evt)continue;auto type=evt->get("type");if(!type)continue;if(type->str()=="response.output_item.done"){
    auto item=evt->get("item");if(!item)continue;auto idx=evt->get("output_index");auto key=idx?idx->value:std::to_string(done.size());done[key]=*item;
  }else if(type->str()=="response.completed"){
    completed=true;if(auto resp=evt->get("response")){if(auto x=resp->get("id"))id=x->str();if(auto out=resp->get("output");out&&out->kind==Json::Array)
      for(size_t i=0;i<out->items.size();++i)done[std::to_string(i)]=out->items[i];
    }
  }else if(type->str()=="response.failed"){
    err="Codex backend reported a failed response";return{};
  }
  }
  if(!completed){err="Codex stream ended before response.completed";return{};}std::string aggregate="{\"id\":\""+json_escape(id)+"\",\"output\":[";
  bool first=true;for(const auto&[_,item]:done){if(!first)aggregate+=',';first=false;aggregate+=json_dump(item);}aggregate+="]}"; return parse_api_response(aggregate,false,err);
}
std::optional<std::string> post(const std::string& path,const std::string& body,std::string& err)const{
  if(chatgpt_auth){err = "internal routing error: ChatGPT login request reached API Key transport";return std::nullopt; }
  const fs::path req = temp_path("codex-cpp-request");const fs::path out = temp_path("codex-cpp-response"); const fs::path header = temp_path("codex-cpp-header");
  {std::ofstream f(req,std::ios::binary);if(!f){err = "cannot create request temp file";return std::nullopt;}f << body;}
  std::string cmd = "curl -sS --fail-with-body --connect-timeout 20 --max-time 300 -o " + shell_quote(out.string());if(!no_api_key){
  if(api_key.empty()){std::error_code ec;fs::remove(req,ec);err = "API key is not set";return std::nullopt;} {std::ofstream f(header,std::ios::binary);f << "Authorization: Bearer " << api_key << "\n";}
#if !defined(_WIN32)
  fs::permissions(header,fs::perms::owner_read | fs::perms::owner_write,fs::perm_options::replace);
#endif
  cmd += " -H " + shell_quote("@" + header.string()); } cmd += " -H " + shell_quote("Content-Type: application/json")+ " --data-binary @" + shell_quote(req.string())+ " " + shell_quote(base_url + path);
  CommandResult cr = run_capture_interruptible(cmd,100000,{},false);const std::string raw = slurp(out,2'000'000); std::error_code ec;fs::remove(req,ec);fs::remove(out,ec);fs::remove(header,ec);if(cr.exit_code != 0){
  err = "HTTP request failed (curl " + std::to_string(cr.exit_code)+ "): " + cr.output;if(!raw.empty())err += "\n" + raw;return std::nullopt; } return raw;
}
std::optional<ApiResponse> call_text_only(const std::string& instructions,const std::string& text,std::string& err)const{
  std::string body;std::string path;if(style == ApiStyle::Responses){body = "{\"model\":\"" + json_escape(model)+ "\",\"instructions\":\"" + json_escape(instructions)+ "\",\"input\":\"" + json_escape(text)+ "\"";
  if(!reasoning_effort.empty())body += ",\"reasoning\":{\"effort\":\"" + json_escape(reasoning_effort)+ "\"}"; body += "}";path = "/v1/responses"; }else{
  body = "{\"model\":\"" + json_escape(model)+ "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(instructions)+ "\"},{\"role\":\"user\",\"content\":\"" + json_escape(text)+ "\"}],\"stream\":false}";
  path = "/chat/completions"; } if(chatgpt_auth){body="{\"model\":\""+json_escape(model)+"\",\"instructions\":\""+json_escape(instructions)+
    "\",\"input\":[{\"role\":\"user\",\"content\":\""+json_escape(text)+ "\"}],\"stream\":true,\"store\":false}";
  auto stream=post_codex(body,err);return stream?parse_codex_stream(*stream,err):std::nullopt; }
  auto raw=post(path,body,err);if(!raw)return std::nullopt;auto a=parse_api_response(*raw,style==ApiStyle::ChatCompletions,err);
  if(!a || !a->text){if(err.empty())err="Text-only response missing text";return std::nullopt;}return a;
}
std::optional<ApiResponse> call_responses(const std::string& instructions,const std::string& input_json,const std::string& tools_json,std::string& err,
        const std::optional<std::string>& previous_response_id = std::nullopt,const std::function<void(const std::string&)>& on_delta ={}, const std::function<void()>& on_tick ={},bool watch_escape = false)const{
  std::string body = "{\"model\":\"" + json_escape(model)+ "\",\"instructions\":\"" + json_escape(instructions)+ "\",\"input\":" + input_json +
  ",\"tools\":" + tools_json + ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":false"; if(!reasoning_effort.empty())body += ",\"reasoning\":{\"effort\":\"" + json_escape(reasoning_effort)+ "\"}";
  if(chatgpt_auth)body += ",\"stream\":true,\"store\":false";else if(previous_response_id && !previous_response_id->empty()) body += ",\"previous_response_id\":\"" + json_escape(*previous_response_id)+ "\"";
  body += "}";if(chatgpt_auth){auto stream = post_codex(body,err,on_delta,on_tick,watch_escape); return stream ? parse_codex_stream(*stream,err): std::nullopt; }
  auto raw = post("/v1/responses",body,err);if(!raw)return std::nullopt;return parse_api_response(*raw,false,err);
}
std::optional<ApiResponse> call_chat(const std::string& messages_json,const std::string& tools_json,std::string& err)const{ if(chatgpt_auth){err = "ChatGPT login requires the Responses backend";return std::nullopt; }
  std::string body = "{\"model\":\"" + json_escape(model)+ "\",\"messages\":" + messages_json + ",\"tools\":" + tools_json + ",\"tool_choice\":\"auto\",\"stream\":false}";
  auto raw = post("/chat/completions",body,err);if(!raw)return std::nullopt;return parse_api_response(*raw,true,err);
}
};
