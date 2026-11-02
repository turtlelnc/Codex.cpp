// Direct lifecycle tests avoid a shell/CLI timing approximation of Creator.
static int creator_process_checks(const fs::path& root){
  fs::create_directories(root);int failed=0;
  auto check=[&](bool ok,const char* message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';if(!ok)++failed;};
  auto policy=ProcessPolicy::tool(root,true);
  auto unsafe=policy;unsafe.unsafe=true;
  check(creator_process({"not-executed"},unsafe,1,{},false).exit_code==126,"Creator refuses policy unsafe override");
  PenguinBuildTool builder;
  check(builder.is_shell(),"Build uses command approval, not only file-write approval");
  check(builder.execute("{\"path\":\"missing\"}",root,SandboxMode::ReadOnly,{},false).exit_code==126,"Build cannot write under read-only policy");
#if defined(_WIN32)
  check(creator_process({"cmd.exe"},policy,1,{},false).exit_code==126,"Creator fails closed without Windows AppContainer");
#else
  if(!sandbox_available()){std::cerr<<"Required lifecycle sandbox unavailable: "<<sandbox_failure<<'\n';return 1;}
  auto closed=creator_process({"/bin/sh","-c","exec 1>&- 2>&-; sleep 2; exit 7"},policy,5,{},false);
  check(closed.exit_code==7,"Early output EOF still waits for the actual deadline and exit status");
  const auto marker=root/"surviving-child",ready=root/"child-ready";
  const auto command="(trap '' TERM; echo ready > "+shell_quote(ready.string())+"; sleep 4; echo leaked > "+shell_quote(marker.string())+") & wait";
  auto timeout=creator_process({"/bin/sh","-c",command},policy,1,{},false);
  check(timeout.exit_code==124&&fs::exists(ready),"Timeout starts and terminates a process with descendants");
  fs::remove(ready);
  busy_input=[&](const char*,size_t){return fs::exists(ready);};
  auto cancelled=creator_process({"/bin/sh","-c",command},policy,8,{},true);busy_input={};
  check(cancelled.exit_code==130&&fs::exists(ready),"User cancellation terminates a started process with descendants");
  fs::remove(ready);bool caught=false;
  try{creator_process({"/bin/sh","-c",command},policy,8,[](){throw std::runtime_error("cancel callback");},false);}catch(const std::runtime_error&){caught=true;}
  check(caught,"Throwing UI cancellation unwinds and stops the worker");
  std::this_thread::sleep_for(std::chrono::seconds(5));
  check(!fs::exists(marker),"No timed-out or cancelled child survives to perform a delayed write");
#if defined(__APPLE__)
  // Probe initialization never needs to fork. Denying fork closes the process
  // group escape path for an untrusted ABI initializer on Seatbelt.
  auto probe=policy;probe.no_fork=true;
  check(creator_process({"/bin/sh","-c","sleep 1 & wait"},probe,3,{},false).exit_code!=0,"macOS ABI probe cannot spawn detached descendants");
#endif
  const auto inherited=root/"inherited-snapshot";fs::create_directory(inherited);std::string error;write_all(inherited/"asset","original",error);process_protected_paths.push_back(fs::canonical(inherited));
  WriteFileTool writer;
  check(writer.run("{\"path\":\"inherited-snapshot/asset\",\"content\":\"changed\"}",root).exit_code!=0,"Model file tools cannot change inherited snapshots inside a broad workspace");
  auto overwrite=creator_process({"/bin/sh","-c","echo changed > "+shell_quote((inherited/"asset").string())},policy,3,{},false);
  check(overwrite.exit_code!=0&&slurp(inherited/"asset")=="original","OS sandbox prevents writing inherited snapshots even when workspace contains them");process_protected_paths.clear();
  // Termination must finish before the scratch directory is removed.
  for(const auto& entry:fs::directory_iterator(execution.private_dir))check(entry.path().filename()=="bwrap","Worker scratch directories are cleaned after child termination");
#endif
  return failed?1:0;
}
