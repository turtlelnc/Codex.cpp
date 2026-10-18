#define main codex_runtime_main
#include "../codex.cpp"
#undef main
#include <cassert>
int main(){
#if !defined(_WIN32)
  ::signal(SIGPIPE,SIG_IGN);
#endif
  const auto base=fs::temp_directory_path()/random_id("process-security"),root=base/"project",outside=base/"secret";std::string error;fs::create_directories(root);write_all(outside,"OUTSIDE_SECRET",error);write_all(root/"ok","inside",error);
  const auto home=root/"fake-home";fs::create_directories(home/".ssh");write_all(home/".ssh"/"key","KEY_SECRET",error);
#if defined(_WIN32)
  _putenv_s("HOME",home.string().c_str());_putenv_s("OPENAI_API_KEY","SHOULD_NOT_INHERIT");
#else
  setenv("HOME",home.c_str(),1);setenv("OPENAI_API_KEY","SHOULD_NOT_INHERIT",1);
#endif
  execution.initialize(root);write_all(execution.private_dir/"host-secret","HOST_SECRET",error);if(!sandbox_available()){std::cout<<"UNAVAILABLE: "<<ManagedProcess::backend()<<" "<<sandbox_failure<<'\n';fs::remove_all(base);return 77;}
  auto policy=ProcessPolicy::tool(root,true);auto call=[&](const std::string& command){return run_capture(command,8000,policy);};
  assert(call("cat ok").output.find("inside")!=std::string::npos);
  assert(call("cat "+shell_quote(outside.string())).exit_code!=0);
  assert(call("cat "+shell_quote((home/".ssh/key").string())).exit_code!=0);
  assert(call("cat "+shell_quote((execution.private_dir/"host-secret").string())).exit_code!=0);
  assert(FrontendBridge::clean("中文\x1b]52;\a\xc2\x9b\xff")=="中文]52;");
  assert(call("env").output.find("SHOULD_NOT_INHERIT")==std::string::npos);
  assert(call("printf allowed > allowed").exit_code==0&&slurp(root/"allowed")=="allowed");
  assert(run_capture("printf denied > denied",8000,ProcessPolicy::tool(root,false)).exit_code!=0);
  auto python=process_executable("python3");if(!python.empty()){
    const auto script="import subprocess; p=subprocess.run(['/bin/cat',"+json_dump(Json{Json::String,outside.string(),{},{}})+"],start_new_session=True); raise SystemExit(p.returncode)";
    auto result=call(shell_quote(python.string())+" -c "+shell_quote(script));assert(result.exit_code!=0&&result.output.find("OUTSIDE_SECRET")==std::string::npos);
    const auto detach="import subprocess; p=subprocess.run(['/usr/bin/true'],start_new_session=True); print('DETACH_RESULT',p.returncode)";
    auto detached=call(shell_quote(python.string())+" -c "+shell_quote(detach));std::cout<<"Detached session probe: "<<detached.exit_code<<" "<<detached.output.substr(0,300)<<'\n';
  }
  fs::create_symlink(outside,root/"link");assert(call("cat link").exit_code!=0);fs::remove(root/"link");
  fs::create_hard_link(outside,root/"alias");assert(call("cat alias").exit_code!=0);fs::remove(root/"alias");
  ReadFileTool read;assert(read.run("{\"path\":\"fake-home/.ssh/key\"}",root).exit_code!=0);
  std::cout<<"PASS executable sandbox, outside reads/writes, credentials, environment, child inheritance, symlinks and hardlinks\n";fs::remove_all(base);
}
