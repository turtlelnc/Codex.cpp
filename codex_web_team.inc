// Local-owner service management. Hosted users never receive this dispatch surface.
struct LocalService : Service {
  std::unique_ptr<Child> team;
  bool team_stopping = false;
  std::chrono::steady_clock::time_point stop_at;
  std::string team_error;
  fs::path team_home() const {
    return home / "team-server";
  }
  J settings() {
    return read_json(home / "team-service.json");
  }
  bool team_running() {
    if (team && team_stopping && std::chrono::steady_clock::now() - stop_at > std::chrono::seconds(15)) {
      team->terminate();
      team_error = "服务停止超时，已终止；重启后请检查中断任务。";
    }
    if (team && !team->running()) {
      team.reset();
      team_stopping = false;
    }
    return bool(team);
  }
  void stopped_only() {
    require(!team_running(), "请先停止团队服务，再修改配置或账号。");
  }
  std::vector<std::string> enrolled() {
    std::vector<std::string> names;
    for (const auto &item : field(read_json(team_home() / "server.json"), "users", obj()).fields)
      names.push_back(item.first);
    return names;
  }
  J team_status() {
    auto config = settings();
    std::vector<J> users;
    for (const auto &name : enrolled())
      users.push_back(str(name));
    return obj({{"running", boolean(team_running())},
                {"sandbox_available", boolean(sandbox_available())},
                {"stopping", boolean(team_stopping)},
                {"origin", str(text(config, "origin"))},
                {"port", field(config, "port", number(8766))},
                {"users", arr(users)},
                {"state_dir", str(team_home().u8string())},
                {"error", str(team_error)}});
  }
  void stop_team() {
    if (!team_running() || team_stopping)
      return;
    team_stopping = true;
    stop_at = std::chrono::steady_clock::now();
#if defined(_WIN32)
    TerminateJobObject(team->job, 130);
#else
    ::kill(team->pid, SIGTERM);
#endif
  }
  void begin_shutdown() override {
    Service::begin_shutdown();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    stop_team();
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (team_running() && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    team.reset();
  }
  ~LocalService() override {
    begin_shutdown();
  }
  J dispatch(const std::string &user, const J &request) override {
    auto op = text(request, "op");
    if (op.rfind("team_", 0) != 0)
      return Service::dispatch(user, request);
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (op == "team_get")
      return team_status();
    if (op == "team_save") {
      stopped_only();
      auto origin = text(request, "origin");
      while (!origin.empty() && origin.back() == '/')
        origin.pop_back();
      require(origin.rfind("https://", 0) == 0 && origin.size() <= 2048 && origin.size() > 8 &&
                  origin.substr(8).find_first_of("/@?#\r\n\t ") == std::string::npos,
              "请填写不含路径、账号或查询参数的 HTTPS 地址。");
      auto value = field(request, "port");
      require(value.kind == J::Other && value.value.size() <= 5 && !value.value.empty() &&
                  std::all_of(value.value.begin(), value.value.end(),
                              [](unsigned char c) { return std::isdigit(c); }),
              "端口必须是整数。");
      int next = std::stoi(value.value);
      require(next >= 1 && next <= 65535 && next != port, "团队端口必须有效，且不能与本地网页端口相同。");
      atomic_save(home / "team-service.json", obj({{"origin", str(origin)}, {"port", number(next)}}));
      team_error.clear();
      return team_status();
    }
    if (op == "team_account" || op == "team_revoke") {
      stopped_only();
      auto name = text(request, "username");
      require(identifier(name), "用户名只允许字母、数字、下划线和连字符，最长 128 字符。");
      auto names = enrolled();
      bool exists = std::find(names.begin(), names.end(), name) != names.end();
      require(!exists || flag(request, "confirmed"), "账号已存在，请使用重置令牌操作并明确确认。");
      require(op != "team_revoke" || (exists && flag(request, "confirmed")), "请确认要停用的账号。");
      auto out = command({binary.u8string(), "serve", "--state-dir", team_home().u8string(),
                          op == "team_account" ? "--add-user" : "--revoke-user", name},
                         10);
      if (op == "team_revoke")
        return obj({{"updated", boolean(true)}});
      std::string prefix = "Login token (save privately): ";
      auto position = out.find(prefix);
      require(position != std::string::npos, "未取得新令牌，请停止其他使用该数据目录的实例后重试。");
      auto token = trim(out.substr(position + prefix.size()));
      require(token.size() == 64 &&
                  std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c); }),
              "令牌生成失败。");
      return obj({{"username", str(name)}, {"token", str(token)}});
    }
    if (op == "team_start") {
      stopped_only();
      auto config = settings();
      auto origin = text(config, "origin");
      require(!origin.empty(), "请先保存团队服务地址和端口。");
      require(!enrolled().empty(), "请先登记至少一个登录账号。");
      auto value = field(config, "port");
      require(value.kind == J::Other, "端口配置损坏，请重新保存。");
      int next = std::stoi(value.value);
      require(next >= 1 && next <= 65535 && next != port, "端口无效，请重新保存配置。");
      std::vector<std::string> args = {binary.u8string()};
#if defined(__linux__)
      args.insert(args.end(), {"--native-child", std::to_string(::getpid())});
#endif
      args.insert(args.end(), {"serve", "--state-dir", team_home().u8string(), "--origin", origin, "--port",
                               std::to_string(next), "--web-dir", web.u8string()});
      auto candidate = std::make_unique<Child>();
      candidate->start(args);
      team = std::move(candidate);
      team_error.clear();
      team_stopping = false;
      std::string output;
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
      while (team->running() && std::chrono::steady_clock::now() < until) {
        output += team->receive();
        if (output.find("Codex.cpp native server web:") != std::string::npos)
          return team_status();
        require(output.size() < 65536, "团队服务启动输出过大。");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      team.reset();
      team_error = "团队服务未能启动，请检查端口是否占用，或数据目录是否被其他实例使用。";
      throw std::runtime_error(team_error);
    }
    if (op == "team_stop") {
      require(flag(request, "confirmed"), "请确认停止会中断团队任务。");
      stop_team();
      return team_status();
    }
    throw std::runtime_error("Unknown team setting");
  }
};
