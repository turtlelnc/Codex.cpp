TUI `/team` now includes native team-server setup and a complete HTTPS team client.
Use "Team projects (HTTPS)" to sign in, create projects, manage members,
configure shared/personal models, publish/sync/commit/merge branches with exact
SHA confirmation, inspect tasks/audit and recover interrupted queues after review.
"Agent tasks on server" queues shared/personal tasks, displays conversations,
approves operations and stops tasks without opening WebUI. Any project member
can approve shared work; the server records the actual actor. Creator-only
membership and shared-credential rules are enforced by the same hosted API.

Credentials are masked and never saved on the TUI client; session cookies stay
in process memory. HTTPS certificate checks remain enabled. Normal chat outside
the team menu still uses the local workspace; team actions use server workspaces.
Local team-service settings remain shared with WebUI, and the TUI stops its own
managed service when it exits.

Download the executable for your OS/CPU. Windows x86 means 32-bit; x64 and
ARM64 are separate downloads. macOS includes Intel and Apple Silicon builds.
Linux includes x64 and ARM64 builds. On macOS, download the `.zip`; on Linux,
download the `.tar.gz`. Each archive contains exactly one executable and preserves
its executable permission. Bare downloads may lose this permission: run
`chmod +x codex-cpp-<platform>` before launching those from Terminal.
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
