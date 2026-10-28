// Optional read-only Gitx experiment. Included inside native_web.
J gitx_review(const fs::path &root) {
  struct Runtime {
    Runtime() { if (git_libgit2_init() < 0) throw std::runtime_error("Cannot initialize Gitx"); }
    ~Runtime() { git_libgit2_shutdown(); }
  };
  static Runtime runtime;
  gitx::GitRepository core(root);
  std::vector<J> status;
  for (const auto &item : core.status())
    status.push_back(obj({{"path", str(item.path)}, {"state", str(item.state)}}));
  auto check = [](int code) {
    if (code < 0) throw std::runtime_error("Gitx repository operation failed");
  };
  git_repository *raw = nullptr;
  check(git_repository_open(&raw, root.u8string().c_str()));
  std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw, git_repository_free);
  git_tree *raw_tree = nullptr;
  git_object *raw_head = nullptr;
  std::string head;
  int resolved = git_revparse_single(&raw_head, raw, "HEAD");
  std::unique_ptr<git_object, decltype(&git_object_free)> commit(raw_head, git_object_free);
  if (resolved == 0) {
    char oid[GIT_OID_HEXSZ + 1]{};
    git_oid_tostr(oid, sizeof oid, git_object_id(raw_head));
    head = oid;
    git_object *tree_object = nullptr;
    check(git_object_peel(&tree_object, raw_head, GIT_OBJECT_TREE));
    raw_tree = reinterpret_cast<git_tree *>(tree_object);
  } else if (resolved != GIT_ENOTFOUND && resolved != GIT_EUNBORNBRANCH) check(resolved);
  std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS;
  git_diff *raw_diff = nullptr;
  check(git_diff_tree_to_workdir_with_index(&raw_diff, raw, raw_tree, &options));
  std::unique_ptr<git_diff, decltype(&git_diff_free)> diff(raw_diff, git_diff_free);
  std::vector<J> untracked;
  for (size_t i = 0; i < git_diff_num_deltas(raw_diff); ++i) {
    const auto *delta = git_diff_get_delta(raw_diff, i);
    if (delta->status == GIT_DELTA_UNTRACKED) untracked.push_back(str(delta->new_file.path));
  }
  struct Output { std::string text; bool truncated = false; } output;
  int printed = git_diff_print(raw_diff, GIT_DIFF_FORMAT_PATCH,
    [](const git_diff_delta *, const git_diff_hunk *, const git_diff_line *line, void *payload) {
      auto &out = *static_cast<Output *>(payload);
      if (out.text.size() + line->content_len + 1 > 524288) {
        out.truncated = true; return static_cast<int>(GIT_EUSER);
      }
      if (line->origin == '+' || line->origin == '-' || line->origin == ' ') out.text += line->origin;
      out.text.append(line->content, line->content_len);
      return 0;
    }, &output);
  if (!output.truncated) check(printed);
  return obj({{"head", str(head)}, {"diff", str(output.text)}, {"untracked", arr(untracked)},
              {"truncated", boolean(output.truncated)}, {"status", arr(status)},
              {"backend", str("gitx/libgit2")}});
}
