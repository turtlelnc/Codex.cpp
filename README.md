# Codex.cpp：原生网页与多人服务

同一个 C++17 程序提供命令行 Agent、本地网页 `web` 和多人网页服务 `serve`。
本地及多人服务均由 C++ 实现，不需要 Python、Node.js 或前端构建工具。
浏览器就是客户端：本地模式操作本机工作区，服务器模式操作服务器上的工作区。
保留文件树、编辑器、工具审批、会话记录、两级模型菜单与自定义模型提供商。

程序始终使用 [内置 Git（libgit2）](docs/LIBGIT2_TRIAL.md)：网页 Diff、多人协作、
`apply_patch` 和 `git_read` / `git_write` 模型工具均无需系统 Git，沿用只读限制、写审批和系统沙箱。

## 编译

需要 CMake、C/C++ 编译器（主程序 C++17），不需要 Python。保留全部 `.inc` 源文件；
CMake 将网页资源直接编入程序；`web/` 目录仅在开发和覆盖页面时使用。
Linux/macOS 构建另需 OpenSSL 开发包及静态库；Windows 使用系统 Schannel。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

Linux/macOS/Termux 产物通常是 `build/codex-cpp`；Windows 可使用 Visual Studio 或 MinGW，
Visual Studio 产物通常是 `build/Release/codex-cpp.exe`。
首次配置会下载固定版本 libgit2、libcurl 并校验 SHA-256，之后复用缓存；
离线构建可加 `-DFETCHCONTENT_SOURCE_DIR_CODEX_LIBGIT2=/path/to/libgit2` 指定已取得的依赖源码。

后端开关和系统 Git 回退实现已移除；旧缓存中的关闭选项也不会再禁用内置 Git。
不需要任何启用宏或后端选择参数。CMake 会自动编译、链接依赖。
若手动使用 g++，仍需提供 libgit2 的头文件与链接参数；单独编译 codex.cpp 已不再是完整构建方式。

网页、登录、编辑文件和模型请求不需要其他语言运行时或系统 `curl`；
上述 Git 操作不需要 Git 可执行文件，模型通过 Shell 主动运行其他 Git 命令时才需要安装它。
Agent 受限执行仍需要系统沙箱：Linux 使用 `bubblewrap`，macOS 使用 `sandbox-exec`。
Linux Release 将 bubblewrap 一起嵌入单文件，但仍需内核允许用户命名空间。
请以普通用户运行，程序拒绝以 root 启动网页服务。

### 平台验证范围

| 平台 | 当前状态 |
| --- | --- |
| Linux | 已验证本地网页、真实 Agent 子进程、审批、多人队列、分支冲突与重启恢复 |
| Windows | 提供 x86、x64、ARM64 单文件构建与运行测试；AppContainer 沙箱未实现，受限 Shell、沙箱 Git 和团队开发操作仍会拒绝执行；模型 HTTP 请求不再依赖沙箱 |
| macOS | 提供原生实现和 CI 构建配置，尚未实机验证；Agent 依赖系统提供可用的 `sandbox-exec` |
| Android / Termux | 不依赖 Python，尚未实机验证；Agent 运行取决于设备内核和应用权限是否允许所需的 Linux 沙箱 |

跨平台编译成功不等于完成所有平台的 Agent 沙箱。受限模式缺少沙箱时拒绝执行，不会自动放宽权限。
Windows/Android 设备也可以直接用浏览器连接已部署的 Linux 服务器，不需要在客户端安装这些工具。

### 单文件下载

[GitHub Releases](https://github.com/turtlelnc/Codex.cpp/releases) 的发布流程涵盖 Windows x86（32 位）、
x64、ARM64，macOS Intel / Apple Silicon，以及 Linux x64 / ARM64。
每个平台先验证把可执行文件独立复制到空目录、清空子进程 PATH 后的模型与网页功能，
并检查动态库依赖；全部成功才发布七个文件与 `SHA256SUMS.txt`。
Windows 静态链接 CRT，macOS 静态链接 OpenSSL 并使用系统证书信任，Linux 使用静态 musl/OpenSSL。
无需额外 DLL、网页目录、Python、Node、Git 或 curl 可执行文件。
运行 `--licenses` 可查看内嵌许可证；平台限制和可选工具依赖见 [发行说明](docs/RELEASE_NOTES.md)。

## TUI 团队设置

在终端运行 `./codex-cpp --ui tui`，输入 `/team` 打开菜单。
可配置 HTTPS 对外地址和本机端口，登记账号、重置令牌、撤销账号，并启动或停止团队服务。
令牌只在临时面板显示一次，不写入会话或输入历史。
账号操作必须先停服，撤销账号保留项目文件；退出 TUI 会停止由它启动的服务。
配置与 WebUI 的“团队”入口共用，数据默认在 `~/.codex-cpp/team-server`。
两处均只管理自己启动的进程，不能接管另一实例；切换管理入口前请先停服。
选择“Team projects (HTTPS)”可直接在 TUI 登录本机或远程团队服务器，无需打开浏览器。
支持创建项目、邀请和移除成员、共同及个人模型配置、模型选择、分支发布与同步、
审阅共同修改并提交、确认精确 SHA 后合并、查看任务和操作审计、检查文件后恢复中断队列。
共同凭据仍只允许创建者配置；成员可以使用，无法查看密钥。

项目菜单“Agent tasks on server”支持共同或个人会话的任务排队、查看会话、审批和停止任务。
其他成员也可以审批共同任务，服务器记录实际操作人。这里的任务始终操作服务器工作区；
退出团队菜单后的普通聊天仍使用当前本地工作区。
访问令牌和 API 密钥输入会被遮罩，登录 Cookie 只存在进程内存中，退出登录会撤销会话。
HTTPS 证书照常校验，不提供跳过验证的开关；HTTPS 代理仍需单独部署。

## 本地网页

```sh
./codex-cpp web --root /path/to/project
```

打开 `http://127.0.0.1:8765`。本地模式自动建立会话，不需要登录；服务只监听回环地址，
校验 Host、Origin、SameSite Cookie 和 CSRF。可以用 `--port` 改端口，或用 `--web-dir` 指定静态资源目录。
在 CMake 构建目录使用 `./build/codex-cpp`（Windows 使用对应 `.exe` 路径）。

网页左侧是会话与文件树，中间是对话和工具审批，右侧是编辑器、Diff 与终端输出。
编辑器支持 Ctrl/⌘+S；只能保存已有普通文件，单文件上限 1 MiB，不提供新建、删除或重命名。
符号链接、硬链接、特殊文件和内部状态目录会被拒绝；二进制和无效 UTF-8 文件只读。
保存时校验 SHA-256，文件被其他人或 Agent 修改后，需要重新载入再保存。

### 模型与提供商

模型按钮先打开推理强度面板，点击模型名称进入模型列表。列表从当前提供商获取，
也可在提供商配置里填写模型 ID；页面不硬编码某一组模型名称。能否选择推理强度取决于提供商能力。

- 内置 Codex 使用订阅设备登录，可在网页中登录；本地也可先运行 `./codex-cpp --provider codex --login`。
- OpenAI、DeepSeek 等兼容服务可在设置里添加，填写服务地址、`chat` / `responses`、模型 ID 和密钥。
- 密钥不返回页面，不放进 Agent 命令参数；自定义密钥通过子进程环境变量传递。
- 本地提供商配置保存在 `~/.codex-cpp/web-providers.json`，可用 `CODEX_CPP_HOME` 改目录。
  同一用户的本地工作区共用该配置。已有的这一格式可以继续读取。
- 也支持启动参数，例如设置好 `DEEPSEEK_API_KEY` 后运行：

```sh
./codex-cpp web --root /path/to/project --provider deepseek
```

`--model`、`--base-url`、`--api`、`--api-key-env`、`--no-api-key` 和 `--reasoning` 也可用于本地模式。
执行权限在网页中选择；多人服务禁止不受限的 `full` 模式。

## 多人服务器

### 从 WebUI 配置

运行 `./codex-cpp web --root /path/to/project` 后，点击顶部“团队”。
在“本机服务”中保存 HTTPS 对外地址与本机后端端口，在“账号”中登记成员并保存仅显示一次的访问令牌，
再启动团队服务。页面提供反向代理配置示例；仍需自行配置 HTTPS 代理。
成员打开该 HTTPS 地址，使用用户名和令牌登录即可。

此入口使用 `<CODEX_CPP_HOME>/team-server` 保存团队数据（默认 `~/.codex-cpp/team-server`），
与当前本地项目分开。它仅管理自己启动的服务，不接管单独运行的 `serve` 进程。
关闭浏览器标签页不会停止服务；退出本地 Codex.cpp 进程会停止其托管的团队服务。
新增、重置或撤销账号需要先停止团队服务；重置与撤销不会删除项目文件。
当前平台缺少工作区沙箱时，页面会提示限制，团队开发仍要求可用的沙箱。

登录团队服务器后，“团队”面板提供成员、共同模型、分支和活动四个页面。
项目创建者可邀请已登记账号、移除成员及配置共同模型凭据；所有成员拥有相同开发和审批权限。
个人模型凭据仍在个人工作区配置。

### 从命令行配置

先创建账户。以下命令会各输出一次随机访问令牌，请私下保存；登录时填写用户名与该令牌。
服务只保存令牌的 SHA-256 摘要，不使用用户自选密码。

```sh
./codex-cpp serve --state-dir /srv/codex-data --add-user alice
./codex-cpp serve --state-dir /srv/codex-data --add-user bob
./codex-cpp serve --state-dir /srv/codex-data --origin https://codex.example.com --port 8765
```

`/srv/codex-data` 须由运行服务的普通用户拥有且可写；也可以选择用户自己的目录。
新增、重置或撤销账户前必须停止服务，状态目录有独占写锁。对已有用户名再次 `--add-user` 会重置其令牌；
`--revoke-user bob` 撤销账户。账户管理命令不需要 `--origin`。

服务只监听 `127.0.0.1`，在 HTTPS 反向代理后部署。以 Caddy 为例：

```caddyfile
codex.example.com {
    reverse_proxy 127.0.0.1:8765
}
```

代理应保留浏览器的 Host、Origin，外部地址必须与 `--origin` 完全一致，包括非默认端口。
登录 Cookie 使用 Secure、HttpOnly、SameSite=Strict，8 小时过期；重启需重新登录。
不需要 Docker，之后可按部署需求增加容器。

### 协作方式

登录后可创建项目、选择共同或个人工作区，创建者可以邀请已登记的账户。

- 创建者管理成员及共同会话的模型凭据；成员只能使用共同凭据，不能通过页面读取或修改密钥。
- 所有成员的开发权限相同。共同会话人人可发任务、停止任务和审批，审批记录实际登录的操作人。
- 每人有独立个人工作区、分支、会话和模型凭据；共同会话也有独立工作区和分支。
  页面不向其他成员提供个人会话、个人任务内容或个人审批。服务器管理员仍能访问磁盘上的数据。
- 每个项目的 Agent 与分支任务按 FIFO 串行执行，避免共同修改互相覆盖；不同项目可并行。
- 先在个人工作区开发，再“发布个人分支”，最后在共同工作区明确合并。
  合并需确认来源和目标的精确提交 SHA，排队期间分支变化会拒绝本次操作。
- 共同工作区的修改可先审阅 Diff 和未跟踪文件，再提交；内容变化后需重新确认。
  项目执行任务期间，网页编辑器暂停保存，避免覆盖运行中的修改。

新项目从空仓库开始。目前不提供远程仓库导入、自动 GitHub 推送或 PR 接口。
首次导入可由管理员把文件放进对应的 `workspaces/<项目>/shared/`，再通过网页审阅并提交。
服务器启动参数不接受全局模型密钥；请分别配置共同和个人提供商。

### 分支冲突

共同分支合并发生冲突时，会返回冲突路径并中止合并，保持目标分支原状。
个人工作区“同步共同分支”也会在冲突时中止，不会自动选一方覆盖。

需要人工解决时，先在个人工作区同步一次，再由 Agent 或操作者在该个人目录里运行
`git merge --no-ff FETCH_HEAD`，编辑冲突文件并 `git add`、`git commit`，然后重新发布个人分支，
由成员再次审阅并合并。失败的同步会保留 `FETCH_HEAD` 供此流程使用。
正式仓库存放在状态目录的 `repositories/` 中；工作区 `.git` 是给 Agent 使用的副本，发布会导入个人副本的提交历史。
服务器 Git 操作在系统沙箱内执行，不在主机上加载 Agent 修改过的 Git 配置或钩子。

### 中断与恢复

任务进入运行状态前会持久化；服务重启后，结果不明的运行任务标记为 `interrupted`，阻塞该项目的队列。
等待中的任务保留，不会自动重放中断任务。创建者应检查实际文件和旧进程是否已经退出，
在网页“检查并恢复队列”中明确确认后继续；涉及的个人/共同 Agent 会话会重新开始，旧会话记录仍保留。
存在未完成的正式仓库合并时，必须先由管理员解决或中止后才能恢复。
Linux 设置父进程退出信号，Windows 使用 Job Object 管理原生子进程；macOS 异常退出后的进程清理尚未实测。

### 数据、权限与规模

这是给可信团队使用的应用级多用户服务。账户和凭据在应用内分开，Agent 工具受工作区沙箱限制，
但进程仍使用同一个操作系统服务账号；没有为每位用户建立独立 Unix UID、容器或虚拟机，
不适合把互不信任的租户当作已完成系统级隔离来部署。
状态目录包含工作区、凭据和会话，必须限制访问并备份；POSIX 使用私有权限，Windows 设置所有者/系统 ACL。

当前限制：100 个项目、16 个同时执行的项目、每项目 100 个等待任务、全局 128 个等待任务，
总任务记录上限 5000；到达上限拒绝新增，避免丢失请求去重记录。每项目保留最近 1000 条操作审计。
HTTP 使用固定线程池和有界队列；不提供 WebSocket 实时共同光标，页面通过轮询同步。
Git 元数据归档与会话会增长，需要管理员监控磁盘容量。本版不自动迁移旧 Python 多人服务数据库。

## 验证

Node.js 仅用作测试运行器，正式运行不需要它：

```sh
node tests/native_smoke.cjs ./build/codex-cpp
cmake --build build --target codex-self-test --parallel 2
./build/codex-self-test
node tests/native_web_test.cjs ./build/codex-cpp
node tests/team_service_test.cjs ./build/codex-cpp
```

第一项不依赖 Agent 沙箱；后两项完整测试需要受限进程执行可用，集成测试还需要 `curl` 和 `git`。
测试使用本地假模型服务，不消耗模型额度。浏览器回归脚本另需 Playwright、Chromium 和 OpenSSL。
GitHub Actions 配置包含 Linux/macOS/Windows 构建与基础冒烟测试，以及 Linux 集成测试。
