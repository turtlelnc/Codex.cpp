{
  using plugin_permissions::Decision;
  const auto old_profile = getenv_or("CODEX_CPP_HOME");
  const auto workspace = root / "permission-workspace";
  const auto profile = root / "permission-profile";
  fs::create_directories(workspace / "docs");
  fs::create_directories(workspace / "docs-other");
  std::string write_error;
  write_all(workspace / "docs/a.txt", "fixture contents", write_error);
  write_all(workspace / "docs/b.txt", "second file", write_error);
  write_all(workspace / "docs-other/c.txt", "outside the directory grant", write_error);
  host_env("CODEX_CPP_HOME", profile.u8string());
  try {
    const auto canonical = fs::canonical(workspace);
    plugin_permissions::FileRead permissions(canonical);
    permissions.set_session("first");
    auto choose = [](Decision choice) {
      return [choice](const auto&, const auto&, const auto&, const auto&) { return choice; };
    };
    auto read = [&](const char* file, plugin_permissions::Prompt prompt = {}) {
      return permissions.read("digest-v1", canonical, file, prompt, 65536);
    };
    check(!read("docs/a.txt"), "Plugin file.read is denied by default");
    check(read("docs/a.txt",choose(Decision::Once)) == "fixture contents" &&
          !read("docs/a.txt"), "One-time plugin permission cannot be reused");
    check(read("docs/a.txt",choose(Decision::SessionDirectory)) &&
          read("docs/b.txt") && !read("docs-other/c.txt"),
          "Plugin directory authorization matches complete components");
    permissions.set_session("second");
    check(!read("docs/a.txt"), "Session plugin grant expires when switching sessions");
    check(read("docs/a.txt",choose(Decision::PersistentFile)).has_value(),
          "Persistent plugin grant is saved");
    plugin_permissions::FileRead restarted(canonical);
    check(restarted.read("digest-v1",canonical,"docs/a.txt",{},65536).has_value() &&
          !restarted.read("digest-v2",canonical,"docs/a.txt",{},65536),
          "Persistent grant survives restart and is specific to module digest");
    const auto listing = parse_json(permissions.manage("list"));
    const auto id = plugin_text(listing->get("persistent")->items.at(0),"id");
    check(restarted.manage("revoke " + id) == "Plugin permission revoked" &&
          !read("docs/a.txt") &&
          permissions.manage("audit").find("fixture contents") == std::string::npos,
          "Established readers observe revocation and audit omits contents");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    check(false, "Plugin permission lifecycle");
  }
  host_env("CODEX_CPP_HOME", old_profile);
}
