{
  using namespace native_web;
  const auto work=root/"checkpoint-project";fs::create_directories(work);
  Session source;std::string why;check(init_session(source,work,false,{},why),"Initialize checkpoint history fixture");
  const std::string human=R"({"role":"user","content":"real initial task"})",latest=R"({"role":"user","content":"real next task"})";
  check(source.record_item(human),"Append actual user to immutable journal");
  for(int i=0;i<4;++i){const auto id="history-tool-"+std::to_string(i);check(source.record_item(json_dump(obj({{"type",str("function_call")},{"call_id",str(id)},{"name",str("read_file")},{"arguments",str("{}")}})))&&source.record_item(json_dump(obj({{"type",str("function_call_output")},{"call_id",str(id)},{"output",str("raw tool log "+std::to_string(i))}}))),"Append tool history");}
  check(source.record_item(latest),"Append latest real user input");const auto original=slurp(source.items_path);
  std::vector<std::string> compacted={R"({"role":"user","content":"[prior conversation summary]\nINTERNAL_SUMMARY_NOT_A_USER_MESSAGE"})",latest};
  for(auto& raw:compacted)raw=json_dump(*parse_json(raw));
  check(source.checkpoint_items(compacted)&&slurp(source.items_path).rfind(original,0)==0&&source.items==compacted,"Compaction appends a checkpoint while preserving original journal bytes");
  check(source.record_item(R"({"role":"assistant","content":"answer after compaction"})"),"Append message after checkpoint");
  const auto expected=source.items;Session resumed;
  check(init_session(resumed,work,false,source.id,why,&source)&&resumed.items==expected,"Restart restores model checkpoint and replays only following records");
  App app(work,{},root/"checkpoint-profile",Options{},true);app.sid=resumed.id;
  auto timeline=app.journal();size_t users=0,tools=0;bool leaked=false;
  for(const auto& row:timeline.items){users+=text(row,"k")=="user";tools+=text(row,"k")=="tool";leaked|=text(row,"text").find("INTERNAL_SUMMARY_NOT_A_USER_MESSAGE")!=std::string::npos;}
  check(users==2&&tools==4&&!leaked,"WebUI keeps all user/tool rows without exposing internal checkpoint summaries");
  auto log=app.rpc(obj({{"op",str("tool_log")},{"session_id",str(resumed.id)},{"call_id",str("history-tool-0")}}));
  check(text(log,"output")=="raw tool log 0","Pre-compaction full tool logs remain accessible");
  const auto before=slurp(resumed.items_path);const auto before_items=resumed.items;
  check(!resumed.record_item(R"({"type":"codex_context_checkpoint","version":2,"items":[]})")&&slurp(resumed.items_path)==before&&resumed.items==before_items,"Unsupported checkpoint version is rejected before mutation");
  check(!resumed.record_item(R"({"type":"codex_context_checkpoint","version":1,"items":[{"type":"codex_context_checkpoint","version":1,"items":[]}]})")&&slurp(resumed.items_path)==before,"Nested checkpoints are rejected");
  const auto oversized=json_dump(obj({{"role",str("user")},{"content",str(std::string(12'000'000,'x'))}}));
  check(!resumed.checkpoint_items({oversized})&&slurp(resumed.items_path)==before&&resumed.items==before_items,"Oversized checkpoints fail without changing readable history or model context");
  const auto legacy="legacy-summary";const auto legacy_file=project_session_root(work)/(std::string(legacy)+".items.jsonl");
  write_all(legacy_file,compacted[0]+"\n"+latest+"\n",why);write_all(project_session_root(work)/(std::string(legacy)+".transcript"),"\n[context compacted]\nINTERNAL_SUMMARY_NOT_A_USER_MESSAGE\n",why);App old(work,{},root/"checkpoint-old-profile",Options{},true);old.sid=legacy;auto rows=old.journal();
  check(rows.items.size()==2&&text(rows.items[0],"k")=="context"&&text(rows.items[1],"k")=="user","Old summary records are classified as internal context instead of user messages");
  const auto literal="literal-summary-marker";write_all(project_session_root(work)/(std::string(literal)+".items.jsonl"),compacted[0]+"\n",why);old.sid=literal;auto literal_rows=old.journal();check(literal_rows.items.size()==1&&text(literal_rows.items[0],"k")=="user","A user marker without compaction provenance remains a user message");
}
