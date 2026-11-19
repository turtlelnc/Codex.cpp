#include "plugin/isolated.h"
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace isolated_plugin {
constexpr size_t input_limit = 64 * 1024;
constexpr size_t output_limit = 64 * 1024;

static int worker(int argc, char** argv) {
#if defined(__linux__) || defined(__APPLE__)
  if (argc != 3) return 2;
  const rlimit cpu{3, 3}, file{1024 * 1024, 1024 * 1024}, core{0, 0};
  if (setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_FSIZE, &file) ||
      setrlimit(RLIMIT_CORE, &core)) return 2;
#if defined(__linux__)
  const rlimit memory{512ull * 1024 * 1024, 512ull * 1024 * 1024};
  if (setrlimit(RLIMIT_AS, &memory)) return 2;
#endif
  std::string input(input_limit + 1, '\0');
  std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
  if (!std::cin.eof() || std::cin.bad()) return 2;
  input.resize(static_cast<size_t>(std::cin.gcount()));
  void* library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (!library) return 3;
  const auto handler = reinterpret_cast<decltype(&codex_plugin_read_v1)>(
      dlsym(library, "codex_plugin_read_v1"));
  if (!handler) return 3;
  const int result = handler(input.data(), input.size());
  return result == 0 ? 0 : 4;
#else
  (void)argc; (void)argv;
  return 126;
#endif
}

class ReadTool final : public ToolRuntime {
  std::string library_;
public:
  explicit ReadTool(const fs::path& library) {
    if (!library.is_absolute()) throw std::runtime_error("Isolated plugin requires an absolute library path");
    const auto parent = fs::canonical(library.parent_path());
    if (sensitive_path(parent / library.filename()))
      throw std::runtime_error("Protected isolated plugin path");
    library_ = creator_read(parent, library.filename().u8string(), 32 * 1024 * 1024);
  }
  std::string name() const override { return "isolated_plugin_read"; }
  std::string spec_json() const override {
    return one_arg_tool_spec("isolated_plugin_read",
        "Send one workspace file (up to 64 KiB) to the explicitly enabled isolated plugin.", "path");
  }
  ToolResult run(const std::string& args, const fs::path& root) override {
    return execute(args, root, SandboxMode::ReadOnly, {}, false);
  }
  ToolResult execute(const std::string& args, const fs::path& root, SandboxMode,
                     const std::function<void()>& tick, bool watch) override {
    try {
      if (execution.unsafe || !sandbox_available())
        return {126, "Isolated plugin requires an OS sandbox"};
      const auto path = json_string_field(args, "path");
      if (!path || path->find('\0') != std::string::npos)
        return {2, "Isolated plugin requires a relative file path"};
      const auto workspace = fs::canonical(root);
      const auto target = workspace / fs::u8path(*path);
      if (!within_root(workspace, target) || sensitive_path(target))
        return {126, "Isolated plugin file is outside the workspace or protected"};
      for (const auto& part : fs::u8path(*path))
        if (part == ".git" || part == ".codex_cpp" || part == ".codex-cpp")
          return {126, "Isolated plugin file is protected"};
      const auto input = creator_read(workspace, *path, input_limit);

      const auto snapshot = execution.private_dir / random_id("isolated-plugin");
      if (!fs::create_directory(snapshot)) return {2, "Cannot create plugin snapshot"};
      struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); }
      } cleanup{snapshot};
      const auto library = snapshot / "module.bin";
      std::string error;
      if (!write_all(library, library_, error)) return {2, error};
      ProcessPolicy policy;
      policy.role = ProcessRole::IsolatedPlugin;
      policy.no_fork = true;
      policy.reads.push_back(library);

      ManagedProcess process;
      if (!process.start({preview_self.u8string(), "--isolated-plugin-worker", library.u8string()},
                         policy, error))
        return {126, error};
      if (!process.send(input, error, 1)) return {2, "Isolated plugin input: " + error};
      process.close_input();

      TerminalInputScope terminal(watch);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      std::string output;
      char buffer[4096];
      bool eof = false;
      while (std::chrono::steady_clock::now() < deadline) {
        if (tick) tick();
#if !defined(_WIN32)
        if (tui_interrupt_requested || (watch && busy_input && busy_input("", 0)))
          return {130, "Isolated plugin interrupted"};
#endif
        if (!eof) {
          const int n = process.receive(buffer, sizeof buffer, 25);
          if (n < -1) return {2, "Isolated plugin pipe failed"};
          if (n == -1) eof = true;
          if (n > 0) {
            if (output.size() + static_cast<size_t>(n) > output_limit)
              return {2, "Isolated plugin output exceeds 64 KiB"};
            output.append(buffer, static_cast<size_t>(n));
          }
        }
        if (eof) {
          const int status = process.wait(25);
          if (status == 0) return {0, std::move(output)};
          if (status >= 0) return {2, "Isolated plugin worker failed (exit " + std::to_string(status) + ")"};
        }
      }
      return {124, "Isolated plugin timed out"};
    } catch (const std::exception& e) {
      return {2, std::string("Isolated plugin: ") + e.what()};
    }
  }
};

static int cli(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "Usage: codex-cpp plugin isolated-run ABSOLUTE_LIBRARY WORKSPACE RELATIVE_FILE\n";
    return 2;
  }
  try {
    const auto root = fs::canonical(fs::u8path(argv[4]));
    execution.initialize(root);
    ReadTool tool(fs::u8path(argv[3]));
    const auto result = tool.run("{\"path\":\"" + json_escape(argv[5]) + "\"}", root);
    (result.exit_code ? std::cerr : std::cout) << result.output << '\n';
    return result.exit_code;
  } catch (const std::exception& e) {
    std::cerr << "Isolated plugin: " << e.what() << '\n';
    return 2;
  }
}
}
