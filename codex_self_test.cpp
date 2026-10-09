// Standalone checks for codex.cpp; build this file separately.
#define main codex_runtime_main
#include "codex.cpp"
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
if(sandboxed){FunctionCall escape_shell{"shell","c5",R"({"command":"touch ../shell-outside.txt"})"};
  check(router.invoke(escape_shell,troot,p,dummy).exit_code!=0 && !fs::exists(troot.parent_path()/"shell-outside.txt"),"sandbox rejects writes outside root");
}
RuntimePolicy plan=p;plan.sandbox=SandboxMode::ReadOnly;check(router.invoke(wr,troot,plan,dummy).exit_code==126,"read-only execution guard");
if(restricted_shell_command(troot,"true",SandboxMode::ReadOnly)){FunctionCall inspect{"shell","c6",R"({"command":"printf inspection"})"};
  auto inspected=router.invoke(inspect,troot,plan,dummy);check(inspected.exit_code==0 && inspected.output.find("inspection")!=std::string::npos, "read-only shell can inspect");
  FunctionCall cannot_write{"shell","c7",R"({"command":"touch read-only-denied.txt"})"};check(router.invoke(cannot_write,troot,plan,dummy).exit_code!=0 &&
  !fs::exists(troot/"read-only-denied.txt"),"read-only shell denies writes");
}
RuntimePolicy unrestricted=p;unrestricted.sandbox=SandboxMode::DangerFullAccess;FunctionCall cwd_shell{"shell","c8",R"({"command":"pwd"})"};
check(router.invoke(cwd_shell,troot,unrestricted,dummy).exit_code==0,"explicit unrestricted shell still runs");
fs::path guard_file=troot.parent_path()/"hardlink-guard.txt";write_all(guard_file,"guard",err);fs::path alias=troot/"hardlink-guard.txt";
fs::create_hard_link(guard_file,alias,ec);if(!ec){FunctionCall attack{"shell","hardlink",R"({"command":"printf escaped > hardlink-guard.txt"})"};
  auto denied=router.invoke(attack,troot,p,dummy);check(denied.exit_code==126 && denied.output.find("external hardlink alias")!=std::string::npos &&
  slurp(guard_file)=="guard","external hardlink shell refused before execution"); FunctionCall direct_write{"write_file","hardlink-write",R"({"path":"hardlink-guard.txt","content":"escaped"})"};
  check(router.invoke(direct_write,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard","hardlinked write_file target refused");
  FunctionCall patch{"apply_patch","hardlink-patch",R"({"patch":""})"};check(router.invoke(patch,troot,p,dummy).exit_code==126 && slurp(guard_file)=="guard", "hardlinked apply_patch workspace refused");
  fs::remove(alias,ec);
}else check(false,"create hardlink test fixture");
fs::remove(guard_file,ec);Session s;check(init_session(s,troot,false,std::nullopt,err),"session creation");s.append_transcript("abc");
const std::string u="{\"role\":\"user\",\"content\":\"hello\"}";const std::string a="{\"role\":\"assistant\",\"content\":\"world\"}"; check(s.record_item(u)&& s.record_item(a),"structured item journal append");
Session s2;check(init_session(s2,troot,false,s.id,err)&& s2.transcript == "abc" && s2.items==std::vector<std::string>({u,a}),"structured session resume");
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
write_all(troot/"Memory.md","# Project memory\n- Build with clang++\n",err);
ro_builder.memory=load_project_memory(troot);
check(ro_builder.build().developer_instructions.find("Build with clang++")!=std::string::npos && ro_builder.build().contextual_user_prefix.find("Build with clang++")==std::string::npos,"memory stays outside user history");
write_all(troot/"Memory.md","- Updated memory\n",err);
check(ro_builder.build().developer_instructions.find("Updated memory")==std::string::npos,"memory snapshot stays stable between turns");
ro_builder.memory=load_project_memory(troot);
check(ro_builder.build().developer_instructions.find("Updated memory")!=std::string::npos,"explicit reload refreshes memory");
check(s.replace_items({u}) && ro_builder.build().developer_instructions.find("Updated memory")!=std::string::npos,"memory survives history compaction");
Session recovery;check(init_session(recovery,troot,false,std::nullopt,err),"recovery session creation");
recovery.append_transcript("unfinished task\n");
recovery.emit(EventKind::TurnStarted,"{\"task\":\"finish task\",\"sandbox\":\"read-only\"}");
recovery.record_item(R"({"type":"function_call","call_id":"unknown","name":"shell","arguments":"{}"})");
Session reopened;check(init_session(reopened,troot,false,recovery.id,err) && reopened.pending_turn()==std::make_pair(std::string("finish task"),std::string("read-only")),"unfinished turn survives restart with sandbox");
check(reopened.close_unconfirmed_calls() && reopened.items.back().find("UNKNOWN")!=std::string::npos,"unconfirmed tool result does not replay execution");
const auto count=reopened.items.size();check(reopened.close_unconfirmed_calls() && reopened.items.size()==count,"recovery is idempotent");
reopened.emit(EventKind::TurnCompleted,"{\"status\":\"interrupted\"}");check(!reopened.pending_turn().first.empty(),"interrupted turn remains available to continue");
reopened.emit(EventKind::TurnCompleted,"{\"status\":\"completed\"}");check(reopened.pending_turn().first.empty(),"completed turn is not resumed");
std::cout <<(failed ? "SELF-TEST FAILED\n" : "SELF-TEST PASSED\n");return failed == 0; }

int main(int argc,char** argv){
  const fs::path root=argc>1?fs::path(argv[1]):fs::temp_directory_path()/"codex-cpp-selftest";
  return self_test(root)?0:1;
}
