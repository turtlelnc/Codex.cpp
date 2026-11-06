// Included in native_web: the host owns authorization and mutation semantics.
// Native plugins provide bounded, declarative codecs, never authorization grants.
static bool team_read_operation(const std::string& op){
  static const std::set<std::string> reads={"projects","list","project_status","member_candidates","providers","models","snapshot","files","read","diff","tool_log","status","side_snapshot","preferences","suggestions_snapshot"};
  return reads.count(op)!=0;
}
struct TeamAdapter{
  int wire=0;std::string profile;bool client=false,server=false,full_snapshots=true;
  std::map<std::string,std::string> request_names,response_names;
};
static bool team_binding(const std::string& name){
  static const std::set<std::string> bindings={"op","project","scope","request_id","approval","allow","fingerprint","confirmed","username","session_id","call_id","base","source","target","permission","sandbox","expected_session","main_session"};return bindings.count(name)!=0;
}
static const std::vector<TeamAdapter>& team_adapters(){
  static const auto adapters=[](){std::vector<TeamAdapter> result;
    auto data=plugin_call("team.protocol.adapters",obj(),[](const J&,const plugin::Emit&){return obj({{"adapters",arr()}});});
    auto entries=data.get("adapters");require(entries&&entries->kind==J::Array&&entries->items.size()<=15,"Invalid team adapter catalog");std::set<int> versions;
    for(const auto& entry:entries->items){TeamAdapter codec;codec.wire=plugin_integer(entry,"wire_version");require(codec.wire>=0&&codec.wire<=65535&&codec.wire!=1&&versions.insert(codec.wire).second,"Invalid/duplicate adapter wire version");require(plugin_integer(entry,"canonical_version")==1,"Adapter requires another host protocol");codec.profile=plugin_text(entry,"profile");require(identifier(codec.profile),"Invalid adapter profile");codec.client=plugin_flag(entry,"client");codec.server=plugin_flag(entry,"server");codec.full_snapshots=plugin_flag(entry,"full_snapshots");
      auto names=[&](const char* key){std::map<std::string,std::string> names;std::set<std::string> destinations;auto value=entry.get(key);if(!value)return names;require(value->kind==J::Object&&value->fields.size()<=16,"Invalid adapter field map");for(const auto& mapping:value->fields){auto from=mapping.first;require(mapping.second.kind==J::String,"Adapter field name must be a string");auto to=mapping.second.str();require(identifier(from)&&identifier(to)&&from!=to&&destinations.insert(to).second,"Invalid/ambiguous adapter field map");require(!team_binding(from)&&!team_binding(to)&&from!="pending"&&to!="pending"&&from!="events"&&to!="events"&&from!="error"&&to!="error","Adapter cannot translate security bindings");names[from]=to;}
        for(const auto& mapping:names)require(!names.count(mapping.second),"Adapter mappings cannot chain");return names;};
      codec.request_names=names("request_renames");codec.response_names=names("response_renames");result.push_back(std::move(codec));
    }return result;
  }();return adapters;
}
static const TeamAdapter* team_adapter(int wire,bool client){for(const auto& codec:team_adapters())if(codec.wire==wire&&(client?codec.client:codec.server))return &codec;return nullptr;}
static J team_rename(J value,const std::map<std::string,std::string>& names,bool reverse=false){
  if(names.empty())return value;require(value.kind==J::Object,"Adapter requires an object payload");for(const auto& mapping:names){const auto& from=reverse?mapping.second:mapping.first;const auto& to=reverse?mapping.first:mapping.second;auto found=value.fields.find(from);if(found==value.fields.end())continue;require(!value.fields.count(to),"Ambiguous compatibility fields; request refused");value.fields[to]=std::move(found->second);value.fields.erase(found);}return value;
}
static J team_encode_request(J request,const TeamAdapter* codec){if(!codec)return request;if(codec->full_snapshots&&text(request,"op")=="snapshot")request.fields.erase("since");return team_rename(std::move(request),codec->request_names,true);}
static J team_decode_request(J request,const TeamAdapter* codec){if(!codec)return request;request=team_rename(std::move(request),codec->request_names);if(codec->full_snapshots&&text(request,"op")=="snapshot")request.fields.erase("since");return request;}
static J team_encode_result(J result,const TeamAdapter* codec){return codec?team_rename(std::move(result),codec->response_names):result;}
static J team_decode_result(J result,const TeamAdapter* codec){return codec?team_rename(std::move(result),codec->response_names,true):result;}
static J team_protocol_description(const std::string& mode){
  std::vector<J> versions={number(1)},profiles;for(const auto& codec:team_adapters())if(codec.server){versions.push_back(number(codec.wire));profiles.push_back(obj({{"wire_version",number(codec.wire)},{"profile",str(codec.profile)}}));}
  return obj({{"application",str("codex-cpp")},{"mode",str(mode)},{"protocols",arr(versions)},
    {"capabilities",arr({str("suggestions.v1"),str("side-chat.v1"),str("scope-auth.v1"),str("snapshot.full.v1"),str("snapshot.delta.v1"),str("approval.bound.v1"),str("request-id.v1"),str("team.write.v1")})},{"adapters",arr(profiles)}});
}
static int team_wire_version(const std::string& value){require(!value.empty()&&value.size()<=5&&value.find_first_not_of("0123456789")==std::string::npos,"Invalid team protocol header");int n=std::stoi(value);require(n<=65535&&std::to_string(n)==value,"Invalid team protocol header");return n;}
