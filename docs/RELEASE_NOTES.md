TUI `/team` now configures the native team server, accounts and access tokens.
It shares settings with WebUI, and stops its own server when the TUI exits.

Download the executable for your OS/CPU. Windows x86 means 32-bit; x64 and
ARM64 are separate downloads. macOS includes Intel and Apple Silicon builds.
Linux includes x64 and ARM64 builds. On macOS/Linux, run `chmod +x codex-cpp-*`.
Use `./codex-cpp-<platform> web --root /path/to/project` or start the TUI directly.

WebUI, libgit2 and libcurl are embedded; Windows uses static CRT and system TLS,
macOS uses static OpenSSL with system certificate trust, Linux uses static
musl/OpenSSL and embedded bubblewrap.
No Python, Node, system Git, curl executable, extra DLLs or web directory is
required for the built-in UI and model transport. `--web-dir` overrides the UI.
Use `--licenses` for embedded third-party notices; SHA256SUMS.txt verifies downloads.

Platform requirements and limits:
- Windows 10/11; its restricted command execution/AppContainer backend is not
  implemented. Chat, native file tools and the editor work; restricted shell,
  sandboxed Git and team development operations report unavailable. This release
  does not silently disable isolation. Windows is not yet a complete team server.
- macOS 12+ and the system sandbox-exec facility; Apple may restrict downloaded
  unsigned applications. These builds are ad-hoc signed, not Apple notarized.
- Linux kernel with enabled unprivileged user namespaces. Embedded bubblewrap
  needs an executable temporary directory; distribution security policies can
  still deny namespaces. System CA certificates are needed for HTTPS on Linux.
- Shell tasks use the OS shell and whatever developer tools your task needs.
  Optional SSH, clipboard commands, MCP servers and plugins have their own
  dependencies. HTTPS team access requires a separately configured reverse proxy.

Each of the seven executables is tested on its matching architecture from an
isolated temporary directory with no curl/Git in PATH before release publication.
