#define main codex_runtime_main
#include "../codex.cpp"
#undef main
#include <cassert>
int main(){const auto home=fs::temp_directory_path()/("codex-plugin-test-"+random_id("test"));fs::create_directories(home);const auto source=home/"source";std::string error;
#if defined(_WIN32)
_putenv_s("CODEX_CPP_HOME",(home/"config").string().c_str());
#else
setenv("CODEX_CPP_HOME",(home/"config").c_str(),1);
#endif
const std::string manifest=R"({"api":1,"id":"demo","version":"1","settings":{"binary":"default"},"commands":{"run":{"description":"run fixture","prompt":"Inspect only"}},"ui":["panel","card","status"],"events":{"tool.completed":[{"kind":"status","text":"{{tool}}:{{exit_code}}"}]}})";
assert(write_all(source/"plugin.json",manifest,error));assert(write_all(source/"skills"/"check"/"SKILL.md","description: fixture\n",error));
plugins=PluginRegistry{};assert(plugins.install(source).find("Installed demo")!=std::string::npos);assert(plugins.find("demo")&&!plugins.find("demo")->enabled);
assert(plugins.expand("/demo:run","").empty());assert(plugins.set_enabled("demo",true,error));assert(plugins.commands().empty());plugins.load();assert(plugins.find("demo")->enabled);
assert(plugins.expand("/demo:run","extra")=="Inspect only\n\nUser arguments:\nextra");assert(plugins.expand("/run","").empty());
assert(skills_manifest(home).find("demo:check: fixture")!=std::string::npos);ReadSkillTool skill;assert(skill.run(R"({"name":"demo:check"})",home).exit_code==0);
auto commands=matching_slash_commands("/demo:");assert(commands.size()==1&&std::string(commands.front()->name)=="/demo:run");
std::string observed;plugins.display=[&](const auto& id,const auto& kind,const auto& text){observed=id+"/"+kind+"/"+text;};
plugins.event("tool.completed",home,"read_file",2);assert(observed=="demo/status/read_file:2");PluginUiTool ui;assert(ui.run(R"({"plugin":"demo","kind":"card","text":"result"})",home).exit_code==0);assert(observed=="demo/card/result");
assert(ui.run(R"({"plugin":"missing","kind":"card","text":"no"})",home).exit_code!=0);assert(plugins.ui("demo","sidebar","no").size());assert(plugins.ui("demo","card",std::string(4097,'x')).size());
assert(plugins.set_enabled("demo",false,error));assert(plugins.find("demo")->enabled);plugins.load();assert(!plugins.find("demo")->enabled);assert(plugins.commands().empty());assert(plugins.ui("demo","card","no").size());
assert(plugins.install(source)=="plugin already installed");assert(plugins.config("demo","binary",std::string("/path with spaces/tool"),error));plugins.load();assert(plugins.find("demo")->settings.at("binary")=="/path with spaces/tool");
assert(plugins.config("demo","binary",std::nullopt,error));plugins.load();assert(plugins.find("demo")->settings.at("binary")=="default");
assert(plugins.install(source,true).find("Updated demo")!=std::string::npos);assert(fs::is_directory(home/"config"/"plugin-backups"));
assert(!plugin_json(R"({"api":1,"api":2})"));
assert(!plugins.set_enabled("../../escape",true,error));
assert(write_all(source/"plugin.json",R"({"api":2,"id":"demo","version":"1"})",error));assert(!PluginRegistry::read(source,error));
assert(write_all(source/"plugin.json",R"({"api":1,"id":"../escape","version":"1"})",error));assert(!PluginRegistry::read(source,error));
assert(write_all(source/"plugin.json",R"({"api":1,"id":"bad","version":"1","ui":["status"],"events":{"tool.completed":[{"kind":"card","text":"bad"}]}})",error));assert(!PluginRegistry::read(source,error));
assert(write_all(source/"plugin.json",R"({"api":1,"id":"bad","version":"1","mcp":"../outside.json"})",error));assert(!PluginRegistry::read(source,error));
#if !defined(_WIN32)
assert(write_all(source/"plugin.json",R"({"api":1,"id":"linked","version":"1"})",error));fs::create_symlink(home/"config",source/"link");assert(plugins.install(source)=="plugin links are not supported");fs::remove(source/"link");
#endif
assert(write_all(home/"config"/"plugins"/"broken"/"plugin.json","invalid",error));plugins.load();assert(plugins.find("demo")&&plugins.errors.size()==1);
fs::remove_all(home);std::cout<<"PASS plugin lifecycle, isolation, namespaces, skills, UI, events and invalid manifests\n";
}
