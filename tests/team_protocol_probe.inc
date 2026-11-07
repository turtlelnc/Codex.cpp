// Test-only CLI exercising the real native TeamConnection, including codecs.
static int team_protocol_probe(int argc,char** argv){
  try{
    if(argc!=4)return 2;
    plugin_creator_self=native_web::executable(argv[0]);
    if(auto module=getenv_or("CODEX_TEST_PROTOCOL_PLUGIN");!module.empty())plugin_manifests.push_back(module);
    plugin_boot();
    TeamConnection connection;connection.login(argv[2],argv[3],getenv_or("CODEX_TEST_TEAM_TOKEN"));
    std::string raw;std::getline(std::cin,raw);auto input=parse_json(raw);
    native_web::require(input&&input->kind==Json::Object,"Invalid test RPC");auto op=native_web::text(*input,"op");input->fields.erase("op");
    auto result=connection.rpc(op,*input);
    std::cout<<json_dump(native_web::obj({{"protocol",native_web::number(connection.protocol)},{"read_only",native_web::boolean(connection.read_only)},{"profile",native_web::str(connection.compatibility)},{"result",result}}))<<'\n';connection.logout();return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
