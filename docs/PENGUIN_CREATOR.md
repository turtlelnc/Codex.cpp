# Penguin Creator（第一版）

可以直接对 Agent 说：

> 帮我制作一个 Penguin 插件：在侧栏增加可暂停的番茄钟。先读取 penguin-creator Skill，生成、实现、检查和测试，完成后让我审阅，不要自动启用。

或要求制作 C/C++ 原生工具。程序内嵌 Skill、公共 C ABI 头文件和独立插件模板；运行预编译 Codex.cpp 不需要再下载项目源码或 SDK。Agent 仍需实现实际功能；脚手架本身只是起点。

## 工作流

1. `read_skill` 读取内置 `penguin-creator`。
2. `penguin_scaffold` 在工作区创建新目录，`kind` 为 `native` 或 `web`。ID 使用 `[a-z0-9][a-z0-9_-]{0,31}`，以兼容原生和网页模块；目录存在就拒绝，不覆盖现有文件。
3. Agent 用已有文件工具实现功能。原生模板包含 native.h、plugin.cpp、CMakeLists.txt、导出定义、清单及 WebUI 入口；web 模板无需编译。
4. `penguin_build` 在 OS 沙箱内调用本地 CMake/编译器。不下载依赖、不安装插件；构建使用工作区写权限和既有审批。已在当前进程加载的原生 id 不允许原地重建。
5. `penguin_check` 静态检查清单、资源路径、工具声明及当前编译产物，并返回文件 SHA-256 和手动启动参数。检查不会执行原生代码或 JavaScript，也不代表功能正确。
6. `penguin_test` 在单独的只读、禁止网络且环境已清理的 OS 沙箱内验证原生 ABI 初始化/服务注册，最多运行 10 秒，限制输出量。它遵循既有命令执行审批；不向当前服务加载插件。调用后不得声称插件工具或界面已经过功能测试。
7. 有浏览器或测试能力时继续验证实际功能；否则明确标记未验证。最后再次 check，网页会展示审阅结果和“复制手动启用命令”。探测报告与检查文件的指纹一致时，才显示“隔离加载测试通过”。
8. 用户审阅并信任代码后，停止旧实例，显式以 `--penguin /绝对路径/penguin.json` 重启。网页中的命令是本地 WebUI 示例；多人服务由管理员把参数加入原服务启动命令，保留原来的 state-dir、监听、认证等配置。

生成、编译、探测或复制命令都不会启用插件，没有自动安装/热加载 RPC。原生代码正式启用后具有宿主完整权限，网页插件具有当前页面/会话权限。

## CLI

进入工作目录再执行，路径必须位于当前目录所代表的工作区内：

```sh
cd /path/to/workspace
codex-cpp penguin scaffold plugins/my-tool my-tool native
codex-cpp penguin build plugins/my-tool
codex-cpp penguin check plugins/my-tool
codex-cpp penguin test plugins/my-tool
```

纯网页插件：

```sh
codex-cpp penguin scaffold plugins/my-widget my-widget web
# 实现 client.js 后静态检查；实际交互另用浏览器验证。
codex-cpp penguin check plugins/my-widget
```

已有命令 `penguin inspect` 和 TUI `/penguin` 保持不变。CLI 的 build/test 与 Agent 工具一样要求隔离，不会因沙箱缺失而自动放开权限。

## 平台与验证边界

- 运行程序和 web 插件不需要 Node.js、Python、CMake 或编译器。构建原生插件才需要 CMake 和 C++17 编译器。
- Linux 使用 bubblewrap，macOS 使用 Seatbelt。构建会读取标准系统编译器/SDK 目录，写权限仍局限于工作区；探测只读暂存的清单、原生库及已声明网页入口/样式，不挂载整个用户工作区；环境不继承模型账号和任意父进程秘密变量。探测前后校验文件指纹，文件改变就要求重测。
- Windows 的示例启动命令使用 PowerShell 语法。当前没有 AppContainer 后端：生成和静态检查可用，自动隔离构建/探测明确失败。用户可以手动使用模板 CMake 工程编译、审阅后自行决定加载；测试报告不把手动编译等同于隔离探测通过。
- Linux musl 全静态发布版不能 dlopen 原生插件；需要常规 glibc 构建来测试/启用原生模块，纯网页插件不受影响。
- 隔离探测只加载目标模块及内置模块，不自动加载其他第三方依赖。未声明的初始化资源、额外动态库、复杂依赖、网络行为、工具副作用和浏览器交互需要单独的测试环境。
- 指纹描述一次检查时的文件，不是签名或安全承诺；修改文件后需要重新构建、探测、检查。不要把“ABI 加载成功”当作“功能正确”。

## 实际回归

`tests/penguin_creator_test.cjs` 测试 SDK 内嵌、两种模板、不覆盖/越界拒绝、真实构建、单独启用，以及恶意初始化写入/环境读取被隔离、挂起初始化按时终止。Windows 分支验证拒绝不安全自动流程及手动编译模板。

`tests/penguin_creator_browser_test.cjs` 使用确定性模拟模型驱动真实 C++ 服务：读 Skill → 创建 → 编辑计数器 → 构建 → 隔离加载 → 检查。覆盖批准和拒绝、源码查看、命令复制、窄屏审阅；确认插件没有自动生效，再由测试显式重启验证生成的原生工具和点击计数器实际工作、卸载清理。

这些测试验证工具链和界面，不消耗真实模型额度；真实 DeepSeek 是否遵循 Skill、完成任意复杂需求，仍取决于模型与具体任务。
