// Included by codex.cpp after its shared types and helpers.
// ---------- API client ----------
enum class ApiStyle{Responses,ChatCompletions};static const char* api_style_name(ApiStyle s){return s == ApiStyle::Responses ? "responses" : "chat"; }
struct CodexQuota{bool available=false;long long reset=0;std::string report,brief;};
static std::optional<CodexQuota> parse_codex_quota(const std::string& raw,long long now){auto j=parse_json(raw);if(!j || !j->get("rate_limit"))return{};
CodexQuota q;std::ostringstream out;if(auto plan=j->get("plan_type"))out<<"Plan: "<<plan->str()<<'\n';
auto bucket=[&](const Json* limit,const std::string& name,bool relevant=true){if(!limit || limit->kind!=Json::Object)return;
 const bool reached=at(limit,"limit_reached") && at(limit,"limit_reached")->value=="true";out<<name<<": "<<(reached?"limit reached":"available")<<'\n';
 for(const char* key:{"primary_window","secondary_window"}){auto w=limit->get(key);if(!w || w->kind!=Json::Object)continue;
  try{auto used=w->get("used_percent"),reset=w->get("reset_at"),span=w->get("limit_window_seconds");
   if(!used || !reset)continue;const auto stamp=std::stoll(reset->value);out<<"  "<<(span?std::to_string(std::stoll(span->value)/3600)+"h":std::string(key))<<": "<<used->value<<"% used; resets in "<<std::max(0LL,stamp-now)/3600<<"h "<<(std::max(0LL,stamp-now)%3600)/60<<"m\n";
   if(relevant){const long long hours=span?std::stoll(span->value)/3600:0;const int left=std::max(0,std::min(100,100-static_cast<int>(std::stod(used->value)+0.5)));
    if(!q.brief.empty())q.brief+=" · ";q.brief+=(hours>=24?std::to_string(hours/24)+"d":hours>0?std::to_string(hours)+"h":std::string(std::string(key)=="primary_window"?"5h":"wk"))+" "+std::to_string(left)+"%";}
   if(relevant && reached && std::stod(used->value)>=100 && stamp>now)q.reset=std::max(q.reset,stamp);
  }catch(...){}
 }
};bucket(j->get("rate_limit"),"Codex");
if(auto extra=j->get("additional_rate_limits");extra && extra->kind==Json::Array)for(const auto& b:extra->items)bucket(b.get("rate_limit"),"Additional quota",false);
auto main=j->get("rate_limit");q.available=at(main,"allowed") && at(main,"allowed")->value=="true" && at(main,"limit_reached") && at(main,"limit_reached")->value=="false";
q.report=out.str();return q; }
static std::optional<CodexQuota> read_codex_quota(std::string& err,bool renew=true){auto a=load_auth();if(!a){err="Not signed in. Run --provider codex --login.";return{};}
if(a->expires<=epoch_now()+90){if(!renew){err="Login token expired; run /status or --quota to refresh";return{};}if(!refresh_auth(*a,err))return{};}
auto request=[&](){std::vector<std::string> h={"Authorization: Bearer "+a->access,"Accept: application/json"};if(!a->account.empty())h.push_back("ChatGPT-Account-ID: "+a->account);
 return web_post("https://chatgpt.com/backend-api/wham/usage","","application/json",h,true);};
auto r=request();if(r.status==401 && renew && refresh_auth(*a,err))r=request();
if(r.status!=200 || !r.error.empty()){err="Quota query failed (HTTP "+std::to_string(r.status)+")";return{};}
auto q=parse_codex_quota(r.body,epoch_now());if(!q)err="Invalid quota response";return q; }
// Incremental SSE decoder; TCP chunks need not coincide with UTF-8 or event boundaries.
struct ChatStreamDecoder{
  ApiResponse result;std::map<int,FunctionCall> calls;std::string line,data,error;bool finished=false,done=false;
  std::function<void(const std::string&)> on_delta;
  void event(){if(data.empty())return;auto payload=std::move(data);data.clear();if(payload=="[DONE]"){done=true;return;}
    auto doc=parse_json(payload);if(!doc){error="Invalid Chat Completions stream event";return;}
    if(doc->get("error")){error="Chat Completions stream reported an API error";return;}
    if(auto id=doc->get("id"))result.id=id->str();auto choices=doc->get("choices");if(!choices || choices->kind!=Json::Array)return;
    for(const auto& choice:choices->items){auto index=choice.get("index");if(index && index->value!="0")continue;
      auto delta=choice.get("delta");if(auto content=at(delta,"content");content && content->kind==Json::String){
        result.text=result.text.value_or("")+content->value;if(on_delta && !content->value.empty())on_delta(content->value);}
      if(auto tools=at(delta,"tool_calls");tools && tools->kind==Json::Array)for(const auto& tool:tools->items){
        auto idx=tool.get("index");if(!idx || idx->kind!=Json::Other){error="Missing streamed tool index";continue;}
        int n;try{n=std::stoi(idx->value);}catch(...){error="Invalid streamed tool index";continue;}
        if(n<0 || n>1024){error="Invalid streamed tool index";continue;}auto& call=calls[n];
        if(auto id=tool.get("id"))call.call_id+=id->str();auto fn=tool.get("function");
        if(auto name=at(fn,"name"))call.name+=name->str();if(auto args=at(fn,"arguments"))call.arguments+=args->str();
      }
      if(auto reason=choice.get("finish_reason");reason && reason->kind==Json::String){
        if(reason->str()=="stop" || reason->str()=="tool_calls")finished=true;else error="Chat Completions stopped: "+reason->str();}
    }
  }
  void feed(const char* bytes,size_t size){for(size_t i=0;i<size;++i){if(bytes[i]!='\n'){line+=bytes[i];if(line.size()>1'000'000)throw std::runtime_error("Chat stream line exceeded 1 MB");continue;}
    if(!line.empty() && line.back()=='\r')line.pop_back();if(line.empty())event();else if(line.rfind("data:",0)==0){auto part=line.substr(5);if(!part.empty()&&part[0]==' ')part.erase(0,1);if(!data.empty())data+='\n';data+=part;}line.clear();}}
  std::optional<ApiResponse> finish(std::string& err){if(!error.empty()){err=error;return{};}if(!done || !finished){err="Chat Completions stream ended before completion";return{};}
    for(const auto& entry:calls){const auto& call=entry.second;if(call.name.empty()||call.call_id.empty()||!parse_json(call.arguments)){err="Incomplete streamed tool call";return{};}result.calls.push_back(call);}
    if(!result.text && result.calls.empty()){err="Chat Completions stream contained neither text nor tool calls";return{};}return result;
  }
};
struct ApiClient{std::string api_key;std::string base_url = "https://api.openai.com";std::string model = "gpt-5.6-sol";std::string reasoning_effort;
ApiStyle style = ApiStyle::Responses;bool no_api_key = false;bool chatgpt_auth = false;bool wait_quota=false;
std::optional<std::string> post_codex(const std::string& body,std::string& err,const std::function<void(const std::string&)>& on_delta ={}, const std::function<void()>& on_tick ={},bool watch_escape = false)const{
  auto a=load_auth();if(!a){err="Not signed in. Run --provider codex --login.";return{};}if(a->expires<=epoch_now()+90 && !refresh_auth(*a,err))return{};
  auto request=[&](){
    std::vector<std::string> headers={"Authorization: Bearer "+a->access,"Accept: text/event-stream","Content-Type: application/json"};
    if(!a->account.empty())headers.push_back("ChatGPT-Account-ID: "+a->account);
    std::string line;
    auto consume=[&](const char* data,size_t size){for(size_t i=0;i<size;++i){line+=data[i];
      if(line.size()>1'000'000)throw std::runtime_error("Codex stream line exceeded 1 MB");
      if(data[i]=='\n'){if(on_delta&&line.rfind("data: ",0)==0){auto event=parse_json(line.substr(6));
        if(event&&at(&*event,"type")&&at(&*event,"type")->str()=="response.output_text.delta"){
          auto delta=event->get("delta");if(delta&&delta->kind==Json::String)on_delta(delta->value);
        }}line.clear();}
    }};
    return native_http("https://chatgpt.com/backend-api/codex/responses",body,headers,false,consume,on_tick,watch_escape);
  };
  auto r=request();for(int retry=0;retry<2 && r.body.find("data:")==std::string::npos &&
    (r.status==429 || r.status==502 || r.status==503 || r.status==504 ||
     (r.status==0 && r.error.rfind("curl failed:",0)==0));++retry){
    std::cerr << "[retry] HTTP " << r.status << "; retrying in " << (retry+1)*2 << " seconds\n";
    for(int tick=0;tick<(retry+1)*20;++tick){
#if !defined(_WIN32)
      if(tui_interrupt_requested){err="Interrupted by Ctrl+C";return{};}
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(100));if(on_tick)on_tick();
    }r=request();
  }
  for(;;){if(!wait_quota || r.status!=429 || r.body.find("data:")!=std::string::npos)break;
    auto q=read_codex_quota(err);if(!q || q->reset<=epoch_now())break;
    auto deadline=q->reset+2;auto next_check=epoch_now()+30;std::cerr<<"[quota] waiting until "<<deadline<<" (Unix seconds); Ctrl+C stops waiting\n";
    while(epoch_now()<deadline){
#if !defined(_WIN32)
      if(tui_interrupt_requested){err="Interrupted while waiting for quota";return{};}
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(100));if(on_tick)on_tick();
      if(epoch_now()>=next_check){std::string query_err;auto live=read_codex_quota(query_err);next_check=epoch_now()+30;
        if(live && live->available){std::cerr<<"[quota] quota restored; continuing\n";break;}
        if(live && live->reset>epoch_now())deadline=live->reset+2;
      }
    }
    a=load_auth();if(!a){err="Not signed in while waiting for quota";return{};}
    if(a->expires<=epoch_now()+90 && !refresh_auth(*a,err))return{};r=request();
  }
  if(r.error=="Interrupted by Esc"){err=r.error;return{};}if(r.status==401 && refresh_auth(*a,err))r=request();
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
  std::vector<std::string> headers={"Content-Type: application/json"};
  if(!no_api_key){if(api_key.empty()){err="API key is not set";return{};}headers.push_back("Authorization: Bearer "+api_key);}
  auto reply=native_http(base_url+path,body,headers,false,{}, {},bool(busy_input));
  if(!reply.error.empty()||reply.status<200||reply.status>=300){err=reply.error.empty()?"HTTP request failed (HTTP "+std::to_string(reply.status)+")":reply.error;return{};}
  return reply.body;
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
std::optional<ApiResponse> call_chat(const std::string& messages_json,const std::string& tools_json,std::string& err,
 const std::function<void(const std::string&)>& on_delta={},const std::function<void()>& on_tick={},bool watch_escape=false)const{
  if(chatgpt_auth){err="ChatGPT login requires the Responses backend";return{};}
  if(!no_api_key && api_key.empty()){err="API key is not set";return{};}
  std::string body="{\"model\":\""+json_escape(model)+"\",\"messages\":"+messages_json+",\"tools\":"+tools_json+",\"tool_choice\":\"auto\",\"stream\":true}";
  std::vector<std::string> headers={"Content-Type: application/json","Accept: text/event-stream"};if(!no_api_key)headers.push_back("Authorization: Bearer "+api_key);
  ChatStreamDecoder decoder;decoder.on_delta=on_delta;
  auto reply=native_http(base_url+"/chat/completions",body,headers,false,[&](const char* data,size_t size){decoder.feed(data,size);},on_tick,watch_escape);
  if(!reply.error.empty()||reply.status<200||reply.status>=300){err=reply.error.empty()?"HTTP request failed (HTTP "+std::to_string(reply.status)+")":reply.error;return{};}
  // Some compatible servers ignore stream=true and return a complete JSON response.
  if(parse_json(reply.body)){auto response=parse_api_response(reply.body,true,err);if(response && response->text && on_delta)on_delta(*response->text);return response;}
  return decoder.finish(err);
}
};
// ---------- model catalog ----------
struct ModelInfo{std::string id,description,default_effort;std::vector<std::string> efforts;};
static std::optional<std::vector<ModelInfo>> parse_codex_models(const std::string& raw){auto j=parse_json(raw);auto list=j?j->get("models"):nullptr;if(!list || list->kind!=Json::Array)return{};
  std::vector<ModelInfo> out;for(const auto& m:list->items){if(m.kind!=Json::Object)continue;ModelInfo info;info.id=at(&m,"slug")?at(&m,"slug")->str():"";
    const std::string visibility=at(&m,"visibility")?at(&m,"visibility")->str():"";if(info.id.empty()|| (!visibility.empty()&& visibility!="list"))continue;
    if(auto d=at(&m,"description"))info.description=d->str();if(auto d=at(&m,"default_reasoning_level"))info.default_effort=d->str();
    if(auto levels=at(&m,"supported_reasoning_levels");levels && levels->kind==Json::Array)for(const auto& l:levels->items)if(auto e=at(&l,"effort");e && !e->str().empty())info.efforts.push_back(e->str());
    out.push_back(std::move(info)); }
  return out; }
// Standard GET /models shape (OpenAI, DeepSeek, most compatible servers). It carries no reasoning-effort data.
static std::optional<std::vector<ModelInfo>> parse_openai_models(const std::string& raw){auto j=parse_json(raw);auto list=j?j->get("data"):nullptr;if(!list || list->kind!=Json::Array)return{};
  static const char* const skip[]={"embedding","whisper","tts","dall-e","moderation","transcribe","audio","realtime","image","davinci","babbage"};std::vector<ModelInfo> out;
  for(const auto& m:list->items){auto id=at(&m,"id");if(!id || id->str().empty())continue;bool drop=false;for(const char* word:skip)if(id->str().find(word)!=std::string::npos)drop=true;
    if(!drop){ModelInfo info;info.id=id->str();out.push_back(std::move(info));} }
  std::sort(out.begin(),out.end(),[](const ModelInfo& a,const ModelInfo& b){return a.id<b.id;});return out; }
static std::optional<std::vector<ModelInfo>> read_codex_models(std::string& err){auto a=load_auth();if(!a){err="Not signed in. Run --provider codex --login.";return{};}
  if(a->expires<=epoch_now()+90 && !refresh_auth(*a,err))return{};
  // This non-public endpoint hides newer models from clients that report an old version.
  const std::string url="https://chatgpt.com/backend-api/codex/models?client_version=1.0.0";
  auto request=[&](){std::vector<std::string> h={"Authorization: Bearer "+a->access,"Accept: application/json"};if(!a->account.empty())h.push_back("ChatGPT-Account-ID: "+a->account);return web_post(url,"","application/json",h,true);};
  auto r=request();if(r.status==401 && refresh_auth(*a,err))r=request();if(r.status!=200 || !r.error.empty()){err="Model list query failed (HTTP "+std::to_string(r.status)+")";return{};}
  auto models=parse_codex_models(r.body);if(!models || models->empty()){err="No models in response";return{};}return models; }
static std::optional<std::vector<ModelInfo>> read_models(const ApiClient& api,std::string& err){if(api.chatgpt_auth)return read_codex_models(err);
  std::vector<std::string> h={"Accept: application/json"};if(!api.no_api_key){if(api.api_key.empty()){err="API key is not set";return{};}h.push_back("Authorization: Bearer "+api.api_key);}
  auto r=web_post(api.base_url+(api.style==ApiStyle::Responses?"/v1/models":"/models"),"","application/json",h,true);
  if(r.status!=200 || !r.error.empty()){err="Model list not available from this provider (HTTP "+std::to_string(r.status)+")";return{};}
  auto models=parse_openai_models(r.body);if(!models || models->empty()){err="No models in response";return{};}return models; }
