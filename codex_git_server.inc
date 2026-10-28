// Private, sandboxed libgit2 worker. Only the coordinator constructs these requests.
namespace embedded_git {
template <class T, void (*Free)(T *)> using Handle = std::unique_ptr<T, decltype(Free)>;
void check(int code) {
  if (code < 0) {
    const auto *error = git_error_last();
    throw std::runtime_error(error && error->message ? error->message : "libgit2 operation failed");
  }
}
Handle<git_object, git_object_free> resolve(git_repository *repo, const std::string &name) {
  git_object *object = nullptr;
  check(git_revparse_single(&object, repo, name.c_str()));
  return {object, git_object_free};
}
std::string oid_text(const git_oid *oid) {
  char value[GIT_OID_HEXSZ + 1]{};
  git_oid_tostr(value, sizeof value, oid);
  return value;
}
Handle<git_commit, git_commit_free> head(git_repository *repo) {
  git_reference *reference = nullptr;
  int code = git_repository_head(&reference, repo);
  Handle<git_reference, git_reference_free> ref(reference, git_reference_free);
  if (code == GIT_EUNBORNBRANCH || code == GIT_ENOTFOUND)
    return {nullptr, git_commit_free};
  check(code);
  git_commit *commit = nullptr;
  check(git_commit_lookup(&commit, repo, git_reference_target(reference)));
  return {commit, git_commit_free};
}
Handle<git_tree, git_tree_free> head_tree(git_repository *repo) {
  auto commit = head(repo);
  git_tree *tree = nullptr;
  if (commit)
    check(git_commit_tree(&tree, commit.get()));
  return {tree, git_tree_free};
}
Handle<git_status_list, git_status_list_free> status(git_repository *repo) {
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
  git_status_list *list = nullptr;
  check(git_status_list_new(&list, repo, &options));
  return {list, git_status_list_free};
}
void create_commit(git_repository *repo, git_tree *tree, const std::string &message,
                   std::vector<const git_commit *> parents, const char *ref, git_oid &id) {
  git_signature *raw = nullptr;
  check(git_signature_now(&raw, "Codex.cpp", "codex@localhost"));
  Handle<git_signature, git_signature_free> signature(raw, git_signature_free);
  check(git_commit_create(&id, repo, ref, raw, raw, nullptr, message.c_str(), tree, parents.size(),
                          parents.empty() ? nullptr : parents.data()));
}
int execute(const fs::path &directory, const fs::path &workspace, const std::vector<std::string> &args,
            const std::function<void(git_tree *)> &validate_tree = {}) {
  require(!args.empty(), "Missing repository operation");
  auto op = args[0];
  if (op == "init") {
    require(args.size() == 4 && args[1] == "--bare" && args[2].rfind("--initial-branch=", 0) == 0,
            "Unsupported initialization");
    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    options.flags = GIT_REPOSITORY_INIT_BARE | GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
    std::string branch = args[2].substr(17);
    options.initial_head = branch.c_str();
    git_repository *raw = nullptr;
    check(git_repository_init_ext(&raw, directory.u8string().c_str(), &options));
    Handle<git_repository, git_repository_free> repo(raw, git_repository_free);
    return 0;
  }
  git_repository *raw = nullptr;
  check(git_repository_open(&raw, directory.u8string().c_str()));
  Handle<git_repository, git_repository_free> repo(raw, git_repository_free);
  check(git_repository_set_workdir(raw, workspace.u8string().c_str(), 0));
  if (op == "rev-parse") {
    require(args.size() == 2, "Invalid reference request");
    auto object = resolve(raw, args[1]);
    std::cout << oid_text(git_object_id(object.get())) << '\n';
  } else if (op == "status" || op == "ls-files") {
    auto list = status(raw);
    for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
      const auto *entry = git_status_byindex(list.get(), i);
      const auto *delta = entry->index_to_workdir ? entry->index_to_workdir : entry->head_to_index;
      const char *name = delta ? delta->new_file.path : nullptr;
      require(name != nullptr, "Missing status path");
      if (op == "ls-files") {
        if (entry->status & GIT_STATUS_WT_NEW)
          std::cout << name << '\0';
      } else
        std::cout << entry->status << ' ' << name << '\n';
    }
  } else if (op == "for-each-ref") {
    git_reference_iterator *iterator = nullptr;
    check(git_reference_iterator_glob_new(&iterator, raw, "refs/heads/*"));
    Handle<git_reference_iterator, git_reference_iterator_free> guard(iterator, git_reference_iterator_free);
    git_reference *reference = nullptr;
    int code;
    while ((code = git_reference_next(&reference, iterator)) == 0) {
      Handle<git_reference, git_reference_free> ref(reference, git_reference_free);
      git_reference *resolved = nullptr;
      check(git_reference_resolve(&resolved, reference));
      Handle<git_reference, git_reference_free> target(resolved, git_reference_free);
      std::cout << std::string(git_reference_name(reference)).substr(11) << ' '
                << oid_text(git_reference_target(resolved)) << '\n';
    }
    require(code == GIT_ITEROVER, "Cannot enumerate branches");
  } else if (op == "fetch") {
    size_t offset = args.size() > 1 && args[1] == "--no-tags" ? 2 : 1;
    require(args.size() == offset + 2, "Invalid local fetch");
    auto source = fs::u8path(args[offset]);
    require(source.is_absolute() && fs::is_directory(source), "Only local repository transfer is allowed");
    git_remote *remote = nullptr;
    check(git_remote_create_anonymous(&remote, raw, source.u8string().c_str()));
    Handle<git_remote, git_remote_free> guard(remote, git_remote_free);
    git_fetch_options options = GIT_FETCH_OPTIONS_INIT;
    options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    std::string spec = args[offset + 1];
    char *value = spec.data();
    git_strarray specs{&value, 1};
    check(git_remote_fetch(remote, &specs, &options, "Codex.cpp local transfer"));
  } else if (op == "reset") {
    require(args.size() == 3 && (args[1] == "--hard" || args[1] == "--soft"), "Invalid reset");
    auto object = resolve(raw, args[2]);
    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    options.checkout_strategy = GIT_CHECKOUT_FORCE;
    check(git_reset(raw, object.get(), args[1] == "--hard" ? GIT_RESET_HARD : GIT_RESET_SOFT, &options));
  } else if (op == "add" || op == "commit") {
    git_index *index = nullptr;
    check(git_repository_index(&index, raw));
    Handle<git_index, git_index_free> guard(index, git_index_free);
    if (op == "add") {
      check(git_index_update_all(index, nullptr, nullptr, nullptr));
      check(git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr));
      check(git_index_write(index));
    } else {
      auto at = std::find(args.begin(), args.end(), "-m");
      require(at != args.end() && at + 1 != args.end(), "Commit message required");
      git_oid tree_id{}, commit_id{};
      check(git_index_write_tree(&tree_id, index));
      git_tree *tree = nullptr;
      check(git_tree_lookup(&tree, raw, &tree_id));
      Handle<git_tree, git_tree_free> holder(tree, git_tree_free);
      auto parent = head(raw);
      std::vector<const git_commit *> parents;
      if (parent)
        parents.push_back(parent.get());
      create_commit(raw, tree, *(at + 1), parents, "HEAD", commit_id);
    }
  } else if (op == "diff") {
    auto tree = head_tree(raw);
    git_diff *diff = nullptr;
    bool cached = args.size() > 1 && args[1] == "--cached";
    if (cached)
      check(git_diff_tree_to_index(&diff, raw, tree.get(), nullptr, nullptr));
    else
      check(git_diff_tree_to_workdir_with_index(&diff, raw, tree.get(), nullptr));
    Handle<git_diff, git_diff_free> guard(diff, git_diff_free);
    if (cached)
      return git_diff_num_deltas(diff) ? 1 : 0;
    std::string output;
    int printed = git_diff_print(
        diff, GIT_DIFF_FORMAT_PATCH,
        [](const git_diff_delta *, const git_diff_hunk *, const git_diff_line *line, void *payload) {
          auto &out = *static_cast<std::string *>(payload);
          if (out.size() + line->content_len + 1 > 4 * 1024 * 1024)
            return static_cast<int>(GIT_EUSER);
          if (line->origin == '+' || line->origin == '-' || line->origin == ' ')
            out += line->origin;
          out.append(line->content, line->content_len);
          return 0;
        },
        &output);
    check(printed);
    std::cout << output;
  } else if (op == "merge") {
    require(args.size() == 4 && args[1] == "--no-ff" && args[2] == "--no-edit", "Invalid merge");
    auto current = head(raw);
    require(current != nullptr, "Cannot merge into an unborn branch");
    auto object = resolve(raw, args[3]);
    git_commit *other = nullptr;
    check(git_commit_lookup(&other, raw, git_object_id(object.get())));
    Handle<git_commit, git_commit_free> theirs(other, git_commit_free);
    if (git_oid_equal(git_commit_id(current.get()), git_commit_id(other)) ||
        git_graph_descendant_of(raw, git_commit_id(current.get()), git_commit_id(other)) == 1)
      return 0;
    git_index *merged = nullptr;
    check(git_merge_commits(&merged, raw, current.get(), other, nullptr));
    Handle<git_index, git_index_free> index(merged, git_index_free);
    if (git_index_has_conflicts(merged)) {
      git_index_conflict_iterator *iterator = nullptr;
      check(git_index_conflict_iterator_new(&iterator, merged));
      Handle<git_index_conflict_iterator, git_index_conflict_iterator_free> guard(
          iterator, git_index_conflict_iterator_free);
      const git_index_entry *ancestor, *ours, *theirs_entry;
      int code;
      while ((code = git_index_conflict_next(&ancestor, &ours, &theirs_entry, iterator)) == 0)
        std::cout << (ours ? ours->path : theirs_entry ? theirs_entry->path : ancestor->path) << '\n';
      require(code == GIT_ITEROVER, "Cannot enumerate conflicts");
      return 1; // In-memory merge: no branch, index or working file was changed.
    }
    git_oid tree_id{}, commit_id{};
    check(git_index_write_tree_to(&tree_id, merged, raw));
    git_tree *tree = nullptr;
    check(git_tree_lookup(&tree, raw, &tree_id));
    Handle<git_tree, git_tree_free> holder(tree, git_tree_free);
    if (validate_tree)
      validate_tree(tree);
    create_commit(raw, tree, "Merge " + args[3], {current.get(), other}, nullptr, commit_id);
    auto target = resolve(raw, oid_text(&commit_id));
    // Preserve a recovery marker before the first worktree mutation.
    {
      std::ofstream marker(directory / "MERGE_HEAD");
      marker << oid_text(git_commit_id(other)) << '\n';
      marker.close();
      require(bool(marker), "Cannot record merge recovery state");
    }
    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    options.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_RECREATE_MISSING;
    check(git_checkout_tree(raw, target.get(), &options));
    git_reference *reference = nullptr;
    check(git_repository_head(&reference, raw));
    Handle<git_reference, git_reference_free> ref(reference, git_reference_free);
    git_reference *updated = nullptr;
    check(git_reference_create_matching(&updated, raw, git_reference_name(reference), &commit_id, 1,
                                        git_commit_id(current.get()), "Codex.cpp merge"));
    git_reference_free(updated);
    check(git_repository_state_cleanup(raw));
  } else
    throw std::runtime_error("Unsupported repository operation");
  return 0;
}
} // namespace embedded_git
int git_command_main(int argc, char **argv) {
  try {
    require(argc >= 5, "Missing internal repository arguments");
    struct Runtime {
      Runtime() {
        embedded_git::check(git_libgit2_init());
      }
      ~Runtime() {
        git_libgit2_shutdown();
      }
    } runtime;
    std::vector<std::string> arguments;
    for (int i = 4; i < argc; ++i)
      arguments.push_back(argv[i]);
    return embedded_git::execute(fs::u8path(argv[2]), fs::u8path(argv[3]), arguments);
  } catch (const std::exception &error) {
    std::cerr << "libgit2: " << error.what() << '\n';
    return 2;
  }
}
