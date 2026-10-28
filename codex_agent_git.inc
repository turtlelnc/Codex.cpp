// Model tools: a bounded JSON protocol to a separate OS-sandboxed worker.
namespace agent_git {
using namespace embedded_git;
std::string bounded_text(const std::string &value, size_t limit) {
  if (!valid_utf8(value))
    return "[Non-UTF-8 text omitted]";
  auto out = value.substr(0, limit);
  while (!valid_utf8(out))
    out.pop_back();
  return out;
}
void keys(const J &args, const std::set<std::string> &allowed) {
  require(args.kind == J::Object, "Expected argument object");
  for (const auto &item : args.fields)
    require(allowed.count(item.first), "Unknown Git argument");
}
std::string string_arg(const J &args, const std::string &key) {
  const auto *value = args.get(key);
  require(value && value->kind == J::String, "Missing string argument: " + key);
  require(value->value.find('\0') == std::string::npos, "NUL in Git argument");
  return value->value;
}
bool protected_path(const fs::path &path) {
  if (path.empty() || path.is_absolute() || path.has_root_name())
    return true;
  for (const auto &part : path) {
    auto name = lower(part.u8string());
    if (name == "." || name == ".." || name == ".git" || name == ".codex_cpp" || name == ".codex-cpp")
      return true;
#if defined(_WIN32)
    if (name.find(':') != std::string::npos || (!name.empty() && (name.back() == '.' || name.back() == ' ')))
      return true;
#endif
  }
  return false;
}
void safe_path(const fs::path &root, const std::string &name) {
  require(name.find('\0') == std::string::npos, "NUL in Git path");
  auto relative = fs::u8path(name);
  require(!protected_path(relative), "Protected or escaping Git path");
  require(within_root(root, root / relative) && !sensitive_path(root / relative),
          "Git path outside workspace or protected");
  fs::path current = root;
  for (const auto &part : relative) {
    current /= part;
    std::error_code error;
    auto status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory)
      break;
    require(!error && !fs::is_symlink(status), "Linked Git path denied");
    if (fs::is_regular_file(status))
      require(fs::hard_link_count(current) == 1, "Hardlinked Git path denied");
  }
}
std::string current_head(git_repository *repo) {
  auto commit = head(repo);
  return commit ? oid_text(git_commit_id(commit.get())) : "";
}
void inspect_index(git_repository *repo, const fs::path &root) {
  git_index *raw = nullptr;
  check(git_repository_index(&raw, repo));
  Handle<git_index, git_index_free> index(raw, git_index_free);
  for (size_t i = 0; i < git_index_entrycount(raw); ++i) {
    const auto *entry = git_index_get_byindex(raw, i);
    require(entry->mode != GIT_FILEMODE_LINK && entry->mode != GIT_FILEMODE_COMMIT,
            "Linked index entries denied");
    safe_path(root, entry->path);
  }
}
void inspect_tree(git_tree *tree, const fs::path &root) {
  int code = git_tree_walk(
      tree, GIT_TREEWALK_PRE,
      [](const char *prefix, const git_tree_entry *entry, void *payload) -> int {
        try {
          auto &base = *static_cast<const fs::path *>(payload);
          auto mode = git_tree_entry_filemode(entry);
          require(mode != GIT_FILEMODE_LINK && mode != GIT_FILEMODE_COMMIT, "Linked tree entries denied");
          safe_path(base, std::string(prefix) + git_tree_entry_name(entry));
          return 0;
        } catch (...) {
          return GIT_EUSER;
        }
      },
      const_cast<fs::path *>(&root));
  require(code == 0, "Checkout/merge tree contains a protected or linked path");
}
void require_clean(git_repository *repo) {
  auto list = status(repo);
  for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
    const auto *entry = git_status_byindex(list.get(), i);
    const auto *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
    require(delta && delta->new_file.path && protected_path(fs::u8path(delta->new_file.path)),
            "Commit or preserve current changes before checkout/merge");
  }
}
void apply(const fs::path &root, const J &args) {
  keys(args, {"patch"});
  auto patch = string_arg(args, "patch");
  require(!patch.empty() && patch.size() <= 700000, "Empty or oversized patch");
  if (auto risk = external_hardlink_risk(root))
    throw std::runtime_error("DENIED: " + *risk);
  git_diff *raw_diff = nullptr;
  check(git_diff_from_buffer(&raw_diff, patch.data(), patch.size()));
  Handle<git_diff, git_diff_free> diff(raw_diff, git_diff_free);
  require(git_diff_num_deltas(raw_diff) > 0, "Patch contains no file changes");
  for (size_t i = 0; i < git_diff_num_deltas(raw_diff); ++i) {
    const auto *delta = git_diff_get_delta(raw_diff, i);
    for (const auto *file : {&delta->old_file, &delta->new_file}) {
      require(file->mode != GIT_FILEMODE_LINK && file->mode != GIT_FILEMODE_COMMIT,
              "Symlink/submodule patches denied");
      if (file->path && std::string(file->path) != "/dev/null")
        safe_path(root, file->path);
    }
  }
  // A private bare repository makes git_apply work in plain directories too.
  // It never creates or changes the workspace's .git or its staging index.
  auto scratch = fs::temp_directory_path() / entropy();
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{scratch};
  git_repository *raw = nullptr;
  check(git_repository_init(&raw, scratch.u8string().c_str(), 1));
  Handle<git_repository, git_repository_free> repo(raw, git_repository_free);
  check(git_repository_set_workdir(raw, root.u8string().c_str(), 0));
  git_apply_options options{};
  check(git_apply_options_init(&options, GIT_APPLY_OPTIONS_VERSION));
  options.flags = GIT_APPLY_CHECK;
  check(git_apply(raw, raw_diff, GIT_APPLY_LOCATION_WORKDIR, &options));
  options.flags = 0;
  check(git_apply(raw, raw_diff, GIT_APPLY_LOCATION_WORKDIR, &options));
  std::cout << json_dump(obj({{"applied", boolean(true)}, {"files", number(git_diff_num_deltas(raw_diff))}}))
            << '\n';
}
int operate(const fs::path &root, const std::string &tool, const J &args) {
  if (tool == "apply_patch") {
    apply(root, args);
    return 0;
  }
  bool write = tool == "git_write";
  require(write || tool == "git_read", "Unknown internal Git tool");
  keys(args, write ? std::set<std::string>{"operation", "reference", "target", "message", "paths"}
                   : std::set<std::string>{"operation", "reference", "limit"});
  auto op = string_arg(args, "operation"), reference = string_arg(args, "reference");
  if (write) {
    require(
        std::set<std::string>{"init", "stage", "stage_all", "commit", "branch", "checkout", "merge"}.count(
            op),
        "Unsupported Git write operation");
    require(string_arg(args, "message").size() <= 1000, "Commit message too long");
    require(args.get("paths") && args.get("paths")->kind == J::Array, "paths must be an array");
    for (const auto &path : args.get("paths")->items) {
      require(path.kind == J::String, "Invalid stage path");
      safe_path(root, path.str());
    }
    if (auto risk = external_hardlink_risk(root))
      throw std::runtime_error("DENIED: " + *risk);
  } else
    require(std::set<std::string>{"status", "diff", "log", "branches"}.count(op),
            "Unsupported Git read operation");
  if (op == "init") {
    require(string_arg(args, "target").empty(), "init requires empty target");
    require(!fs::exists(root / ".git") && !fs::is_symlink(root / ".git"),
            "Repository metadata already exists");
    git_repository_init_options options{};
    check(git_repository_init_options_init(&options, GIT_REPOSITORY_INIT_OPTIONS_VERSION));
    options.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    options.initial_head = "main";
    git_repository *created = nullptr;
    check(git_repository_init_ext(&created, root.u8string().c_str(), &options));
    Handle<git_repository, git_repository_free> guard(created, git_repository_free);
    fs::create_directories(root / ".git/info");
    std::ofstream exclude(root / ".git/info/exclude", std::ios::app);
    exclude << "\n.codex_cpp/\n.codex-cpp/\n";
    exclude.close();
    require(bool(exclude), "Cannot exclude Agent metadata");
  }
  git_repository *raw = nullptr;
  check(git_repository_open(&raw, root.u8string().c_str()));
  Handle<git_repository, git_repository_free> repo(raw, git_repository_free);
  require(!git_repository_is_bare(raw) && git_repository_workdir(raw) &&
              fs::canonical(git_repository_workdir(raw)) == root,
          "Git tools require the current workspace repository");
  require(within_root(root, fs::u8path(git_repository_path(raw))), "External repository metadata denied");
  auto head_before = current_head(raw);
  if (write)
    require(string_arg(args, "target") == head_before,
            "HEAD changed; inspect and confirm exact target again");
  if (op == "status") {
    auto list = status(raw);
    std::vector<J> entries;
    for (size_t i = 0; i < git_status_list_entrycount(list.get()) && i < 1000; ++i) {
      const auto *entry = git_status_byindex(list.get(), i);
      const auto *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
      if (delta && delta->new_file.path && !protected_path(fs::u8path(delta->new_file.path)))
        entries.push_back(obj({{"path", str(delta->new_file.path)}, {"flags", number(entry->status)}}));
    }
    std::cout << json_dump(obj({{"head", str(head_before)},
                                {"entries", arr(entries)},
                                {"truncated", boolean(git_status_list_entrycount(list.get()) > 1000)}}))
              << '\n';
    return 0;
  }
  if (op == "diff") {
    inspect_index(raw, root);
    auto result = git_review(root);
    auto &value = result.fields["diff"].value;
    if (value.size() > 80000) {
      value = bounded_text(value, 80000);
      result.fields["truncated"] = boolean(true);
    }
    if (!valid_utf8(value)) {
      value = "[Non-UTF-8 diff omitted]";
      result.fields["truncated"] = boolean(true);
    }
    result.fields.erase("status");
    result.fields.erase("untracked");
    std::cout << json_dump(result) << '\n';
    return 0;
  }
  if (op == "log") {
    const auto *limit = args.get("limit");
    require(limit && limit->kind == J::Other, "Expected integer limit");
    require(!limit->value.empty() && limit->value.size() <= 3 &&
                std::all_of(limit->value.begin(), limit->value.end(),
                            [](unsigned char c) { return std::isdigit(c); }),
            "Expected integer limit");
    int count = std::stoi(limit->value);
    require(count >= 1 && count <= 100, "limit must be 1..100");
    std::vector<J> commits;
    if (!head_before.empty()) {
      auto from = resolve(raw, reference.empty() ? "HEAD" : reference);
      git_revwalk *walk = nullptr;
      check(git_revwalk_new(&walk, raw));
      Handle<git_revwalk, git_revwalk_free> guard(walk, git_revwalk_free);
      git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
      check(git_revwalk_push(walk, git_object_id(from.get())));
      git_oid oid{};
      int code = 0;
      while (count-- && (code = git_revwalk_next(&oid, walk)) == 0) {
        git_commit *commit = nullptr;
        check(git_commit_lookup(&commit, raw, &oid));
        Handle<git_commit, git_commit_free> holder(commit, git_commit_free);
        const char *summary = git_commit_summary(commit);
        commits.push_back(
            obj({{"id", str(oid_text(&oid))}, {"summary", str(summary ? bounded_text(summary, 1000) : "")}}));
      }
      require(code == 0 || code == GIT_ITEROVER, "Cannot walk history");
    }
    std::cout << json_dump(obj({{"head", str(head_before)}, {"commits", arr(commits)}})) << '\n';
    return 0;
  }
  if (op == "branches")
    return execute(root / ".git", root, {"for-each-ref"});
  if (op == "stage" || op == "stage_all") {
    git_index *index = nullptr;
    check(git_repository_index(&index, raw));
    Handle<git_index, git_index_free> guard(index, git_index_free);
    if (op == "stage_all") {
      auto callback = [](const char *name, const char *, void *payload) -> int {
        try {
          auto &base = *static_cast<const fs::path *>(payload);
          if (protected_path(fs::u8path(name)))
            return 1;
          safe_path(base, name);
          return 0;
        } catch (...) {
          return GIT_EUSER;
        }
      };
      check(git_index_update_all(index, nullptr, callback, const_cast<fs::path *>(&root)));
      check(
          git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, callback, const_cast<fs::path *>(&root)));
    } else {
      require(!args.get("paths")->items.empty(),
              "stage requires paths; use stage_all explicitly for all files");
      for (const auto &path : args.get("paths")->items) {
        auto name = path.str();
        if (fs::exists(root / fs::u8path(name)))
          check(git_index_add_bypath(index, name.c_str()));
        else
          check(git_index_remove_bypath(index, name.c_str()));
      }
    }
    check(git_index_write(index));
  } else if (op == "commit") {
    require(!trim(string_arg(args, "message")).empty(), "Commit message required");
    inspect_index(raw, root);
    int changed = execute(root / ".git", root, {"diff", "--cached", "--quiet"});
    require(changed == 1 || head_before.empty(), "No staged changes to commit");
    execute(root / ".git", root, {"commit", "-m", string_arg(args, "message")});
  } else if (op == "branch" || op == "checkout") {
    int valid = 0;
    check(git_branch_name_is_valid(&valid, reference.c_str()));
    require(valid && !reference.empty(), "Invalid local branch name");
    require(!head_before.empty(), "Create an initial commit before branching");
    git_reference *branch = nullptr;
    if (op == "branch") {
      auto commit = head(raw);
      check(git_branch_create(&branch, raw, reference.c_str(), commit.get(), 0));
    } else {
      require_clean(raw);
      inspect_index(raw, root);
      check(git_branch_lookup(&branch, raw, reference.c_str(), GIT_BRANCH_LOCAL));
      Handle<git_reference, git_reference_free> holder(branch, git_reference_free);
      auto target = resolve(raw, "refs/heads/" + reference);
      git_object *target_tree = nullptr;
      check(git_object_peel(&target_tree, target.get(), GIT_OBJECT_TREE));
      Handle<git_object, git_object_free> tree_guard(target_tree, git_object_free);
      inspect_tree(reinterpret_cast<git_tree *>(target_tree), root);
      git_checkout_options options{};
      check(git_checkout_options_init(&options, GIT_CHECKOUT_OPTIONS_VERSION));
      options.checkout_strategy = GIT_CHECKOUT_SAFE;
      check(git_checkout_tree(raw, target.get(), &options));
      check(git_repository_set_head(raw, ("refs/heads/" + reference).c_str()));
      branch = nullptr;
    }
    if (branch)
      git_reference_free(branch);
  } else if (op == "merge") {
    require(reference.size() == 40 && std::all_of(reference.begin(), reference.end(),
                                                  [](unsigned char c) { return std::isxdigit(c); }),
            "merge requires an exact source commit SHA");
    require_clean(raw);
    inspect_index(raw, root);
    int code = execute(root / ".git", root, {"merge", "--no-ff", "--no-edit", reference},
                       [&](git_tree *tree) { inspect_tree(tree, root); });
    if (code)
      return code;
  }
  std::cout << json_dump(obj(
                   {{"operation", str(op)}, {"head", str(current_head(raw))}, {"completed", boolean(true)}}))
            << '\n';
  return 0;
}
} // namespace agent_git
int agent_git_main(int argc, char **argv) {
  try {
    require(argc == 3, "Invalid internal Agent Git invocation");
    std::string frame;
    char c;
    while (std::cin.get(c) && c != '\n') {
      require(frame.size() < 900000, "Git request too large");
      frame += c;
    }
    auto request = parse_json(frame);
    require(request && request->kind == J::Object, "Invalid Agent Git request");
    struct Runtime {
      Runtime() {
        embedded_git::check(git_libgit2_init());
      }
      ~Runtime() {
        git_libgit2_shutdown();
      }
    } runtime;
    auto root = fs::canonical(fs::u8path(argv[2]));
    return agent_git::operate(root, text(*request, "tool"), field(*request, "arguments"));
  } catch (const std::exception &error) {
    std::cerr << "libgit2 tool: " << error.what() << '\n';
    return 2;
  }
}
ToolResult run_agent_tool(const std::string &tool, const std::string &arguments, const fs::path &root,
                          bool write, SandboxMode mode, const std::function<void()> &tick, bool watch) {
  try {
    if (write && mode == SandboxMode::ReadOnly)
      return {126, "DENIED: mutation disabled by read-only policy"};
    if (write)
      if (auto risk = external_hardlink_risk(root))
        return {126, "DENIED: " + *risk};
    auto parsed = parse_json(arguments);
    require(parsed && parsed->kind == J::Object, "Expected tool argument object");
    auto frame = json_dump(obj({{"tool", str(tool)}, {"arguments", *parsed}})) + "\n";
    require(frame.size() < 900000, "Tool request exceeds 900 KB");
    ManagedProcess worker;
    std::string why;
    auto policy = ProcessPolicy::tool(root, write, mode == SandboxMode::DangerFullAccess);
    require(worker.start({executable("codex-cpp").u8string(), "--native-agent-git", root.u8string()}, policy,
                         why, true),
            why);
    require(worker.send(frame, why), why);
    worker.close_input();
#if !defined(_WIN32)
    struct Terminal {
      termios saved{};
      bool raw = false;
      explicit Terminal(bool watch) {
        if (watch && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
          auto mode = saved;
          mode.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
          mode.c_iflag &= static_cast<tcflag_t>(~(ICRNL | IXON));
          mode.c_cc[VMIN] = 0;
          mode.c_cc[VTIME] = 0;
          raw = tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
        }
      }
      ~Terminal() {
        if (raw)
          tcsetattr(STDIN_FILENO, TCSANOW, &saved);
      }
    } terminal(watch);
#endif
    std::string output;
    char buffer[8192];
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (true) {
      if (watch && busy_input && busy_input("", 0))
        return {130, "Interrupted; inspect workspace before retrying"};
#if !defined(_WIN32)
      if (tui_interrupt_requested)
        return {130, "Interrupted; inspect workspace before retrying"};
      if (terminal.raw && !busy_input) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, 0) > 0) {
          char keys[64];
          auto n = ::read(STDIN_FILENO, keys, sizeof keys);
          if (n > 0 && contains_plain_escape(keys, n))
            return {130, "Interrupted; inspect workspace before retrying"};
        }
      }
#endif
      if (tick)
        tick();
      auto n = worker.receive(buffer, sizeof buffer, 100);
      if (n > 0) {
        output.append(buffer, static_cast<size_t>(n));
        require(output.size() <= 200000, "Git tool output exceeds limit; narrow the request");
      }
      if (n == -1)
        break;
      require(n != -2, "Git worker read failed");
      require(std::chrono::steady_clock::now() < until, "Git tool timed out");
    }
    int code = worker.wait(5000);
    return {code, "exit_code=" + std::to_string(code) + "\n" + output};
  } catch (const std::exception &error) {
    return {2, error.what()};
  }
}
