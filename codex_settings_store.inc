// Shared by local TUI, WebUI and hosted per-account/project preferences.
struct PreferenceDefinition { const char* key; const char* label; const char* help; bool initial; };
static const std::vector<PreferenceDefinition>& preference_definitions(){
  static const std::vector<PreferenceDefinition> definitions={
    {"side_chat","Read-only side discussion","Manual questions use model quota; never writes project files.",true},
    {"auto_suggestions","Automatic next-step suggestions","Additional model usage. At most once per main session; off by default.",false},
    {"carry_reasoning","Carry available reasoning in requests","Chat API only: include returned reasoning on historical replies. Required tool continuation reasoning is always retained. May increase input tokens.",false}
  };return definitions;
}
static fs::path assistance_directory(const fs::path& home,const fs::path& root){
  auto path=home/"web-assistance"/portable_project_key(root);
#if defined(_WIN32)
  auto native=fs::absolute(path).lexically_normal().make_preferred().native();
  if(native.rfind(L"\\\\?\\",0)!=0){if(native.rfind(L"\\\\",0)==0)native=L"\\\\?\\UNC\\"+native.substr(2);else native=L"\\\\?\\"+native;}
  return fs::path(native);
#else
  return path;
#endif
}
static J load_preferences(const fs::path& home,const fs::path& root,const std::string& actor=""){
  auto value=read_json(assistance_directory(home,root)/"ui-preferences"/(digest(actor)+".json"));auto result=obj();
  for(const auto& entry:preference_definitions()){auto item=value.get(entry.key);require(!item||(item->kind==J::Other&&(item->value=="true"||item->value=="false")),"Invalid saved preference");result.fields[entry.key]=item?*item:boolean(entry.initial);}
  return result;
}
// Defined after the portable cross-process lease implementation.
static J save_preferences(const fs::path& home,const fs::path& root,const std::string& actor,const J& patch);

static std::shared_ptr<portable::Lease> lock_provider_registry(const fs::path& home);
