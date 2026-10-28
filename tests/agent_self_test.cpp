// Standalone checks for codex.cpp; build this file separately.
#define main codex_runtime_main
#include "../codex.cpp"
#undef main

static bool self_test(const fs::path& root){int failed = 0;std::error_code root_ec;fs::create_directories(root,root_ec);
auto check =[&](bool ok,const char* what){std::cout <<(ok ? "PASS " : "FAIL ")<< what << '\n';if(!ok)++failed; };
const std::string sample = R"({"id":"resp_123","output":[{"type":"function_call","call_id":"call_7","name":"read_file","arguments":"{\"path\":\"a.txt\"}"}]})";
std::string parse_err;auto parsed_response=parse_api_response(sample,false,parse_err); check(parsed_response && parsed_response->calls.size()==1 && parsed_response->calls[0].name=="read_file" &&
  parsed_response->calls[0].call_id=="call_7" && json_string_field(parsed_response->calls[0].arguments,"path")==std::optional<std::string>("a.txt"),"Responses function_call parser");
check(parsed_response && parsed_response->id=="resp_123","response id parser");
const std::string chat_sample = R"({"id":"chat_1","choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"tool_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]}}]})";
auto parsed_chat=parse_api_response(chat_sample,true,parse_err); check(parsed_chat && parsed_chat->calls.size()==1 && parsed_chat->calls[0].call_id=="tool_1","Chat Completions tool_call parser");
const std::string many=R"({"id":"r","metadata":{"name":"decoy"},"output":[{"type":"function_call","name":"read_file","call_id":"a","arguments":"{\"path\":\"a\"}"},{"type":"function_call","name":"list_dir","call_id":"b","arguments":"{\"path\":\".\"}"}]})";
auto parsed_many=parse_api_response(many,false,parse_err); check(parsed_many && parsed_many->calls.size()==2 && parsed_many->calls[1].name=="list_dir","multiple scoped tool calls");
ApiClient stream_client;stream_client.chatgpt_auth=true;auto streamed=stream_client.parse_codex_stream(
  "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}\n\n"
  "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r1\",\"output\":null}}\n\n",parse_err);
check(streamed && streamed->calls.size()==1 && streamed->calls[0].call_id=="c1","Codex SSE tool result");
check(!stream_client.parse_codex_stream("data: {\"type\":\"response.in_progress\"}\n",parse_err),"incomplete Codex SSE rejected");
check(json_string_field(R"({"outer":{"path":"decoy"},"path":"right"})","path")==std::optional<std::string>("right"),"JSON fields stay in scope");
ToolRouter router;router.add<ReadFileTool>();router.add<ListDirTool>();router.add<ReadSkillTool>();router.add<WriteFileTool>();router.add<ShellTool>();router.add<ApplyPatchTool>();
check(router.find("shell")!= nullptr && router.find("nope")== nullptr,"ToolRouter lookup"); check(router.specs_json().find("\"read_file\"")!= std::string::npos,"tool schema generation");
check(router.chat_specs_json().find("\"function\"")!= std::string::npos,"chat tool schema generation");fs::path troot = root / ".codex_cpp" / "selftest";
std::error_code ec;fs::create_directories(troot,ec);std::string err;write_all(troot / "a.txt","hello\n",err); RuntimePolicy p;p.sandbox = SandboxMode::WorkspaceWrite;p.approval = ApprovalPolicy::Never;
Session dummy;dummy.root = troot;dummy.dir = troot;dummy.id = "selftest";dummy.events_path = troot / "events.jsonl"; FunctionCall read{"read_file","c1",R"({"path":"a.txt"})"};auto rr = router.invoke(read,troot,p,dummy);
check(rr.exit_code == 0 && rr.output == "hello\n","read_file runtime");FunctionCall wr{"write_file","c2",R"({"path":"b.txt","content":"world\n"})"};
auto rw = router.invoke(wr,troot,p,dummy);check(rw.exit_code == 0 && slurp(troot / "b.txt")== "world\n","write_file runtime"); FunctionCall escape{"write_file","c3",R"({"path":"../outside.txt","content":"no"})"};
check(router.invoke(escape,troot,p,dummy).exit_code!=0,"workspace file path guard");FunctionCall shell{"shell","c4",R"({"command":"touch denied.txt"})"};
const auto sandboxed=restricted_shell_command(troot,"true",p.sandbox);auto sandbox_shell=router.invoke(shell,troot,p,dummy);
check((sandboxed ? sandbox_shell.exit_code==0 && fs::exists(troot/"denied.txt"): sandbox_shell.exit_code==126 && !fs::exists(troot/"denied.txt")), "automatic shell runs only under an OS sandbox");
if(sandboxed){
  // The sandbox may allow a private shadow at the same pathname. Check the
  // actual host file: a zero exit status inside that private filesystem is safe.
  const auto outside=troot.parent_path()/"shell-outside.txt";
  const bool fixture=write_all(outside,"host sentinel",err);
  FunctionCall escape_shell{"shell","c5",R"({"command":"printf escaped > ../shell-outside.txt"})"};
  router.invoke(escape_shell,troot,p,dummy);
  check(fixture && slurp(outside)=="host sentinel","sandbox preserves host files outside workspace");
  fs::remove(outside,ec);
}
RuntimePolicy plan=p;plan.sandbox=SandboxMode::ReadOnly;check(router.invoke(wr,troot,plan,dummy).exit_code==126,"read-only execution guard");
if(restricted_shell_command(troot,"true",SandboxMode::ReadOnly)){FunctionCall inspect{"shell","c6",R"({"command":"printf inspection"})"};
  auto inspected=router.invoke(inspect,troot,plan,dummy);check(inspected.exit_code==0 && inspected.output.find("inspection")!=std::string::npos, "read-only shell can inspect");
  FunctionCall cannot_write{"shell","c7",R"({"command":"touch read-only-denied.txt"})"};check(router.invoke(cannot_write,troot,plan,dummy).exit_code!=0 &&
  !fs::exists(troot/"read-only-denied.txt"),"read-only shell denies writes");
}
RuntimePolicy unrestricted=p;unrestricted.sandbox=SandboxMode::DangerFullAccess;FunctionCall cwd_shell{"shell","c8",
#if defined(_WIN32)
R"({"command":"cd"})"
#else
R"({"command":"pwd"})"
#endif
};
auto native_shell=router.invoke(cwd_shell,troot,unrestricted,dummy);if(native_shell.exit_code)std::cerr<<"native shell exit "<<native_shell.exit_code<<": "<<native_shell.output<<'\n';check(native_shell.exit_code==0,"explicit unrestricted shell still runs");
fs::path guard_file=troot.parent_path()/"hardlink-guard.txt";write_all(guard_file,"guard",err);fs::path alias=troot/"hardlink-guard.txt";
fs::create_hard_link(guard_file,alias,ec);if(!ec){FunctionCall attack{"shell","hardlink",R"({"command":"printf escaped > hardlink-guard.txt"})"};
  auto denied=router.invoke(attack,troot,p,dummy);check(denied.exit_code==126 && denied.output.find("hardlink")!=std::string::npos &&
  slurp(guard_file)=="guard","external hardlink shell refused before execution"); FunctionCall direct_write{"write_file","hardlink-write",R"({"path":"hardlink-guard.txt","content":"escaped"})"};
  check(router.invoke(direct_write,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard","hardlinked write_file target refused");
  FunctionCall patch{"apply_patch","hardlink-patch",R"({"patch":""})"};check(router.invoke(patch,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard", "hardlinked apply_patch workspace refused");
  fs::remove(alias,ec);
}else check(false,"create hardlink test fixture");
fs::remove(guard_file,ec);Session s;check(init_session(s,troot,false,std::nullopt,err),"session creation");Session empty;check(init_session(empty,troot,false,s.id,err) && empty.items.empty(),"empty listed session can resume");s.append_transcript("abc");
const std::string u="{\"role\":\"user\",\"content\":\"hello\"}";const std::string a="{\"role\":\"assistant\",\"content\":\"world\"}"; check(s.record_item(u)&& s.record_item(a),"structured item journal append");
Session s2;check(init_session(s2,troot,false,s.id,err)&& s2.transcript == "abc" && s2.items==std::vector<std::string>({u,a}),"structured session resume");
const auto ids=session_ids(troot);check(std::find(ids.begin(),ids.end(),s.id+".items")==ids.end(),"session list excludes item journals");
char name[]="codex-cpp",flag[]="--resume";char* missing[]={name,flag};check(!parse_options(2,missing,err),"missing resume ID is rejected before model request");
check(!init_session(s2,troot,false,std::string("../escape"),err),"invalid session ID rejected"); check(s.replace_items({u})&& init_session(s2,troot,false,s.id,err)&& s2.items.size()==1,"atomic item journal replacement");
check(matching_slash_commands("").empty()&& matching_slash_commands("hello /model").empty(),"slash popup plain-text guard"); auto slash_model = matching_slash_commands("/mo");
check(!slash_model.empty()&& std::string(slash_model.front()->name)== "/model","slash popup prefix matching"); check(matching_slash_commands("/model high").empty(),"slash popup closes for arguments");
write_all(troot / "AGENTS.md","Project rule: run focused tests.\n",err);write_all(troot / ".agents" / "skills" / "demo" / "SKILL.md", "---\nname: demo\ndescription: Small test skill\n---\nInstructions.\n",err);
check(skills_manifest(troot).find("demo: Small test skill")!=std::string::npos,"project skill discovery");
check(router.invoke({"read_skill","c6",R"({"name":"demo"})"},troot,p,dummy).output.find("Instructions.")!=std::string::npos,"skill read by name");
ApiClient prompt_api;prompt_api.model = "test-model";prompt_api.style = ApiStyle::Responses;
PromptBuilder prompt_builder{troot,p,prompt_api,false,"concise","finish the test"};PromptBundle prompt_bundle = prompt_builder.build();
check(prompt_bundle.developer_instructions.find("Sandbox and approvals")!= std::string::npos && prompt_bundle.developer_instructions.find("test-model")!= std::string::npos,"prompt developer layers");
check(prompt_bundle.contextual_user_prefix.find("Project rule: run focused tests.")!= std::string::npos && prompt_bundle.contextual_user_prefix.find("<environment_context>")!= std::string::npos &&
  prompt_bundle.contextual_user_prefix.find(troot.string())!= std::string::npos,"prompt AGENTS and environment context");
RuntimePolicy ro = p;ro.sandbox = SandboxMode::ReadOnly;ro.approval = ApprovalPolicy::OnRequest;PromptBuilder ro_builder{troot,ro,prompt_api,false,"",""};
check(ro_builder.build().developer_instructions.find("workspace is read-only")!= std::string::npos,"prompt read-only permissions");
prompt_builder.design_mode=true;check(prompt_builder.build().developer_instructions.find("# Design mode")!=std::string::npos,"design instructions reach developer context");check(prompt_builder.build().developer_instructions.find("end-to-end task")!=std::string::npos && prompt_builder.build().developer_instructions.find("operational mistake or misinterpretation")!=std::string::npos,"design prioritizes task flow and consequential copy");check(prompt_builder.build().developer_instructions.find("actual primary-workflow testing is a required delivery check")!=std::string::npos && prompt_builder.build().developer_instructions.find("explicitly mark the workflow unverified")!=std::string::npos,"design requires observable workflow checks or explicit runtime gap");prompt_builder.design_mode=false;check(prompt_builder.build().developer_instructions.find("# Design mode")==std::string::npos,"design instructions absent outside mode");ro_builder.design_mode=true;auto design_plan=ro_builder.build().developer_instructions;check(design_plan.find("# Design mode")!=std::string::npos && design_plan.find("workspace is read-only")!=std::string::npos,"design proposal retains read-only permissions");
write_all(troot/"Memory.md","# Project memory\n- Build with clang++\n",err);
ro_builder.memory=load_project_memory(troot);
check(ro_builder.build().developer_instructions.find("Build with clang++")!=std::string::npos && ro_builder.build().contextual_user_prefix.find("Build with clang++")==std::string::npos,"memory stays outside user history");
write_all(troot/"Memory.md","- Updated memory\n",err);
check(ro_builder.build().developer_instructions.find("Updated memory")==std::string::npos,"memory snapshot stays stable between turns");
ro_builder.memory=load_project_memory(troot);
check(ro_builder.build().developer_instructions.find("Updated memory")!=std::string::npos,"explicit reload refreshes memory");
check(s.replace_items({u}) && ro_builder.build().developer_instructions.find("Updated memory")!=std::string::npos,"memory survives history compaction");
auto quota=parse_codex_quota(R"({"rate_limit":{"limit_reached":true,"primary_window":{"used_percent":100,"reset_at":200},"secondary_window":{"used_percent":100,"reset_at":300}}})",100);
check(quota && quota->reset==300,"quota waits for all exhausted windows");
quota=parse_codex_quota(R"({"rate_limit":{"limit_reached":false,"primary_window":{"used_percent":100,"reset_at":200}}})",100);
check(quota && quota->reset==0,"ordinary 429 does not trigger quota wait");
{auto m=parse_codex_models(R"({"models":[{"slug":"a","visibility":"list","default_reasoning_level":"low","supported_reasoning_levels":[{"effort":"low"},{"effort":"ultra"}]},{"slug":"h","visibility":"hide"}]})");
check(m && m->size()==1 && (*m)[0].id=="a" && (*m)[0].efforts.size()==2 && (*m)[0].efforts[1]=="ultra" && (*m)[0].default_effort=="low","codex model catalog keeps listed models and their efforts");
auto o=parse_openai_models(R"({"data":[{"id":"zeta"},{"id":"text-embedding-3"},{"id":"alpha"}]})");
check(o && o->size()==2 && (*o)[0].id=="alpha" && (*o)[0].efforts.empty(),"openai model list is sorted and drops non-chat models");
check(!parse_codex_models("{}") && !parse_openai_models("{}"),"model list parsers reject unrelated JSON");}
quota=parse_codex_quota(R"({"rate_limit":{"limit_reached":true,"primary_window":{"used_percent":100,"reset_at":50}}})",100);
check(quota && quota->reset==0 && !parse_codex_quota("{}",100),"missing or expired reset does not loop");
check(parse_codex_quota(R"({"rate_limit":{"allowed":true,"limit_reached":false}})",100)->available,"reset card quota restoration is recognized");
Session recovery;check(init_session(recovery,troot,false,std::nullopt,err),"recovery session creation");
recovery.append_transcript("unfinished task\n");
recovery.emit(EventKind::TurnStarted,"{\"task\":\"finish task\",\"sandbox\":\"read-only\"}");
recovery.record_item(R"({"type":"function_call","call_id":"unknown","name":"shell","arguments":"{}"})");
Session reopened;check(init_session(reopened,troot,false,recovery.id,err) && reopened.pending_turn()==std::make_pair(std::string("finish task"),std::string("read-only")),"unfinished turn survives restart with sandbox");
check(reopened.close_unconfirmed_calls() && reopened.items.back().find("UNKNOWN")!=std::string::npos,"unconfirmed tool result does not replay execution");
const auto count=reopened.items.size();check(reopened.close_unconfirmed_calls() && reopened.items.size()==count,"recovery is idempotent");
reopened.emit(EventKind::TurnCompleted,"{\"status\":\"interrupted\"}");check(!reopened.pending_turn().first.empty(),"interrupted turn remains available to continue");
reopened.emit(EventKind::TurnCompleted,"{\"status\":\"completed\"}");check(reopened.pending_turn().first.empty(),"completed turn is not resumed");
// Remote connection parsing must never allow local shell or SSH option injection.
{
  auto parse_remote=[&](std::vector<std::string> words,std::string& error){std::vector<char*> args;for(auto& word:words)args.push_back(word.data());return parse_remote_connection(static_cast<int>(args.size()),args.data(),error);};
  std::string error;
  auto connection=parse_remote({"codex-cpp","connect","alice@server","--project","demo","--port","2222","--check"},error);
  check(connection&&connection->check&&connection->project=="demo"&&connection->port=="2222","remote connection accepts account, project and readiness check");
  if(connection){auto command=remote_ssh_command(*connection);check(command.front()=="ssh"&&command.back()=="--check"&&std::find(command.begin(),command.end(),"ForwardAgent=no")!=command.end(),"remote transport uses OpenSSH without forwarding agent credentials");}
  check(!parse_remote({"codex-cpp","connect","-oProxyCommand=touch"},error),"remote rejects SSH option injection");
  check(!parse_remote({"codex-cpp","connect","alice@server;id"},error),"remote rejects shell metacharacters");
  check(!parse_remote({"codex-cpp","connect","server","--project","../bob"},error),"remote rejects project traversal");
  check(!parse_remote({"codex-cpp","connect","server","--port","65536"},error),"remote rejects invalid ports");
  check(!parse_remote({"codex-cpp","connect","server","--danger-full-access"},error),"remote rejects local runtime flags");
  check(!parse_remote({"codex-cpp","connect","server","--check","--login"},error),"remote rejects conflicting actions");
}
{
  auto remote=[&](std::vector<std::string> words){std::vector<char*> args;for(auto& word:words)args.push_back(word.data());std::string error;return parse_remote_connection(static_cast<int>(args.size()),args.data(),error);};
  auto team=remote({"codex-cpp","connect","alice@server","--team","demo","--personal"});
  check(team&&team->collaborate&&team->personal&&team->team=="demo","remote personal team workspace selection");
  check(bool(remote({"codex-cpp","connect","server","--collaborate"})),"remote team management console selection");
  check(!remote({"codex-cpp","connect","server","--personal"}),"remote personal mode requires a team");
  check(!remote({"codex-cpp","connect","server","--team","demo","--project","other"}),"remote rejects mixed private and team workspaces");
  std::vector<std::string> words={"codex-cpp","--","--danger-full-access"};std::vector<char*> args;for(auto& word:words)args.push_back(word.data());std::string error;auto options=parse_options(static_cast<int>(args.size()),args.data(),error);
  check(options&&options->policy.sandbox==SandboxMode::WorkspaceWrite&&options->prompt==std::vector<std::string>{"--danger-full-access"},"prompt delimiter prevents shared message from changing runtime permissions");
}
// Provider defaults must not inherit the previous provider's configuration.
{
  std::string error;Options selected;selected.provider="deepseek";
  check(configure_provider(selected,error)&&selected.api_style==ApiStyle::ChatCompletions&&selected.api_key_env=="DEEPSEEK_API_KEY","DeepSeek preset chooses chat API and its own key variable");
  Options openai;openai.provider="openai";
  check(configure_provider(openai,error)&&openai.api_style==ApiStyle::Responses&&openai.api_key_env=="OPENAI_API_KEY","OpenAI preset chooses Responses and its own key variable");
  Options invalid;invalid.provider="unknown";
  check(!configure_provider(invalid,error),"unknown provider rejected");
  Options custom;custom.provider="custom";custom.base_url="http://127.0.0.1:1234/v1";custom.model="local";custom.no_api_key=true;
  check(configure_provider(custom,error)&&custom.no_api_key&&custom.base_url=="http://127.0.0.1:1234/v1"&&custom.model=="local","custom preset preserves explicit endpoint and no-key mode");
  check(find_slash_command("/provider")!=nullptr,"provider selector appears in command menu");
}
// Undo restores working-tree bytes rather than the committed version.
{
  FileUndoHistory history;const auto folder=troot/"undo";fs::create_directories(folder);
  const auto file=folder/"user.txt";write_all(file,"uncommitted user work",err);
  check(history.write(folder,file,"agent edit").exit_code==0,"undo captures existing working bytes");
  check(history.undo(folder).exit_code==0&&slurp(file)=="uncommitted user work","undo preserves pre-existing user edits");
  const auto created=folder/"created.txt";
  check(history.write(folder,created,"first").exit_code==0&&history.write(folder,created,"second").exit_code==0,"undo captures consecutive writes");
  check(history.undo(folder).exit_code==0&&slurp(created)=="first","undo restores previous write");
  check(history.undo(folder).exit_code==0&&!fs::exists(created),"undo removes newly created file");
  history.write(folder,file,"agent edit");write_all(file,"later user edit",err);
  check(history.undo(folder).exit_code!=0&&slurp(file)=="later user edit","undo refuses to overwrite later user edits");
  write_all(file,"agent edit",err);
  const auto alias=folder/"alias";fs::create_hard_link(file,alias);
  check(history.undo(folder).exit_code!=0,"undo rejects hardlinked target");fs::remove(alias);
  fs::remove(file);
#if !defined(_WIN32)
  fs::create_symlink(folder/"other",file);
  check(history.undo(folder).exit_code!=0,"undo rejects substituted symlink");fs::remove(file);
#endif
  check(history.write(folder,file,std::string(16*1024*1024+1,'x')).exit_code!=0&&!fs::exists(file),"undo rejects oversized write before mutation");
}
std::cout <<(failed ? "SELF-TEST FAILED\n" : "SELF-TEST PASSED\n");return failed == 0; }

int main(int argc,char** argv){
  const fs::path root=argc>1?fs::path(argv[1]):fs::temp_directory_path()/"codex-cpp-selftest";
  return self_test(root)?0:1;
}
