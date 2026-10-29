// HTTPS client for the existing hosted API. No account tokens or session cookies
// are persisted, and project permissions remain enforced by HostedService.
struct TeamConnection {
  std::string origin,cookie,csrf,username;
  bool connected()const{return !cookie.empty()&&!csrf.empty();}
  void clear(){std::fill(cookie.begin(),cookie.end(),'\0');cookie.clear();csrf.clear();username.clear();}
  static std::string validate_origin(std::string value){
    while(!value.empty()&&value.back()=='/')value.pop_back();
    native_web::require(value.rfind("https://",0)==0&&value.size()>8&&value.size()<=2048&&
      value.substr(8).find_first_of("/@?#\r\n\t ")==std::string::npos,"Use an HTTPS origin without path or credentials");
    return value;
  }
  native_web::J request(const std::string& path,const native_web::J& body=native_web::obj(),bool get=false){
    using namespace native_web;
    std::vector<std::string> headers={"Origin: "+origin,"Content-Type: application/json"};
    if(!cookie.empty())headers.push_back("Cookie: "+cookie);
    if(!csrf.empty())headers.push_back("X-CSRF-Token: "+csrf);
    auto response=native_http(origin+path,get?"":json_dump(body),headers,get);
    if(!response.error.empty()){clear();throw std::runtime_error(response.error);}
    if(response.status==401){clear();throw std::runtime_error("Team session expired or revoked; sign in again");}
    auto parsed=parse_json(response.body);
    require(parsed&&parsed->kind==J::Object,"Invalid team server response");
    require(response.status==200,text(*parsed,"error","Team request failed (HTTP "+std::to_string(response.status)+")"));
    auto set_cookie=response.headers.find("set-cookie");
    if(set_cookie!=response.headers.end()){
      auto value=set_cookie->second.substr(0,set_cookie->second.find(';'));
      require(value.rfind("codex_session=",0)==0,"Unexpected team session cookie");cookie=value;
    }
    return *parsed;
  }
  void login(const std::string& address,const std::string& user,const std::string& token){
    using namespace native_web;clear();origin=validate_origin(address);
    try {
      request("/api/login",obj({{"username",str(user)},{"password",str(token)}}));
      auto me=request("/api/me",obj(),true);
      require(text(me,"mode")=="server"&&text(me,"username")==user&&!text(me,"csrf").empty(),"Expected a hosted team service");
      csrf=text(me,"csrf");username=user;
    }catch(...){clear();throw;}
  }
  native_web::J rpc(const std::string& op,native_web::J values=native_web::obj()){
    native_web::require(connected(),"Sign in to a team server first");values.fields["op"]=native_web::str(op);
    auto response=request("/api/rpc",values);native_web::require(response.get("result")!=nullptr,"Missing team result");
    return response.fields.at("result");
  }
  void logout(){try{if(connected())request("/api/logout");}catch(...){clear();throw;}clear();}
};
