// Structured tools deliberately separate read and write approval policies.
class GitToolBase : public ToolRuntime {
  bool write_;

public:
ToolResult execute(const std::string& args,const fs::path& root,SandboxMode mode,const std::function<void()>& tick,bool watch)override{return run_with_policy(args,root,mode,tick,watch);}
  explicit GitToolBase(bool write) : write_(write) {}
  std::string name() const override {
    return write_ ? "git_write" : "git_read";
  }
  bool mutates() const override {
    return write_;
  }
  std::string spec_json() const override {
    if (write_)
      return R"({"type":"function","name":"git_write","description":"Modify only the current workspace Git repository using embedded libgit2. Inspect HEAD first and pass its exact SHA as target (empty for an unborn repository). Commit author is Codex.cpp. stage requires explicit paths; stage_all stages all permitted files. branch creates without switching; checkout refuses unsafe overwrites. merge requires reference and target to be exact commit SHAs and never writes conflict markers. Remote operations, force/reset/clean and arbitrary Git arguments are unavailable. Use empty strings/array for unused fields.","parameters":{"type":"object","properties":{"operation":{"type":"string","enum":["init","stage","stage_all","commit","branch","checkout","merge"]},"reference":{"type":"string"},"target":{"type":"string"},"message":{"type":"string"},"paths":{"type":"array","items":{"type":"string"}}},"required":["operation","reference","target","message","paths"],"additionalProperties":false},"strict":true})";
    return R"({"type":"function","name":"git_read","description":"Inspect the current workspace Git repository using embedded libgit2. status returns HEAD and path status flags; diff compares HEAD with final working files; log returns recent commit summaries; branches lists local refs. reference selects a commit for log (empty means HEAD); limit is 1..100 (use 20 normally). No command strings or remote operations.","parameters":{"type":"object","properties":{"operation":{"type":"string","enum":["status","diff","log","branches"]},"reference":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":100}},"required":["operation","reference","limit"],"additionalProperties":false},"strict":true})";
  }
  ToolResult run(const std::string &args, const fs::path &root) override {
    return run_with_policy(args, root, SandboxMode::WorkspaceWrite, {});
  }
  ToolResult run_with_policy(const std::string &args, const fs::path &root, SandboxMode mode,
                             const std::function<void()> &tick, bool watch = false) {
    return run_libgit_tool(name(), args, root, write_, mode, tick, watch);
  }
};
class GitReadTool final : public GitToolBase {
public:
  GitReadTool() : GitToolBase(false) {}
};
class GitWriteTool final : public GitToolBase {
public:
  GitWriteTool() : GitToolBase(true) {}
};
