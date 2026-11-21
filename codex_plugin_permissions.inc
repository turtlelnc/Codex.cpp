// Core-owned permissions for isolated workers. Only trusted UI code supplies
// a decision. Plugin data never selects the permission scope or duration.
namespace plugin_permissions {
enum class Decision { Deny, Once, SessionFile, SessionDirectory, PersistentFile, PersistentDirectory };
using Prompt = std::function<Decision(const std::string&, const fs::path&, const fs::path&, const fs::path&)>;

class FileRead {
  fs::path store_;
  Json session_grants_ = native_web::arr();
  std::string session_;

  static Json load(const fs::path& path) {
    if (!fs::exists(fs::symlink_status(path))) return native_web::arr();
    const auto value = plugin_json(native_web::read_bytes(path, 1024 * 1024));
    if (!value || value->kind != Json::Array) throw std::runtime_error("Invalid plugin permission state");
    return *value;
  }

  void audit(const std::string& identity, const fs::path& root, const fs::path& scope,
             const char* event, const std::string& decision = {}) const {
    const auto path = store_ / "audit.json";
    auto records = load(path);
    records.items.push_back(plugin_object({
      {"at",plugin_string(now_iso8601())}, {"permission",plugin_string("file.read")},
      {"event",plugin_string(event)}, {"decision",plugin_string(decision)},
      {"plugin",plugin_string(identity)}, {"workspace",plugin_string(root.u8string())},
      {"scope",plugin_string(scope.u8string())}
    }));
    if (records.items.size() > 256) records.items.erase(records.items.begin(), records.items.end() - 256);
    native_web::atomic_save(path, records);
  }

  static bool matches(const Json& grants, const std::string& identity,
                      const fs::path& root, const fs::path& requested) {
    for (const auto& grant : grants.items) {
      if (plugin_text(grant,"plugin") != identity ||
          plugin_text(grant,"workspace") != root.u8string() ||
          plugin_text(grant,"permission") != "file.read") continue;
      const auto scope = fs::u8path(plugin_text(grant,"scope"));
      if (!plugin_flag(grant,"directory")) {
        if (scope == requested) return true;
        continue;
      }
      const auto mismatch = std::mismatch(scope.begin(), scope.end(), requested.begin(), requested.end());
      if (mismatch.first == scope.end()) return true;
    }
    return false;
  }

public:
  explicit FileRead(const fs::path& workspace) {
    store_ = fs::weakly_canonical(own_auth_path().parent_path()) / "isolated-plugin-permissions";
    if (within_root(workspace, store_))
      throw std::runtime_error("Plugin permission state must live outside the workspace");
    native_web::private_dir(store_);
    store_ = fs::canonical(store_);
    if (within_root(workspace, store_))
      throw std::runtime_error("Plugin permission state must live outside the workspace");
    process_protected_paths.push_back(store_);
  }

  void set_session(const std::string& session) {
    if (session_ != session) session_grants_.items.clear();
    session_ = session;
  }

  std::optional<std::string> read(const std::string& identity, const fs::path& root,
                                  const fs::path& relative, const Prompt& prompt, size_t limit) {
    const auto path = store_ / "grants.json";
    {
      portable::Lease lock(store_, true);
      audit(identity, root, relative, "request");
      if (matches(session_grants_, identity, root, relative) ||
          matches(load(path), identity, root, relative)) {
        auto bytes = creator_read(root, relative.u8string(), limit);
        audit(identity, root, relative, "use", "existing");
        return bytes;
      }
    }
    const auto directory = relative.parent_path();
    const auto choice = prompt ? prompt(identity, root, relative, directory) : Decision::Deny;
    portable::Lease lock(store_, true);
    if (choice == Decision::Deny) {
      audit(identity, root, relative, "decision", "deny");
      return {};
    }
    const bool persistent = choice == Decision::PersistentFile || choice == Decision::PersistentDirectory;
    const bool subtree = choice == Decision::SessionDirectory || choice == Decision::PersistentDirectory;
    const auto scope = subtree ? directory : relative;
    audit(identity, root, scope, "decision", persistent ? "persistent" : choice == Decision::Once ? "once" : "session");
    auto bytes = creator_read(root, relative.u8string(), limit);
    if (choice != Decision::Once) {
      auto grant = plugin_object({
        {"id",plugin_string(random_id("grant"))}, {"permission",plugin_string("file.read")},
        {"plugin",plugin_string(identity)}, {"workspace",plugin_string(root.u8string())},
        {"scope",plugin_string(scope.u8string())}, {"directory",native_web::boolean(subtree)}
      });
      auto grants = persistent ? load(path) : session_grants_;
      if (grants.items.size() >= 512) throw std::runtime_error("Too many plugin grants");
      grants.items.push_back(std::move(grant));
      if (persistent) native_web::atomic_save(path, grants);
      else session_grants_ = std::move(grants);
    }
    audit(identity, root, relative, "use", "approved");
    return bytes;
  }

  std::string manage(const std::string& command) {
    portable::Lease lock(store_, true);
    const auto path = store_ / "grants.json";
    auto persistent = load(path);
    if (command == "audit") return json_dump(load(store_ / "audit.json"));
    if (command.empty() || command == "list")
      return json_dump(plugin_object({{"session",session_grants_},{"persistent",persistent}}));
    if (command.rfind("revoke ", 0) != 0)
      return "Usage: /plugin-permissions list | audit | revoke GRANT_ID";
    const auto id = trim(command.substr(7));
    for (auto* grants : {&session_grants_, &persistent}) {
      auto found = std::find_if(grants->items.begin(), grants->items.end(),
          [&](const Json& grant) { return plugin_text(grant,"id") == id; });
      if (found == grants->items.end()) continue;
      const auto grant = *found;
      grants->items.erase(found);
      if (grants == &persistent) native_web::atomic_save(path, persistent);
      audit(plugin_text(grant,"plugin"), fs::u8path(plugin_text(grant,"workspace")),
            fs::u8path(plugin_text(grant,"scope")), "revoke", id);
      return "Plugin permission revoked";
    }
    return "Grant not found";
  }
};
}
