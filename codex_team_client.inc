// HTTP/HTTPS client for the existing hosted API. No account tokens or session cookies
// are persisted, and project permissions remain enforced by HostedService.
struct TeamConnection {
  std::string origin,cookie,csrf,username,compatibility;int protocol=-1;bool read_only=true,deltas=false;
  bool connected()const{return !cookie.empty()&&!csrf.empty()&&protocol>=0;}
  void clear(){std::fill(cookie.begin(),cookie.end(),'\0');cookie.clear();csrf.clear();username.clear();compatibility.clear();protocol=-1;read_only=true;deltas=false;}
  static std::string validate_origin(std::string value){
    return native_web::browser_origin(value);
  }
  native_web::J request(const std::string& path,const native_web::J& body=native_web::obj(),bool get=false){
    using namespace native_web;
    std::vector<std::string> headers={"Origin: "+origin,"Content-Type: application/json"};
    if(!cookie.empty())headers.push_back("Cookie: "+cookie);
    if(!csrf.empty())headers.push_back("X-CSRF-Token: "+csrf);
    if(protocol>=0)headers.push_back("X-Codex-Protocol: "+std::to_string(protocol));
    auto response=native_http(origin+path,get?"":json_dump(body),headers,get);
    if(!response.error.empty()){clear();throw std::runtime_error(response.error);}
    if(response.status==426){clear();throw std::runtime_error("Team protocol changed or is incompatible; reconnect and negotiate again. No operation was retried.");}
    if(response.status==401){clear();throw std::runtime_error("Team session expired or revoked; sign in again");}
    auto parsed=parse_json(response.body);
    require(parsed&&parsed->kind==J::Object,"Invalid team server response");
    require(response.status==200,text(*parsed,"error","Team request failed (HTTP "+std::to_string(response.status)+")"));
    auto set_cookie=response.headers.find("set-cookie");
    if(set_cookie!=response.headers.end()){
      auto value=set_cookie->second.substr(0,set_cookie->second.find(';'));
      require(value.rfind("codex_session_",0)==0||value.rfind("codex_session=",0)==0,"Unexpected team session cookie");cookie=value;
    }
    return *parsed;
  }
  void negotiate(){using namespace native_web;
    auto response=native_http(origin+"/api/protocol",{}, {"Origin: "+origin},true);require(response.error.empty(),response.error);
    auto description=parse_json(response.body);
    const bool missing=response.status==404||(response.status==400&&description&&text(*description,"error")=="Unknown route");
    if(missing){require(team_adapter(0,true)!=nullptr,"Server has no protocol handshake; explicitly load the trusted legacy adapter for read-only access");
      auto probe=native_http(origin+"/api/me",{}, {"Origin: "+origin},true);auto me=parse_json(probe.body);
      require(probe.error.empty()&&probe.status==401&&me&&text(*me,"mode")=="server","Legacy server identity does not match the supported profile");
      protocol=0;read_only=true;deltas=false;compatibility=team_adapter(0,true)->profile+" (unverified server semantics; read-only)";return;
    }
    require(response.status==200&&description&&text(*description,"application")=="codex-cpp"&&text(*description,"mode")=="server","Invalid team protocol handshake; no downgrade attempted");
    auto versions=description->get("protocols"),features=description->get("capabilities");require(versions&&versions->kind==J::Array&&!versions->items.empty()&&versions->items.size()<=16&&features&&features->kind==J::Array&&features->items.size()<=32,"Malformed team protocol metadata");
    std::set<int> supported;for(const auto& value:versions->items){require(value.kind==J::Other&&value.value.size()<=5&&value.value.find_first_not_of("0123456789")==std::string::npos&&!value.value.empty(),"Invalid peer protocol version");supported.insert(team_wire_version(value.value));}
    if(supported.count(1))protocol=1;else for(const auto& codec:team_adapters())if(codec.client&&supported.count(codec.wire)){protocol=codec.wire;compatibility=codec.profile;break;}
    require(protocol>=0,"No compatible team protocol or installed adapter; upgrade one side or install a reviewed adapter");
    std::set<std::string> capabilities;for(const auto& feature:features->items){require(feature.kind==J::String&&feature.str().size()<=128,"Invalid peer capability");capabilities.insert(feature.str());}
    require(capabilities.count("scope-auth.v1")&&capabilities.count("snapshot.full.v1"),"Peer cannot guarantee scoped reads and full snapshots");
    read_only=!(capabilities.count("approval.bound.v1")&&capabilities.count("request-id.v1")&&capabilities.count("team.write.v1"));
    deltas=capabilities.count("snapshot.delta.v1")!=0;
  }
  std::string protocol_status()const{return "Protocol "+std::to_string(protocol)+(compatibility.empty()?"":" via "+compatibility)+(read_only?" | compatibility read-only":" | development operations negotiated");}
  void login(const std::string& address,const std::string& user,const std::string& token){
    using namespace native_web;clear();origin=validate_origin(address);
    try {
      negotiate();
      request("/api/login",obj({{"username",str(user)},{"password",str(token)}}));
      auto me=request("/api/me",obj(),true);
      require(text(me,"mode")=="server"&&text(me,"username")==user&&!text(me,"csrf").empty(),"Expected a hosted team service");
      csrf=text(me,"csrf");username=user;
    }catch(...){clear();throw;}
  }
  native_web::J rpc(const std::string& op,native_web::J values=native_web::obj()){
    native_web::require(connected(),"Sign in to a team server first");values.fields["op"]=native_web::str(op);
    native_web::require(!read_only||native_web::team_read_operation(op),"Compatibility is read-only: this peer has not negotiated approval, deduplication and write semantics");
    if(!deltas&&op=="snapshot")values.fields.erase("since");
    const auto* codec=protocol==1?nullptr:native_web::team_adapter(protocol,true);
    auto response=request("/api/rpc",native_web::team_encode_request(std::move(values),codec));native_web::require(response.get("result")!=nullptr,"Missing team result");
    return native_web::team_decode_result(response.fields.at("result"),codec);
  }
  void logout(){try{if(connected())request("/api/logout");}catch(...){clear();throw;}clear();}
};
