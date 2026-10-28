// Hosted-mode policy and project coordination use the same native App runtime.
struct HostedService : Service {
  J database;
  fs::path database_path;
  Options initial;
  std::map<std::string, std::shared_ptr<App>> apps;
  std::map<std::string, std::thread> workers;
  std::set<std::string> active_projects;
  bool stopping = false;
  std::thread scheduler;
#if defined(_WIN32)
  HANDLE state_lock = INVALID_HANDLE_VALUE;
#else
  int state_lock = -1;
#endif
  bool hosted() const override {
    return true;
  }
  std::string mode() const override {
    return "server";
  }
  HostedService(fs::path state, Options base) : initial(std::move(base)) {
    home = std::move(state);
    private_dir(home);
    database_path = home / "server.json";
#if defined(_WIN32)
    state_lock = CreateFileW((home / "service.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(state_lock != INVALID_HANDLE_VALUE, "Stop the service before editing accounts");
#else
    state_lock = ::open((home / "service.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(state_lock >= 0 && flock(state_lock, LOCK_EX | LOCK_NB) == 0,
            "Stop the service before editing accounts");
#endif
    database = read_json(database_path);
    for (auto key : {"users", "projects"})
      if (!database.get(key))
        database.fields[key] = obj();
    if (!database.get("jobs"))
      database.fields["jobs"] = arr();
    for (auto &job : database.fields["jobs"].items)
      if (text(job, "status") == "running") {
        job.fields["status"] = str("interrupted");
        auto id = text(job, "project");
        if (database.fields["projects"].get(id))
          database.fields["projects"].fields[id].fields["blocked"] = boolean(true);
      }
    persist();
  }
  ~HostedService() override {
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      stopping = true;
      for (auto &[key, application] : apps) {
        std::lock_guard<std::recursive_mutex> app_lock(application->mutex);
        application->stopping = true;
        if (application->child)
          application->child->terminate();
      }
    }
    if (scheduler.joinable())
      scheduler.join();
    for (auto &[key, thread] : workers)
      if (thread.joinable())
        thread.join();
#if defined(_WIN32)
    if (state_lock != INVALID_HANDLE_VALUE)
      CloseHandle(state_lock);
#else
    if (state_lock >= 0)
      ::close(state_lock);
#endif
  }
  void begin_shutdown() override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    stopping = true;
    for (auto &[key, application] : apps) {
      std::lock_guard<std::recursive_mutex> app_lock(application->mutex);
      application->stopping = true;
      if (application->child)
        application->child->terminate();
    }
  }
  void before_listen() override {
    scheduler = std::thread([this]() {
      while (true) {
        {
          std::lock_guard<std::recursive_mutex> lock(mutex);
          if (stopping)
            return;
          std::set<std::string> queued;
          for (auto &job : database.fields["jobs"].items)
            if (text(job, "status") == "queued")
              queued.insert(text(job, "project"));
          for (auto &id : queued)
            if (!flag(database.fields["projects"].fields[id], "blocked"))
              start_queue(id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  void persist() {
    atomic_save(database_path, database);
  }
  bool login(const std::string &user, const std::string &password) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto item = database.fields["users"].get(user);
    return item && same(text(*item, "token_hash"), digest(password));
  }
  std::string add_user(const std::string &user) {
    require(identifier(user), "Use an account identifier of at most 128 letters/digits/_/-");
    auto token = entropy();
    database.fields["users"].fields[user] = obj({{"token_hash", str(digest(token))}});
    persist();
    return token;
  }
  bool member(const std::string &user, const J &project) {
    auto members = field(project, "members", arr());
    return std::any_of(members.items.begin(), members.items.end(),
                       [&](const J &item) { return item.str() == user; });
  }
  J &project(const std::string &user, const std::string &id) {
    auto found = database.fields["projects"].fields.find(id);
    require(found != database.fields["projects"].fields.end() && member(user, found->second),
            "Project access denied");
    return found->second;
  }
  std::shared_ptr<App> app(const std::string &user, const std::string &id, bool personal) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto key = id + (personal ? "/personal/" + user : "/shared");
    auto found = apps.find(key);
    if (found != apps.end())
      return found->second;
    auto root = home / "workspaces" / key;
    fs::create_directories(root);
    auto state = home / "contexts" / key;
    auto result = std::make_shared<App>(root, binary, state, initial, false, true);
    apps[key] = result;
    return result;
  }
  fs::path repository(const std::string &user, const std::string &id, bool personal) {
    auto p = home / "repositories" / id / (personal ? "personal-" + user : "shared");
    fs::create_directories(p);
    return p;
  }
  struct GitResult {
    int code;
    std::string out;
  };
  GitResult git(const std::string &user, const std::string &id, bool personal, std::vector<std::string> tail,
                bool allow_failure = false, const std::vector<fs::path> &reads = {}) {
    auto application = app(user, id, personal);
    auto repo = repository(user, id, personal);
#ifdef CODEX_WITH_LIBGIT2
    std::vector<std::string> args = {binary.u8string(), "--native-git-command", repo.u8string(),
                                     application->workspace.root.u8string()};
    args.insert(args.end(), tail.begin(), tail.end());
#else
    std::vector<std::string> args = {"git",
                                     "--git-dir=" + repo.u8string(),
                                     "--work-tree=" + application->workspace.root.u8string(),
                                     "-c",
                                     "core.bare=false",
                                     "-c",
                                     "core.hooksPath=/dev/null",
                                     "-c",
                                     "core.fsmonitor=false",
                                     "-c",
                                     "user.name=Codex.cpp",
                                     "-c",
                                     "user.email=codex@localhost"};
    if (!tail.empty() && tail[0] == "init")
      args = {"git", "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false"};
    args.insert(args.end(), tail.begin(), tail.end());
#endif
    ManagedProcess process;
    auto policy = ProcessPolicy::tool(application->workspace.root, true);
    policy.reads = reads;
    policy.writes = {repo};
    std::string why;
    require(process.start(args, policy, why, true), why);
    process.close_input();
    std::string output;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    char buffer[8192];
    while (true) {
      require(!stopping_service, "Service is stopping");
      int n = process.receive(buffer, sizeof buffer, 100);
      if (n > 0) {
        output.append(buffer, static_cast<size_t>(n));
        require(output.size() <= 4 * 1024 * 1024, "Git output exceeds limit");
      }
      if (n == -1)
        break;
      require(std::chrono::steady_clock::now() < deadline, "Git operation timed out");
    }
    int code = process.wait(5000);
    if (code && !allow_failure)
      std::cerr << "Native Git diagnostic: " << output.substr(0, 2000) << '\n';
    if (!allow_failure)
      require(code == 0, "Git operation refused; inspect workspace and retry explicitly");
    return {code, output};
  }
  void init_repository(const std::string &user, const std::string &id, bool personal) {
    auto path = repository(user, id, personal);
    if (fs::exists(path / "HEAD"))
      return;
    git(user, id, personal,
        {"init", "--bare", "--initial-branch=" + std::string(personal ? "personal" : "shared"),
         path.u8string()});
    auto exclude = path / "info/exclude";
    fs::create_directories(exclude.parent_path());
    {
      std::ofstream output(exclude, std::ios::app);
      output << "\n.codex_cpp/\n.codex-cpp/\n";
      output.close();
      require(static_cast<bool>(output), "Cannot protect Agent state from Git publication");
    }
    if (personal) {
      auto shared = repository(user, id, false);
      git(user, id, true, {"fetch", "--no-tags", shared.u8string(), "shared"}, false, {shared});
      git(user, id, true, {"reset", "--hard", "FETCH_HEAD"});
    } else
      git(user, id, false, {"commit", "--allow-empty", "-m", "Initialize shared project"});
    refresh_metadata(user, id, personal);
  }
  void refresh_metadata(const std::string &user, const std::string &id, bool personal) {
    auto root = app(user, id, personal)->workspace.root;
    auto repo = repository(user, id, personal);
    auto dot = root / ".git";
    std::error_code ec;
    if (fs::exists(dot) || fs::is_symlink(dot)) {
      auto history = home / "metadata-history";
      fs::create_directories(history);
      fs::rename(dot, history / entropy());
    }
    fs::copy(repo, dot, fs::copy_options::recursive);
    std::ofstream config(dot / "config", std::ios::app);
    config << "\n[core]\n bare = false\n hooksPath = /dev/null\n fsmonitor = false\n";
  }
  std::string head(const std::string &user, const std::string &id, bool personal,
                   const std::string &ref = "HEAD") {
    return trim(git(user, id, personal, {"rev-parse", ref}).out);
  }
  std::vector<std::string> untracked(const std::string &user, const std::string &id, bool personal) {
    auto source = git(user, id, personal, {"ls-files", "--others", "--exclude-standard", "-z"}).out;
    std::istringstream input(source);
    std::vector<std::string> names;
    std::string name;
    while (std::getline(input, name, '\0'))
      names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
  }
  std::string fingerprint(const std::string &user, const std::string &id, bool personal) {
    auto application = app(user, id, personal);
    auto tracked = git(user, id, personal, {"diff", "HEAD", "--no-ext-diff", "--no-textconv"}).out;
    auto status = git(user, id, personal, {"status", "--porcelain"}).out;
    std::string source =
        std::to_string(tracked.size()) + ":" + tracked + std::to_string(status.size()) + ":" + status;
    size_t total = source.size();
    for (auto &name : untracked(user, id, personal)) {
      auto image = application->workspace.untracked_image(name);
      total += image.size();
      require(total <= 64 * 1024 * 1024, "Review exceeds 64 MiB");
      source += std::to_string(name.size()) + ":" + name + digest(image);
    }
    return digest(source + head(user, id, personal));
  }
  J review(const std::string &user, const std::string &id, bool personal) {
    auto result = git(user, id, personal, {"diff", "HEAD", "--no-ext-diff", "--no-textconv"});
    std::vector<J> names;
    for (auto &name : untracked(user, id, personal))
      names.push_back(str(name));
    return obj({{"head", str(head(user, id, personal))},
                {"diff", str(result.out.substr(0, 524288))},
                {"fingerprint", str(fingerprint(user, id, personal))},
                {"truncated", boolean(result.out.size() > 524288)},
                {"untracked", arr(names)}});
  }
  J merge(const std::string &user, const std::string &id, bool personal, const std::string &ref) {
    require(trim(git(user, id, personal, {"status", "--porcelain"}).out).empty(),
            "Commit or discard current changes before merging");
    auto result = git(user, id, personal, {"merge", "--no-ff", "--no-edit", ref}, true);
    J outcome;
    if (result.code) {
#ifdef CODEX_WITH_LIBGIT2
      require(result.code == 1, "libgit2 merge failed; inspect workspace before retrying");
      auto paths = result.out;
#else
      auto paths = git(user, id, personal, {"diff", "--name-only", "--diff-filter=U"}, true).out;
      git(user, id, personal, {"merge", "--abort"}, true);
#endif
      require(!fs::exists(repository(user, id, personal) / "MERGE_HEAD"),
              "Merge abort incomplete; administrator must inspect repository before recovery");
      std::vector<J> files;
      std::istringstream input(paths);
      std::string line;
      while (std::getline(input, line))
        files.push_back(str(line));
      outcome = obj({{"merged", boolean(false)},
                     {"conflicts", arr(files)},
                     {"error", str("Merge aborted; original target preserved")}});
    } else
      outcome = obj({{"merged", boolean(true)}, {"head", str(head(user, id, personal))}});
    refresh_metadata(user, id, personal);
    return outcome;
  }
  void start_queue(const std::string &id) {
    if (active_projects.count(id) || stopping || active_projects.size() >= 16)
      return;
    if (workers[id].joinable())
      workers[id].join();
    active_projects.insert(id);
    workers[id] = std::thread([this, id]() {
      while (true) {
        J task;
        size_t index = 0;
        {
          std::lock_guard<std::recursive_mutex> lock(mutex);
          if (stopping || flag(database.fields["projects"].fields[id], "blocked"))
            break;
          auto &jobs = database.fields["jobs"].items;
          for (; index < jobs.size(); ++index)
            if (text(jobs[index], "project") == id && text(jobs[index], "status") == "queued")
              break;
          if (index == jobs.size())
            break;
          task = jobs[index];
          jobs[index].fields["status"] = str("running");
          persist();
        }
        J result;
        std::string status = "completed";
        try {
          std::string user = text(task, "actor"), op = text(task, "op");
          J body = field(task, "request", obj());
          bool personal = text(task, "scope") == "personal";
          std::shared_ptr<App> application;
          {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            require(database.fields["users"].get(user) != nullptr, "Account revoked");
            project(user, id);
            application = app(user, id, personal);
          }
          if (op == "send" || op == "login") {
            {
              std::lock_guard<std::recursive_mutex> lock(mutex);
              require(!stopping, "Service is stopping");
              body.fields["request_id"] = str(text(task, "id"));
              result = application->rpc(body);
            }
            while (true) {
              {
                std::lock_guard<std::recursive_mutex> lock(application->mutex);
                if (!application->busy) {
                  require(application->error.empty(), application->error);
                  break;
                }
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
          } else if (op == "checkpoint" || op == "publish") {
            if (personal && fs::exists(application->workspace.root / ".git/HEAD")) {
              git(user, id, true, {"fetch", (application->workspace.root / ".git").u8string(), "HEAD"});
              git(user, id, true, {"reset", "--soft", "FETCH_HEAD"});
            }
            require(!text(body, "message").empty() && text(body, "message").size() <= 1000,
                    "Commit message required");
            if (op == "checkpoint")
              require(text(body, "fingerprint") == fingerprint(user, id, false),
                      "Files changed since review; confirm again");
            git(user, id, personal, {"add", "-A"});
            auto changed = git(user, id, personal, {"diff", "--cached", "--quiet"}, true);
            if (changed.code)
              git(user, id, personal, {"commit", "-m", text(body, "message")});
            if (personal) {
              auto source = repository(user, id, true);
              git(user, id, false, {"fetch", source.u8string(), "HEAD:refs/heads/users/" + user}, false,
                  {source});
            }
            refresh_metadata(user, id, personal);
            result = obj({{"head", str(head(user, id, personal))}});
          } else if (op == "sync") {
            auto source = repository(user, id, false);
            git(user, id, true, {"fetch", source.u8string(), "shared"}, false, {source});
            result = merge(user, id, true, "FETCH_HEAD");
          } else if (op == "merge") {
            auto source_user = text(body, "username");
            {
              std::lock_guard<std::recursive_mutex> lock(mutex);
              project(source_user, id);
            }
            require(flag(body, "confirmed") &&
                        text(body, "source") == head(user, id, false, "refs/heads/users/" + source_user) &&
                        text(body, "target") == head(user, id, false),
                    "Branches changed; inspect exact commits and confirm again");
            result = merge(user, id, false, "refs/heads/users/" + source_user);
          }
        } catch (const std::exception &e) {
          status = "failed";
          result = obj({{"error", str(e.what())}});
        }
        {
          std::lock_guard<std::recursive_mutex> lock(mutex);
          auto &row = database.fields["jobs"].items[index];
          if (stopping || fs::exists(repository(text(task, "actor"), id, text(task, "scope") == "personal") /
                                     "MERGE_HEAD")) {
            status = "interrupted";
            database.fields["projects"].fields[id].fields["blocked"] = boolean(true);
          }
          row.fields["status"] = str(status);
          row.fields["result"] = result;
          row.fields.erase("request");
          persist();
        }
      }
      std::lock_guard<std::recursive_mutex> lock(mutex);
      active_projects.erase(id);
    });
  }
  void audit(const std::string &user, const std::string &id, const J &request) {
    auto &log = database.fields["projects"].fields[id].fields["audit"];
    if (log.kind != J::Array)
      log = arr();
    J details = obj();
    for (auto key : {"id", "model", "effort", "permission", "username"})
      if (request.get(key))
        details.fields[key] = field(request, key);
    if (text(request, "op") == "approve")
      details.fields["allow"] = boolean(flag(request, "allow"));
    log.items.push_back(obj({{"actor", str(user)},
                             {"op", field(request, "op")},
                             {"scope", str(text(request, "scope", "shared"))},
                             {"time", number(std::time(nullptr))},
                             {"details", details}}));
    if (log.items.size() > 1000)
      log.items.erase(log.items.begin());
    persist();
  }
  J dispatch(const std::string &user, const J &request) override;
};
J HostedService::dispatch(const std::string &user, const J &request) {
  std::unique_lock<std::recursive_mutex> lock(mutex);
  require(database.fields["users"].get(user) != nullptr, "Account revoked");
  auto op = text(request, "op"), id = text(request, "project");
  if (op == "projects" || op == "list") {
    std::vector<J> names;
    for (auto &[name, p] : database.fields["projects"].fields)
      if (member(user, p))
        names.push_back(str(name));
    return obj({{"projects", arr(names)}});
  }
  if (op == "create") {
    id = text(request, "name", id);
    require(identifier(id) && id.size() <= 40, "Project name must contain 1..40 letters/digits/_/-");
    require(!database.fields["projects"].get(id), "Project already exists");
    require(database.fields["projects"].fields.size() < 100, "Project limit reached");
    database.fields["projects"].fields[id] =
        obj({{"owner", str(user)}, {"members", arr({str(user)})}, {"blocked", boolean(false)}});
    try {
      app(user, id, false);
      init_repository(user, id, false);
    } catch (...) {
      database.fields["projects"].fields.erase(id);
      apps.erase(id + "/shared");
      std::error_code ec;
      fs::remove_all(home / "workspaces" / id, ec);
      fs::remove_all(home / "repositories" / id, ec);
      fs::remove_all(home / "contexts" / id, ec);
      throw;
    }
    persist();
    return obj({{"project", str(id)}});
  }
  auto &info = project(user, id);
  bool owner = text(info, "owner") == user, personal = text(request, "scope") == "personal";
  require(identifier(id), "Invalid project");
  if (op == "invite" || op == "remove") {
    require(owner, "Only the creator can manage members");
    auto target = text(request, "username");
    require(database.fields["users"].get(target) && target != text(info, "owner"),
            "Choose another enrolled account");
    auto &members = info.fields["members"].items;
    if (op == "invite") {
      if (!member(target, info))
        members.push_back(str(target));
    } else {
      for (auto &job : database.fields["jobs"].items)
        if (text(job, "project") == id && text(job, "actor") == target) {
          require(text(job, "status") != "running", "Wait for this member's current task to finish");
          if (text(job, "status") == "queued") {
            job.fields["status"] = str("cancelled");
            job.fields.erase("request");
          }
        }
      members.erase(
          std::remove_if(members.begin(), members.end(), [&](const J &x) { return x.str() == target; }),
          members.end());
    }
    persist();
    audit(user, id, request);
    return obj({{"updated", boolean(true)}});
  }
  if (op == "personal") {
    init_repository(user, id, true);
    return obj({{"ready", boolean(true)}});
  }
  if (op == "project_status") {
    std::vector<J> jobs;
    for (auto &job : database.fields["jobs"].items)
      if (text(job, "project") == id && (text(job, "scope") != "personal" || text(job, "actor") == user)) {
        auto copy = job;
        copy.fields.erase("request");
        copy.fields.erase("hash");
        jobs.push_back(copy);
      }
    if (jobs.size() > 50)
      jobs.erase(jobs.begin(), jobs.end() - 50);
    auto refs =
        git(user, id, false, {"for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads/"}).out;
    J branches = obj();
    std::istringstream input(refs);
    std::string name, sha;
    while (input >> name >> sha)
      branches.fields[name] = str(sha);
    std::vector<J> audit_rows;
    for (auto &row : field(info, "audit", arr()).items)
      if (text(row, "scope") != "personal" || text(row, "actor") == user)
        audit_rows.push_back(row);
    return obj({{"audit", arr(audit_rows)},
                {"owner", field(info, "owner")},
                {"members", field(info, "members")},
                {"blocked", boolean(flag(info, "blocked"))},
                {"busy", boolean(active_projects.count(id) > 0)},
                {"jobs", arr(jobs)},
                {"branches", branches},
                {"head", str(head(user, id, false))}});
  }
  if (op == "recover") {
    require(owner && flag(request, "confirmed"), "Creator must review and confirm recovery");
    require(!active_projects.count(id), "Task still running");
    require(text(request, "fingerprint") == fingerprint(user, id, false),
            "Inspect current files before recovery");
    // Recovery must not discard an unfinished merge or resume an Agent turn whose
    // last tool outcome is unknown. Private turns need the same treatment as shared ones.
    for (const auto &entry : fs::directory_iterator(home / "repositories" / id))
      require(!fs::exists(entry.path() / "MERGE_HEAD"),
              "Resolve or abort the interrupted merge before recovery");
    for (auto &job : database.fields["jobs"].items) {
      if (text(job, "project") != id || text(job, "status") != "interrupted" || flag(job, "recovered"))
        continue;
      auto affected = app(text(job, "actor"), id, text(job, "scope") == "personal");
      {
        std::lock_guard<std::recursive_mutex> app_lock(affected->mutex);
        affected->sid.clear();
        affected->messages = arr();
        affected->error.clear();
        affected->save();
      }
      job.fields["recovered"] = boolean(true);
    }
    info.fields["blocked"] = boolean(false);
    persist();
    audit(user, id, request);
    start_queue(id);
    return obj({{"recovered", boolean(true)}});
  }
  if (op == "diff") {
    init_repository(user, id, personal);
    return review(user, id, personal);
  }
  if (op == "permission")
    require(text(request, "permission") != "full",
            "Hosted service never enables unrestricted Agent execution");
  if (!personal && (op == "provider_save" || op == "provider_delete" || op == "login"))
    require(owner, "Only the creator configures shared credentials");
  if (op == "send" || op == "login" || op == "publish" || op == "sync" || op == "checkpoint" ||
      op == "merge") {
    auto rid = text(request, "request_id");
    require(identifier(rid), "request_id is required");
    require(json_dump(request).size() <= 70000, "Task too large");
    auto signature = digest(json_dump(request));
    size_t pending = 0, total_pending = 0;
    for (auto &job : database.fields["jobs"].items) {
      if (text(job, "project") == id && text(job, "actor") == user && text(job, "rid") == rid) {
        require(text(job, "hash") == signature, "request_id already used for a different task");
        return obj(
            {{"job", field(job, "id")}, {"status", field(job, "status")}, {"duplicate", boolean(true)}});
      }
      if (text(job, "project") == id && (text(job, "status") == "queued" || text(job, "status") == "running"))
        ++pending;
      if (text(job, "status") == "queued" || text(job, "status") == "running")
        ++total_pending;
    }
    require(!flag(info, "blocked"), "Project requires review and recovery after interrupted execution");
    require(total_pending < 128 && database.fields["jobs"].items.size() < 5000,
            "Server task history/queue capacity reached");
    require(pending < 100, "Project queue is full");
    if (op == "publish" || op == "sync") {
      personal = true;
      init_repository(user, id, true);
    }
    if (op == "checkpoint" || op == "merge")
      personal = false;
    if (personal)
      init_repository(user, id, true);
    auto token = entropy();
    auto body = request;
    body.fields["scope"] = str(personal ? "personal" : "shared");
    auto row = obj({{"id", str(token)},
                    {"actor", str(user)},
                    {"project", str(id)},
                    {"scope", str(personal ? "personal" : "shared")},
                    {"op", str(op)},
                    {"rid", str(rid)},
                    {"hash", str(signature)},
                    {"request", body},
                    {"status", str("queued")}});
    database.fields["jobs"].items.push_back(row);
    persist();
    start_queue(id);
    return obj({{"job", str(token)}, {"status", str("queued")}});
  }
  init_repository(user, id, personal);
  auto application = app(user, id, personal);
  if (op == "approve") {
    std::lock_guard<std::recursive_mutex> app_lock(application->mutex);
    auto decision = request.get("allow");
    require(decision && decision->kind == J::Other &&
                (decision->value == "true" || decision->value == "false") && application->child &&
                application->child->running() && !application->pending_id.empty() &&
                same(text(request, "approval"), application->pending_id),
            "Approval is stale");
    audit(user, id, request);
    return application->rpc(request);
  }
  if (std::set<std::string>{"stop", "model", "reasoning", "permission", "provider_save", "provider_delete",
                            "provider_use", "session"}
          .count(op)) {
    auto result = application->rpc(request);
    audit(user, id, request);
    return result;
  }
  if (op == "write") {
    require(!active_projects.count(id), "Wait for project tasks to finish before saving in the editor");
    return application->rpc(request);
  }
  lock.unlock();
  auto result = application->rpc(request);
  if (op == "snapshot" && !personal && !owner) {
    auto &events = result.fields["events"].items;
    events.erase(std::remove_if(events.begin(), events.end(),
                                [](const J &e) { return text(e, "type") == "login.notice"; }),
                 events.end());
  }
  return result;
}
