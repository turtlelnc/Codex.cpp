static int side_boundary_checks(const fs::path& directory){
  using namespace native_web;try{
    private_dir(directory);auto root=directory/"work";private_dir(root);Options options;options.root=root;options.provider="custom";options.model="fixture";options.base_url="http://127.0.0.1:1";options.no_api_key=true;
    App parent(root,fs::absolute("unused-side-test"),directory/"state",options,true,true);
    auto snapshot=obj({{"op",str("side_snapshot")},{"main_session",str("")}});
    parent.rpc(snapshot,true,"alice");auto& alice=*parent.side_chats.at(digest("alice/"));alice.sid="session-alice-private";
    {std::ofstream journal(alice.session_directory/(alice.sid+".items.jsonl"));journal<<"{\"role\":\"user\",\"content\":\"ALICE_PRIVATE\"}\n";}
    auto own=parent.rpc(snapshot,true,"alice"),other=parent.rpc(snapshot,true,"bob");
    require(json_dump(own).find("ALICE_PRIVATE")!=std::string::npos,"Own side history unavailable");require(json_dump(other).find("ALICE_PRIVATE")==std::string::npos,"Side history leaked to another actor");require(parent.sessions().items.empty(),"Side journals appeared in main navigation");
    bool denied=false;try{parent.rpc(obj({{"op",str("session")},{"session_id",str(alice.sid)}}));}catch(...){denied=true;}require(denied,"Main session accepted private side journal");
    parent.rpc(obj({{"op",str("preferences_save")},{"side_chat",boolean(false)}}),true,"alice");denied=false;try{parent.rpc(snapshot,true,"alice");}catch(...){denied=true;}require(denied,"Disabled side channel still usable");
    require(flag(parent.rpc(obj({{"op",str("preferences")}}),true,"bob"),"side_chat"),"Preferences crossed actor boundary");
    denied=false;try{parent.rpc(obj({{"op",str("suggestions_generate")}}),true,"bob");}catch(...){denied=true;}require(denied&&parent.side_chats.size()==2,"Disabled automatic suggestions started model worker");
    // Local instances share a home and an empty actor; all assistance data must
    // nevertheless remain scoped to the canonical workspace.
    auto shared_home=directory/"local-home",root_a=directory/"local-a",root_b=directory/"local-b";private_dir(root_a);private_dir(root_b);
    private_dir(shared_home/"ui-preferences");atomic_save(shared_home/"ui-preferences"/(digest("")+".json"),obj({{"auto_suggestions",boolean(true)}}));
    App local_a(root_a,fs::absolute("unused"),shared_home,options,true,true),local_b(root_b,fs::absolute("unused"),shared_home,options,true,true);
    require(!flag(local_a.ui_preferences(""),"auto_suggestions"),"Ambiguous old preferences silently enabled model usage");
    local_a.rpc(obj({{"op",str("preferences_save")},{"auto_suggestions",boolean(true)},{"side_chat",boolean(false)}}));
    require(!flag(local_b.ui_preferences(""),"auto_suggestions")&&flag(local_b.ui_preferences(""),"side_chat"),"Local preferences leaked across workspaces");
    private_dir(local_a.assistance_root()/"suggestions");atomic_save(local_a.assistance_root()/"suggestions"/(digest("")+".json"),obj({{"actions",arr({obj({{"title",str("A only")},{"prompt",str("Private A task")}})})}}));
    require(local_b.rpc(obj({{"op",str("suggestions_snapshot")}})).get("actions")==nullptr,"Cached suggestions leaked to a different workspace");
    App reopened(root_a,fs::absolute("unused"),shared_home,options,true,true);require(flag(reopened.ui_preferences(""),"auto_suggestions")&&reopened.rpc(obj({{"op",str("suggestions_snapshot")}})).get("actions"),"Workspace-scoped assistance did not persist");
    for(const auto& glyph:std::vector<std::string>{u8"é",u8"中",u8"😀"})for(size_t offset=3996;offset<=4000;++offset){auto value=std::string(offset,'x')+glyph+glyph;auto prefix=utf8_prefix(value,4000);require(prefix.size()<=4000&&valid_utf8(prefix)&&value.rfind(prefix,0)==0,"Context truncation split a UTF-8 code point");}
    require(utf8_prefix("abc",0).empty()&&utf8_prefix("abc",99)=="abc","UTF-8 prefix edge cases");
    HostedService hosted(directory/"hosted",options);hosted.binary=fs::absolute("unused-side-test");
    hosted.database.fields["users"]=obj({{"alice",obj()},{"bob",obj()}});
    hosted.database.fields["projects"]=obj({{"demo",obj({{"owner",str("alice")},{"members",arr({str("alice"),str("bob")})}})}});
    auto shared=hosted.app("alice","demo",false);shared->rpc(snapshot,true,"bob");auto& revoked=*shared->side_chats.at(digest("bob/"));
    hosted.dispatch("alice",obj({{"op",str("remove")},{"project",str("demo")},{"username",str("bob")}}));
    require(revoked.stopping,"Member revocation did not stop side workers");denied=false;try{hosted.dispatch("bob",obj({{"op",str("side_snapshot")},{"project",str("demo")},{"main_session",str("")}}));}catch(...){denied=true;}require(denied,"Removed member retained side access");
    std::cout<<"PASS actor-isolated side journals and settings, hidden/private main-session boundary, disabled-feature server enforcement\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
