# Codex.cpp

> A compact C++17 Codex-style coding agent runtime.
> 精简的 C++17 Codex 风格终端编程 Agent。

`codex-cpp` 是一个独立的、clean-room 实现的 Codex 风格终端编程 Agent。

它不是 OpenAI Codex CLI 的逐行移植，也不依赖官方 Codex CLI Runtime。核心 Agent loop、工具调用、会话存储、TUI、权限控制和 Provider 接入由少量 C++ 源文件实现；它们仍通过 `codex.cpp` 一次编译。

```text
codex.cpp + codex_api.inc + codex_tui.inc
    ↓ clang++ / g++ codex.cpp
codex-cpp
```

API 请求使用系统 `curl`；`apply_patch` 和 `/diff` 还需要 `git`。

主目录恢复自完成缩减的提交 `280e8cb`。远程网页与 Cloudflare Tunnel 版本已封存到 [archive/remote-web](archive/remote-web/ARCHIVE.md)，可独立查看和恢复。

---

## Features / 功能

- C++17 实现，单条编译命令
- OpenAI Responses API function calling loop
- Chat Completions tool calling
- ChatGPT / Codex 订阅设备登录
- OpenAI API Key
- DeepSeek API
- 自定义 OpenAI-compatible Provider
- 交互式终端 TUI
- Markdown 终端渲染
- Markdown Table 渲染
- 多轮会话
- 本地会话恢复
- 项目 Memory.md（显式记录、跨会话读取）
- 临时请求错误有限重试与未完成回合续跑
- 自动上下文压缩
- 文件读写
- Shell 命令
- `git apply` Patch
- Workspace 权限控制
- macOS / Linux OS-level shell sandbox
- Skills
- MCP stdio tools
- 图片输入
- JSONL Agent event 输出
- Plan / Goal / Review 等 Agent 辅助模式

---

# 中文

## 源码结构

- `codex.cpp`：入口、工具、会话与 Agent loop；编译此文件即可生成程序。
- `codex_api.inc`：API 客户端与流式响应。
- `codex_tui.inc`：终端界面与 Markdown 渲染。
- `codex_self_test.cpp`：独立的解析、工具、会话与权限检查。
- `sandbox_probe.py`：使用临时工作区验证 Shell 沙箱边界。

构建时需要把 `codex.cpp` 和两个 `.inc` 文件放在同一目录。运行程序无需这些源码文件；API 请求需要系统 `curl`。

---

## 编译

macOS / Linux：

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
```

或者：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
```

然后：

```bash
./codex-cpp --help
```

测试已从运行程序移到独立源码：

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex_self_test.cpp -o codex-self-test
./codex-self-test
python3 sandbox_probe.py
```

---

## 项目记忆与失败续跑

项目根目录的 `Memory.md` 会在新建、恢复或分叉会话时读取一次，作为独立参考上下文携带，不重复追加到聊天历史，最多 16 KiB。它适合保存构建方式、已确认的偏好和项目事实；项目规则仍放在 `AGENTS.md`。旧笔记可能过时，模型应先核对；只有用户明确要求时才更新记忆，不会自动总结每轮对话。

交互模式下查看或追加笔记：

```text
/memory
/memory 使用 clang++ 编译，保留 C++17 兼容性
```

`/memory 内容` 追加成功后刷新当前记忆；`/memory` 查看并重新加载，适合直接编辑文件后刷新。上下文压缩不会丢失当前记忆。只读模式禁止追加。

TUI 默认在底部显示淡色状态行；`/statusline bar on` 可开启顶部浮空栏：模型与 reasoning、权限（`danger-full-access` 标红）、上下文占用（相对自动压缩阈值）、订阅剩余额度、会话名。输入等待时每 5 秒刷新；额度来自非公开后端，自动刷新在后台执行，最多每 2 分钟查询一次，不阻塞输入；缓存标注更新时间，查询失败显示 stale 和错误，令牌过期时用 `/status` 或 `--quota` 刷新登录，其余项均为本地数据。`/status pin` 在顶部固定详细状态面板（再次执行取消），`/statusline bar off|on` 隐藏或显示浮空栏。终端高度 <30 行时只显示浮空栏，<15 行时隐藏。

`/model` 的模型列表：`--provider codex` 读取订阅账号的非公开模型接口，可得到每个模型支持的 effort（如 `max`、`ultra`）；其他 Provider 读取标准 `GET /models`，该接口不含 effort 信息，此时使用 `low/medium/high/xhigh`。读取失败时退回手动输入。

交互模式下 `/status` 显示本地状态和订阅额度；TUI 在输入框下方保留额度面板，提交下一条输入后收起。查询失败仍显示本地状态。

使用 `./codex-cpp --quota` 查询订阅用量和重置时间；添加 `--wait-quota` 后，HTTP 429 且查询确认主额度窗口耗尽时，等待已耗尽窗口中最晚的重置时间，再请求当前步骤。等待期间每 30 秒只读查询额度；使用重置卡或其他方式恢复额度后提前续跑。两项仅适用于 `--provider codex`。等待期间可按 Ctrl+C 退出，之后用 `--resume SESSION_ID --continue --wait-quota` 续跑。电脑休眠期间不会运行，唤醒后继续检查时间。查询失败、重置时间缺失、额外模型额度或流已开始时不会进入自动等待；查询路径属于非公开后端，可能变化。

临时限流、部分服务错误和网络故障最多重试两次。Codex 流一旦收到 SSE 内容，不会自动重新请求，以免重复显示已输出的内容。持续限流仍会停止并保留会话；不会自动重新执行工具。

恢复未完成的任务：

```bash
./codex-cpp --root /path/to/project --resume SESSION_ID --continue
```

也可以先用 `--resume SESSION_ID` 打开会话，再输入 `/continue`。续跑复用已有用户消息与工具结果；原回合的只读限制会保留。若工具调用已记录但结果缺失，会标记为执行结果未知，要求模型先检查工作区再决定是否重做；这类调用的实际效果需要核对。

---

## 快速开始

### 1. 使用 ChatGPT / Codex 订阅

默认 Provider 就是 `codex`。

首次登录：

```bash
./codex-cpp --provider codex --login
```

程序会显示设备验证码，并要求在：

```text
https://auth.openai.com/codex/device
```

完成授权。

之后直接：

```bash
./codex-cpp
```

或者指定项目目录：

```bash
./codex-cpp --root ~/Projects/my-project
```

也可以直接给一个任务：

```bash
./codex-cpp --root . "检查这个项目并修复编译错误"
```

默认 Codex 模型：

```text
gpt-5.6-sol
```

例如：

```bash
./codex-cpp \
  --provider codex \
  --model gpt-5.6-sol \
  --reasoning high \
  --root .
```

---

## OpenAI API

设置 API Key：

```bash
export OPENAI_API_KEY="..."
```

启动：

```bash
./codex-cpp --provider openai
```

或者：

```bash
./codex-cpp \
  --provider openai \
  --model gpt-5.6-sol \
  --reasoning high
```

`openai` Provider 使用 Responses API。

注意：

```text
--provider codex
```

使用 ChatGPT/Codex 登录。

而：

```text
--provider openai
```

使用 `OPENAI_API_KEY`。

两者的认证与计费路径不同。

---

## DeepSeek

设置：

```bash
export DEEPSEEK_API_KEY="..."
```

运行：

```bash
./codex-cpp --provider deepseek
```

当前代码默认模型：

```text
deepseek-flash
```

也可以覆盖：

```bash
./codex-cpp \
  --provider deepseek \
  --model your-model-name
```

DeepSeek Provider 使用 Chat Completions 风格的工具调用。

---

## 自定义 Provider

可以连接兼容 Responses API 或 Chat Completions 的服务：

```bash
./codex-cpp \
  --provider custom \
  --api chat \
  --base-url http://127.0.0.1:1234/v1 \
  --model local-model \
  --no-api-key
```

或者：

```bash
export MY_API_KEY="..."

./codex-cpp \
  --provider custom \
  --api responses \
  --base-url https://example.com \
  --model my-model \
  --api-key-env MY_API_KEY
```

也支持：

```text
CODEX_CPP_BASE_URL
CODEX_CPP_MODEL
CODEX_CPP_API_KEY_ENV
```

---

## TUI

交互模式：

```bash
./codex-cpp -i
```

UI 模式：

```bash
./codex-cpp --ui auto
./codex-cpp --ui tui
./codex-cpp --ui plain
```

`auto` 会在终端支持 VT 时启用 TUI，否则使用普通文本界面。

当前 TUI 支持基础 Markdown 显示，包括：

- 标题
- **Bold**
- *Italic*
- `inline code`
- 链接
- fenced code block
- 引用
- 无序 / 有序 / 嵌套列表（自动悬挂缩进）
- 分隔线
- Markdown Table

代码框右上角的 `⧉` 鼠标移入时高亮，可点击复制对应代码（需开启鼠标捕获，默认开启）；`/mouse off` 时可用 `/copy code`。

其他体验细节：英文按单词换行并保持样式；代码块与 diff 使用底色带（diff 附 `+N -M` 统计）；工具调用只显示命令/路径，长输出折叠为 `… +N more lines`；等待模型时显示动态状态；调整终端大小在输入等待时立即重绘；向上滚动时底部会提示可返回最新输出。

例如模型输出：

```markdown
| File | Status |
| --- | --- |
| main.cpp | Modified |
| test.cpp | Passed |
```

会直接渲染为终端表格，而不是显示原始 `|` 字符。

---

## 内置工具

模型目前可以调用：

```text
read_file
list_dir
read_skill
write_file
shell
apply_patch
```

### `read_file`

读取 Workspace 内的文本文件。

### `list_dir`

列出目录内容。

### `write_file`

创建或完整替换 Workspace 内的文本文件。

### `shell`

执行：

- 编译
- 测试
- Git
- 搜索
- 项目命令
- Shell 工具

### `apply_patch`

通过：

```bash
git apply
```

应用 unified diff。

### `read_skill`

读取：

```text
~/.codex/skills/<skill>/SKILL.md
```

或项目中的：

```text
.agents/skills/<skill>/SKILL.md
```

---

## 权限与 Sandbox

默认：

```text
sandbox: workspace-write
approval: on-request
```

共有三种权限：

```text
read-only
workspace-write
danger-full-access
```

命令行：

```bash
./codex-cpp --sandbox read-only
```

```bash
./codex-cpp --sandbox workspace-write
```

```bash
./codex-cpp --danger-full-access
```

也可以在会话中修改：

```text
/permissions
```

### macOS

受限 Shell 使用：

```text
/usr/bin/sandbox-exec
```

### Linux

使用：

```text
bubblewrap / bwrap
```

受限模式默认不给 Shell 网络权限。

如果系统无法提供真正的 OS Sandbox，程序不会假装已经隔离，而是拒绝执行受限 Shell。

只有显式使用：

```text
danger-full-access
```

才会绕过这个限制。

> `danger-full-access` 会给予模型运行命令的完整用户权限。只应在可信 Workspace 中使用。

---

## 会话

所有项目会话默认保存在：

```text
<workspace>/.codex_cpp/sessions/
```

一个 Session 通常包含：

```text
<session-id>.jsonl
<session-id>.transcript
<session-id>.items.jsonl
<session-id>.meta.json
```

其中：

- `.jsonl`：Agent / Tool 事件
- `.transcript`：可读对话文本
- `.items.jsonl`：结构化模型上下文
- `.meta.json`：Session 元信息

查看：

```bash
./codex-cpp --list-sessions --root .
```

恢复：

```bash
./codex-cpp --resume SESSION_ID --root .
```

在交互模式也可以：

```text
/sessions
/resume
```

TUI 下 `/resume` 可以直接选择已有 Session。

---

## 会话分支

创建新会话：

```text
/new
```

复制当前上下文到新 Session：

```text
/fork
```

归档：

```text
/archive
```

永久删除：

```text
/delete
```

归档后的 Session 位于：

```text
.codex_cpp/archived_sessions/
```

---

## Context Compaction

当上下文变大时，Runtime 可以调用模型压缩历史。

手动执行：

```text
/compact
```

程序也会在上下文达到内部阈值后自动尝试压缩。

结构化 `items.jsonl` 会被替换为压缩后的上下文，而不是单纯清空历史。

---

## 图片

启动时：

```bash
./codex-cpp \
  --image screenshot.png \
  "检查这个截图中的错误"
```

交互模式：

```text
/image screenshot.png
```

然后发送下一条消息。

支持：

```text
PNG
JPEG
WebP
GIF
```

单轮最多 4 张图片。

---

## 文件上下文

可以把文件显式附加到下一轮：

```text
/mention src/main.cpp
```

它会把对应文件内容加入下一次用户消息的上下文。

---

## Skills

支持用户级 Skills：

```text
~/.codex/skills/
```

以及项目级 Skills：

```text
.agents/skills/
```

结构：

```text
.agents/
└── skills/
    └── build-project/
        └── SKILL.md
```

查看：

```text
/skills
```

模型可以通过 `read_skill` 按需读取 Skill。

---

## MCP

支持本地 stdio MCP Server。

MCP 返回的 PNG、JPEG、WebP、GIF 图片会作为观察结果传给模型，不再只显示类型占位符；单次最多 4 张，单张 base64 数据最多 11 MB。图片数据不打印到终端。桌面控制仍需要单独配置的 MCP 后端；`experiments/desktop` 仅为受监督的桥接实验，不是独立桌面服务。

默认配置：

```text
~/.codex-cpp/mcp.json
```

也可以指定：

```bash
./codex-cpp --mcp-config ./mcp.json
```

示例：

```json
{
  "mcpServers": {
    "demo": {
      "command": "python3",
      "args": [
        "/absolute/path/server.py"
      ]
    }
  }
}
```

查看当前 MCP：

```text
/mcp
```

注意：MCP Server 本身以当前用户权限运行，不受 Agent Shell Sandbox 等价保护。

---

## 常用斜杠命令

| Command | 功能 |
| --- | --- |
| `/model` | 从 Provider 读取模型列表并选择模型与 reasoning effort（`/model refresh` 重新读取；`/model MODEL [EFFORT]` 直接设置） |
| `/login` | ChatGPT Device Login |
| `/auth` | 查看登录状态 |
| `/permissions` | 修改 Sandbox 权限 |
| `/status` | 当前模型、Session、Sandbox、上下文状态 |
| `/sessions` | 查看保存的 Session |
| `/resume` | 恢复 Session |
| `/clear(new)` | 新建会话并重置上下文；兼容 `/clear` 和 `/new` |
| `/fork` | Fork 当前会话 |
| `/rename` | 重命名 Session |
| `/archive` | 归档 Session |
| `/delete` | 删除 Session |
| `/compact` | 压缩上下文 |
| `/plan` | 开关 Plan Mode |
| `/goal` | 设置长期任务目标 |
| `/personality` | concise / balanced / detailed |
| `/review` | Review 当前 Git 变更 |
| `/diff` | 查看 Git Diff |
| `/mention` | 将文件加入下一轮上下文 |
| `/image` | 将图片加入下一轮 |
| `/skills` | 打开技能选择面板；方向键或滚轮选择，回车插入，Esc 关闭（纯文本模式列出技能） |
| `/mcp` | 查看 MCP |
| `/init` | 创建基础 `AGENTS.md` |
| `/copy` | 复制上一条模型 Markdown 回复 |
| `/statusline` | 配置 TUI Status Line |
| `/title` | 修改终端标题 |
| `/logout` | 删除/清除当前认证 |
| `/help` | 命令帮助 |
| `/quit` | 退出 |

---


## JSONL / 自动化

加上：

```bash
./codex-cpp --json "run tests"
```

会把 Agent 生命周期事件输出为 JSONL。

事件包括：

```text
session.started
session.resumed
turn.started
model.output
tool.requested
approval.requested
approval.decision
tool.started
tool.completed
turn.completed
error
```

因此可以把 `codex-cpp` 当作一个简单的 Agent Runtime 接到其他程序中，而不一定只通过 TUI 使用。

---

## 认证数据保存在哪里？

ChatGPT Device Login 不读取或修改官方：

```text
~/.codex/auth.json
```

而是使用自己的：

```text
~/.codex-cpp/auth.json
```

也可以通过：

```text
CODEX_CPP_HOME
```

改变位置。

项目 Session 则始终跟随 Workspace：

```text
<workspace>/.codex_cpp/
```

因此认证数据与项目对话数据是分开的。

---

## CLI 参数

```text
codex-cpp [options] [prompt...]
codex-cpp -i [options]
```

主要选项：

```text
--provider codex|openai|deepseek|custom

--login
--device-auth  # alias for --login

--api responses|chat
--base-url URL
--model MODEL
--reasoning low|medium|high|xhigh
--api-key-env NAME
--no-api-key

-i
--interactive

--ui auto|tui|plain

--root DIR
--resume SESSION_ID
--list-sessions

--json

--auto
--approval on-request|never

--sandbox read-only|workspace-write|danger-full-access
--read-only
--danger-full-access

--max-steps N

--mcp-config PATH
--image PATH

--help
```

默认最大 Agent steps：

```text
64
```

允许范围：

```text
1–512
```

---

## 项目定位

这个项目的目标不是重新实现完整官方 Codex CLI。

更准确地说，它是在一个尽可能小、可读、可修改的 C++ Runtime 中保留 Codex-style Agent 的关键结构：

```text
User
  ↓
Model
  ↓
function_call
  ↓
Tool Router
  ↓
Approval / Sandbox
  ↓
Tool Runtime
  ↓
function_call_output
  ↓
Model
  ↓
...
```

因此它更适合：

- 学习 Coding Agent Runtime
- 做 Agent 实验
- 自定义模型 Provider
- 在旧系统或受限环境中部署
- 构建轻量 Codex-style CLI
- 修改 Agent loop / Tool protocol
- 集成本地模型
- 测试新的工具和 Sandbox 机制

而不是追求和官方 Codex CLI 100% feature parity。

---

# English

## What is codex-cpp?

`codex-cpp` is a clean-room C++17 implementation of a Codex-style terminal coding agent. Its API client and TUI live in adjacent `.inc` files, while `codex.cpp` remains the build entry point.

It is not a line-by-line port of the official OpenAI Codex CLI.

Instead, it implements the core runtime architecture directly in C++:

```text
Model
  ↓
Function Call
  ↓
Tool Router
  ↓
Approval / Sandbox
  ↓
Tool Runtime
  ↓
Tool Result
  ↓
Model
```

API requests use system `curl`; `apply_patch` and `/diff` also require `git`.

---

## Build

macOS / Linux:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
```

Build and run the standalone checks:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex_self_test.cpp -o codex-self-test
./codex-self-test
python3 sandbox_probe.py
```

---

## ChatGPT / Codex login

```bash
./codex-cpp --provider codex --login
```

Complete the device authorization, then run:

```bash
./codex-cpp -i
```

or:

```bash
./codex-cpp --root . "fix the failing tests"
```

The current default Codex model is:

```text
gpt-5.6-sol
```

---

## OpenAI API

```bash
export OPENAI_API_KEY="..."

./codex-cpp --provider openai
```

Example:

```bash
./codex-cpp \
  --provider openai \
  --model gpt-5.6-sol \
  --reasoning high
```

---

## DeepSeek

```bash
export DEEPSEEK_API_KEY="..."

./codex-cpp --provider deepseek
```

The current built-in default is:

```text
deepseek-flash
```

---

## Custom endpoints

```bash
./codex-cpp \
  --provider custom \
  --api chat \
  --base-url http://127.0.0.1:1234/v1 \
  --model local-model \
  --no-api-key
```

Both Responses-style and Chat-Completions-style backends are supported.

---

## Built-in tools

```text
read_file
list_dir
read_skill
write_file
shell
apply_patch
```

Additional tools can be exposed through local MCP stdio servers.

---

## Sessions

Sessions are stored per workspace:

```text
<workspace>/.codex_cpp/sessions/
```

Each session contains structured model items, a readable transcript, metadata and event logs.

List sessions:

```bash
./codex-cpp --list-sessions --root .
```

Resume:

```bash
./codex-cpp --resume SESSION_ID --root .
```

or interactively:

```text
/resume
```

---

## Sandbox

Available permission modes:

```text
read-only
workspace-write
danger-full-access
```

macOS restricted commands use `sandbox-exec`.

Linux restricted commands use `bwrap`.

Restricted shells do not receive network access.

If a real OS-level sandbox cannot be established, restricted shell execution is denied rather than silently running unrestricted.

---

## TUI

```bash
./codex-cpp -i
```

The terminal renderer supports basic Markdown including:

- headings
- bold / italic text
- inline code
- fenced code blocks
- links
- quotes
- bullet lists
- Markdown tables

UI selection:

```bash
./codex-cpp --ui auto
./codex-cpp --ui tui
./codex-cpp --ui plain
```

---

## Skills

User Skills:

```text
~/.codex/skills/
```

Project Skills:

```text
.agents/skills/
```

Each Skill is represented by:

```text
SKILL.md
```

List available Skills with:

```text
/skills
```

---

## MCP

Default MCP config:

```text
~/.codex-cpp/mcp.json
```

Example:

```json
{
  "mcpServers": {
    "demo": {
      "command": "python3",
      "args": [
        "/absolute/path/server.py"
      ]
    }
  }
}
```

Or:

```bash
./codex-cpp --mcp-config ./mcp.json
```

---

## Authentication storage

ChatGPT credentials are stored separately from the official Codex CLI:

```text
~/.codex-cpp/auth.json
```

The program deliberately does not use:

```text
~/.codex/auth.json
```

Workspace conversations are stored separately under:

```text
<workspace>/.codex_cpp/
```

---

## Status

This project implements the core Codex-style agent workflow, but it is not intended to be a complete replacement for every official Codex CLI feature.

Only implemented slash commands are listed in the command menu and help.

The focus is a small, hackable and understandable coding-agent runtime rather than full feature parity.

---

## Why?

A complete modern coding-agent runtime normally includes a large application stack.

`codex-cpp` asks a different question:

> How small can a useful C++ coding-agent runtime stay while keeping its major components easy to inspect?

The result is intended to be easy to inspect, modify, compile and experiment with.

在交互界面输入 `$` 也可打开技能面板，继续输入名称筛选；选中后会插入 `$技能名 `，补充任务再发送。

交互界面默认使用简洁开屏、随机中英文短句和灰底输入框；模型、权限、上下文及额度集中在底部状态行。`/statusline bar on` 可开启顶部状态条。

### 界面设计

`/design` 或 `/design on` 开启设计模式，`/design off` 关闭。`/design 需求` 对单次任务应用设计规则；`/design plan 需求` 先给方案，并使用只读权限。设计模式复用现有工具和项目技术栈，不自带图片生成或浏览器预览。

规则参考 [claude-design 提示词](https://github.com/asgeirtj/system_prompts_leaks/tree/main/Anthropic/claude-design)，重新提炼为现有界面分析、视觉一致性、可用交互、无障碍和验证要求，不依赖其专用运行时。

设计模式优先验证完整任务流程和页面切换后的可见内容；删除重复、显而易见或解释内部组织方式的常驻文案，保留隐私、题源、给分等影响决策的信息。不以禁用常见字体或卡片来追求“反 AI 风格”。

较完整的 Design 任务默认要求模型在交付前调用 `design_review`；方案和简单文字、颜色修改跳过。审核使用当前模型及登录/提供商配置，仅接收任务和模型选定的项目内文本源码，不继承聊天记录，没有执行或写入工具。主代理判断意见并修复；若修复涉及交互、状态或可访问性，再复查一次。每轮对话最多两次审核尝试，失败也计数；审核失败必须说明缺口。审核不能替代实际交互、渲染或读屏验证，也不能保证没有遗漏。

`design_review` 仅在可写 Design 模式可用，最多 12 个文件、160 KB 源码；二进制 PPTX 等应通过实际渲染检查。审核会额外消耗当前提供商的模型额度；主代理按规则决定是否跳过小修改及是否需要第二轮；程序在交付前检查是否已审核或明确记录跳过理由，并约束调用上限和只读隔离。审核开始后不能改为跳过；无效的审核参数或跳过请求必须处理，不能直接交付。

Design 模式切换紫色输入框和状态标识，保留页面聊天及模型上下文。`/clear` 与 `/new` 相同：开启新会话并重置上下文，原会话仍保存在本地。

命令菜单将新建会话合并显示为 `/clear(new)`，输入 `/clear`、`/new` 或 `/clear(new)` 均执行同一个操作。

设计规则另参考 [duyet/frontend-design](https://github.com/duyet/codex-claude-plugins/blob/master/frontend-design/skills/frontend-design/SKILL.md)，提炼视觉方向、字体层级、克制配色、有目的的动效、组件组合和局部状态；不引入其字体禁用清单或固定框架偏好。

### 执行期间补充消息

TUI 在等待模型或执行长工具时仍可输入。回车发送引导消息，当前请求或工具结束后、下一次模型请求前加入上下文；若本轮模型已给出最终回复，会继续处理引导。Tab 将消息放入队列，当前轮结束后按发送顺序自动执行；Ctrl+J 换行，Esc 中断当前操作。尚未发送的输入会留在输入框。审批面板打开时按键用于审批。

队列仅保存在当前进程中；已应用的引导写入会话。`/clear(new)`、恢复或分叉会话会清空尚未应用的队列和草稿。纯文本及 JSON 模式不提供执行中的输入面板。

TUI 粘贴超过 10 行文本时显示 `[Pasted text #N +行数 lines]`，左右移动和删除将标记视为一个整体；发送、引导和排队均展开完整原文。鼠标悬停高亮，点击打开全文编辑面板，回车换行、Ctrl+S 保存、Esc 取消。保存后行数标记随之更新。

手动输入同样的标记会保留为普通文字。任务结束时尚未完成的粘贴会继续接收；运行中也支持 Delete 删除折叠块。

输入回归测试（macOS/Linux，无需模型或登录）：`python3 tests/tui_input.py`。

### 可选后台 Computer Use（实验）

macOS Accessibility 和 Windows UI Automation 后端通过 `computer_batch` 观察控件、调用按钮或替换可编辑值，不发送全局鼠标/键盘事件，不主动切换焦点。系统不支持的控件直接报错，没有截图或全局输入回退。应用仍可能自行激活窗口；Windows 尚未实机验证。构建、MCP 配置和能力边界见 [tools/COMPUTER_USE.md](tools/COMPUTER_USE.md)。
