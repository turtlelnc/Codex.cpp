# Codex.cpp 网页精简体验包

当前首页布局、手机抽屉侧栏、项目文件树与内置文件编辑器、两级模型/推理强度菜单、
可自定义的模型提供商设置与本地 Agent 源码。
不含多人协调器、部署脚本、历史归档、测试、截图或二进制。

## 编译

Linux / macOS，需要支持 C++17 的编译器：

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
# 或用 g++ 替代 clang++
```

## 查看网页

需要 Python 3.10+。保留 server/ 和 web/ 与 codex-cpp 在同一目录。

```sh
./codex-cpp web --root .
```

浏览器打开 http://127.0.0.1:8765 即可使用，不需要登录或令牌：服务只监听本机，
并校验 Host / Origin、SameSite=Strict 会话 cookie 与 CSRF 令牌，其他网页无法借你的浏览器操作 Agent。
只看首页不需要配置或调用模型。

界面为米白主题，三栏布局：左侧会话与项目文件，中间对话（工具调用卡片、内联审批与 Diff、流式回答），
右侧代码 / 变更（工作区相对 HEAD 的 git diff，待审批补丁在此逐块审阅）/ 终端。运行中点击 ■ 可停止任务。

### 项目文件与编辑器

左侧“项目文件”不只是摆设：它列出 `--root` 工作区里的文件，点文件夹展开/收起，
点文件就在右侧“代码”面板打开编辑器。

- 文件夹默认全部折叠，只显示第一层；展开状态在页面内保留，不写盘。
- 编辑器有行号、语法高亮和滚动同步，支持 Ctrl/⌘+S 保存、Tab 插入 2 个空格、“重新载入”。
- 只能编辑工作区里已经存在的普通文件，暂不支持新建、删除、重命名；单文件上限 1 MiB。
- 二进制文件（含 NUL 字节）以只读方式打开，保存按钮不可用。
- 保存前用 SHA-256 校验磁盘上读到的版本；如果文件已被 Agent 或其他程序修改，保存会被拒绝并提示冲突，
  可点“重新载入”查看最新内容。未编辑时，任务结束后编辑器会自动同步磁盘上的新内容。
- 工作区里的 `.git`、`.codex_cpp` 等内部目录、符号链接和非普通文件不会出现在文件树中；
  编辑器也会拒绝这些内部路径。

### 模型选择

点击模型按钮先显示强度面板，再点击模型名称进入模型列表（卡片字号更紧凑，已去掉无意义的装饰符号）。
获取真实模型列表、发任务需要配置 Provider 或登录；接口没有返回强度信息时提供手动选择。

## 调用模型（可选）

需要系统 curl；文件 Diff 需要 git；Linux 受限命令执行需要 bubblewrap（bwrap）。
Codex 订阅可先运行 `./codex-cpp --provider codex --login`，也可在网页设置配置 Provider。

### 提供商设置

- 网页“提供商”面板里只内置 Codex（使用 ChatGPT / Codex 订阅登录，可点“设备登录”）。
  OpenAI、DeepSeek 以及其他兼容 OpenAI 协议的服务都请在这里手动添加。
- 添加时填写：名称、服务地址（如 `https://api.example.com/v1`，不能带账号、查询串或片段）、
  API 类型（`chat` 或 `responses`）、模型列表（逗号分隔，第一个为默认，可留空）、API 密钥（本地服务可留空）。
- 配置保存在本机私有目录的 `web-providers.json`（默认 `~/.codex-cpp/web-providers.json`，
  可用 `CODEX_CPP_HOME` 改目录），文件权限 0600，所有本地工作区共用；密钥不会写进项目目录，也不会返回给页面。
- 使用自定义提供商时，密钥通过环境变量 `MY_API_KEY` 传给子进程；没填密钥的本地服务用 `--no-api-key` 启动。
- 旧版本保存在 `~/.codex-cpp/web-<hash>.config.json` 里的提供商配置不再读取，也不会自动迁移，
  需要在页面里重新添加一次。
- 命令行参数 `--provider`、`--base-url`、`--api`、`--api-key-env`、`--no-api-key` 仍可在启动时指定，
  在页面里第一次选择提供商之前优先；命令行也仍然支持 deepseek / openai 预设。
- 不要将 API 密钥写进源码或命令参数。

例如已设置 DEEPSEEK_API_KEY 后（命令行预设方式）：

```sh
./codex-cpp web --root /path/to/project --provider deepseek
```

## 说明

精简包用于本地 web 模式，不含 serve/SSH 协作的完整组件；文件编辑功能只在 local 模式可用。
Linux 已验证。macOS 入口已提供但未实机验证；Windows 暂不支持托管网页。
