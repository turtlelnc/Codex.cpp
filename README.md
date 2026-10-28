# Codex.cpp：原生网页与多人服务

同一个 C++17 程序提供命令行 Agent、本地网页 `web` 和多人网页服务 `serve`。
本地及多人服务均由 C++ 实现，不需要 Python、Node.js 或前端构建工具。
浏览器就是客户端：本地模式操作本机工作区，服务器模式操作服务器上的工作区。
保留文件树、编辑器、工具审批、会话记录、两级模型菜单与自定义模型提供商。

可选的 [libgit2 嵌入实验](docs/LIBGIT2_TRIAL.md) 已支持本地网页状态与 Diff，无需运行系统 Git；
多人提交/合并等操作仍使用原来的 Git 后端。

## 编译

保留所有 `.inc` 源文件，以及二进制旁的 `web/` 静态资源。
Linux / macOS / Termux 可用支持 C++17 的编译器：

```sh
clang++ -std=c++17 -O2 -pthread codex.cpp -o codex-cpp
# 或用 g++ 替代 clang++
```

也可用 CMake；它会把 `web/` 复制到可执行文件旁：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

Windows 可用 Visual Studio 的 C++ 工具链运行以上 CMake 命令，产物通常是
`build/Release/codex-cpp.exe`。MinGW-w64 的直接编译方式：

```sh
g++ -std=c++17 -O2 -pthread codex.cpp -o codex-cpp.exe -static -lws2_32 -lbcrypt -ladvapi32 -lshell32
```

只启动网页、登录多人服务、编辑已有文件，不需要其他运行时。
模型网络请求需要系统 `curl`，Diff 和协作分支需要 `git`；Agent 的受限进程执行还需要可用的系统沙箱。
Linux 使用 `bubblewrap`（`bwrap`），macOS 使用 `sandbox-exec`。请以普通用户运行，程序拒绝以 root 启动网页服务。

### 平台验证范围

| 平台 | 当前状态 |
| --- | --- |
| Linux | 已验证本地网页、真实 Agent 子进程、审批、多人队列、分支冲突与重启恢复 |
| Windows | 原生服务已通过 MinGW-w64 交叉编译；尚未实机验证。原 Agent 的 AppContainer 沙箱未实现，依赖它的模型请求和协作 Git 操作会拒绝执行 |
| macOS | 提供原生实现和 CI 构建配置，尚未实机验证；Agent 依赖系统提供可用的 `sandbox-exec` |
| Android / Termux | 不依赖 Python，尚未实机验证；Agent 运行取决于设备内核和应用权限是否允许所需的 Linux 沙箱 |

跨平台编译成功不等于完成所有平台的 Agent 沙箱。受限模式缺少沙箱时拒绝执行，不会自动放宽权限。
Windows/Android 设备也可以直接用浏览器连接已部署的 Linux 服务器，不需要在客户端安装这些工具。

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
node tests/native_smoke.cjs ./codex-cpp
clang++ -std=c++17 -O1 -pthread tests/agent_self_test.cpp -o codex-self-test
./codex-self-test
node tests/native_web_test.cjs ./codex-cpp
```

第一项不依赖 Agent 沙箱；后两项完整测试需要受限进程执行可用，集成测试还需要 `curl` 和 `git`。
测试使用本地假模型服务，不消耗模型额度。浏览器回归脚本另需 Playwright、Chromium 和 OpenSSL。
GitHub Actions 配置包含 Linux/macOS/Windows 构建与基础冒烟测试，以及 Linux 集成测试。
