#include "plugin/isolated.h"
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <cstddef>
#endif

namespace isolated_plugin {
#if defined(__linux__)
static bool restrict_process_creation() {
#if defined(__x86_64__) && !defined(__ILP32__)
  constexpr unsigned architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
  constexpr unsigned architecture = AUDIT_ARCH_AARCH64;
#else
  return false;
#endif
#if (defined(__x86_64__) && !defined(__ILP32__)) || defined(__aarch64__)
  constexpr unsigned denied = SECCOMP_RET_ERRNO | EPERM;
  sock_filter instructions[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
#if defined(__x86_64__)
    // Reject the alternate x32 syscall ABI before comparing syscall numbers.
    BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, denied),
#endif
#ifdef SYS_fork
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_fork, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, denied),
#endif
#ifdef SYS_vfork
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_vfork, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, denied),
#endif
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, denied),
    // clone3 is 435 on the supported ABIs, including with older libc headers.
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 435, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, denied),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{static_cast<unsigned short>(sizeof instructions / sizeof instructions[0]), instructions};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}
#endif
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
  if (setrlimit(RLIMIT_AS, &memory) || !restrict_process_creation()) return 2;
#endif
  void* library = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (!library) return 3;
  const auto v2 = reinterpret_cast<decltype(&codex_plugin_read_v2)>(
      dlsym(library, "codex_plugin_read_v2"));
  bool requested = false;
  auto read = [](void* context, char* buffer, size_t capacity, size_t* actual) -> int {
    auto& used = *static_cast<bool*>(context);
    if (used || !buffer || !actual || capacity < input_limit) return 1;
    used = true;
    std::cout << "READ\n" << std::flush;
    std::cin.read(buffer, static_cast<std::streamsize>(input_limit));
    if (std::cin.bad()) return 1;
    *actual = static_cast<size_t>(std::cin.gcount());
    return 0;
  };
  int result;
  if (v2) {
    const CodexIsolatedReadV2 broker{&requested, read};
    result = v2(&broker);
  } else {
    const auto v1 = reinterpret_cast<decltype(&codex_plugin_read_v1)>(
        dlsym(library, "codex_plugin_read_v1"));
    if (!v1) return 3;
    std::string input(input_limit, '\0');
    size_t size = 0;
    if (read(&requested, input.data(), input.size(), &size)) return 2;
    result = v1(input.data(), size);
  }
  return result == 0 ? 0 : 4;
#else
  (void)argc; (void)argv;
  return 126;
#endif
}

struct ManifestModule {
  std::string name, bytes, fingerprint;
};

static ManifestModule read_manifest(const fs::path& path, const std::string& reviewed) {
  std::map<std::string,std::string> captured;
  const auto report = creator_check(path.parent_path(), path.u8string(), &captured, true);
  const auto manifest = plugin_json(captured.at(path.filename().u8string()));
  const auto tool = manifest ? manifest->get("isolated_read") : nullptr;
  if (!tool)
    throw std::runtime_error("Legacy plugin execution is disabled. Migrate to an isolated_read manifest.");
  const auto fingerprint = plugin_text(report, "review_sha256");
  if (!reviewed.empty() && fingerprint != reviewed)
    throw std::runtime_error("Isolated plugin review mismatch");
  if (!plugin_flag(report, "built")) throw std::runtime_error("Isolated plugin library is missing");
  return {tool->str(), std::move(captured.at(plugin_text(report, "library"))), fingerprint};
}

class ReadTool final : public ToolRuntime {
  std::string library_, identity_;
  std::string tool_name_ = "isolated_plugin_read";
public:
  plugin_permissions::FileRead permissions;
  plugin_permissions::Prompt prompt;
  ReadTool(const fs::path& library, const fs::path& workspace) : permissions(workspace) {
    if (!library.is_absolute()) throw std::runtime_error("Isolated plugin requires an absolute library path");
    const auto parent = fs::canonical(library.parent_path());
    if (sensitive_path(parent / library.filename()))
      throw std::runtime_error("Protected isolated plugin path");
    library_ = creator_read(parent, library.filename().u8string(), 32 * 1024 * 1024);
    identity_ = native_web::digest(library_);
  }
  ReadTool(ManifestModule module, const fs::path& workspace)
      : library_(std::move(module.bytes)), identity_(std::move(module.fingerprint)),
        tool_name_(std::move(module.name)), permissions(workspace) {}
  std::string name() const override { return tool_name_; }
  std::string spec_json() const override {
    return one_arg_tool_spec(tool_name_.c_str(),
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
      const auto relative = fs::u8path(*path);
      if (relative.empty() || relative.is_absolute() || path->find_first_of("\\:") != std::string::npos)
        return {2, "Isolated plugin requires a relative file path"};
      for (const auto& part : relative)
        if (part.empty() || part == "." || part == "..")
          return {2, "Isolated plugin path has an invalid component"};
      const auto target = workspace / relative;
      if (!within_root(workspace, target) || sensitive_path(target))
        return {126, "Isolated plugin file is outside the workspace or protected"};
      for (const auto& part : relative) {
        const auto folded = portable::folded(part.u8string());
        if (folded == ".git" || folded == ".codex_cpp" || folded == ".codex-cpp")
          return {126, "Isolated plugin file is protected"};
      }

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
      policy.workspace = workspace;
      policy.no_fork = true;
      policy.reads.push_back(library);

      ManagedProcess process;
      if (!process.start({preview_self.u8string(), "--isolated-plugin-worker", library.u8string()},
                         policy, error))
        return {126, error};
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      std::string output;
      char buffer[4096];
      bool eof = false, delivered = false;
      std::string request;
      while (std::chrono::steady_clock::now() < deadline) {
        if (tick) tick();
#if !defined(_WIN32)
        if (tui_interrupt_requested) return {130, "Isolated plugin interrupted"};
#endif
        if (!eof) {
          int n;
          {
            TerminalInputScope terminal(watch);
#if !defined(_WIN32)
            if (watch && busy_input && busy_input("", 0)) return {130, "Isolated plugin interrupted"};
#endif
            n = process.receive(buffer, delivered ? sizeof buffer : 1, 25);
          }
          if (n < -1) return {2, "Isolated plugin pipe failed"};
          if (n == -1) eof = true;
          if (n > 0 && !delivered) {
            request.push_back(buffer[0]);
            if (std::string_view("READ\n").substr(0, request.size()) != request)
              return {2, "Invalid isolated plugin request"};
            if (request == "READ\n") {
              const auto input = permissions.read(identity_, workspace, relative, prompt, input_limit);
              if (!input) return {126, "Plugin file.read is not authorized"};
              if (!process.send(*input, error, 1)) return {2, "Isolated plugin input: " + error};
              process.close_input();
              delivered = true;
              deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
          } else if (n > 0) {
            if (output.size() + static_cast<size_t>(n) > output_limit)
              return {2, "Isolated plugin output exceeds 64 KiB"};
            output.append(buffer, static_cast<size_t>(n));
          }
        }
        if (eof) {
          const int status = process.wait(25);
          if (status == 0 && delivered) return {0, std::move(output)};
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
    ReadTool tool(fs::u8path(argv[3]), root);
    const auto result = tool.run("{\"path\":\"" + json_escape(argv[5]) + "\"}", root);
    (result.exit_code ? std::cerr : std::cout) << result.output << '\n';
    return result.exit_code;
  } catch (const std::exception& e) {
    std::cerr << "Isolated plugin: " << e.what() << '\n';
    return 2;
  }
}
}
