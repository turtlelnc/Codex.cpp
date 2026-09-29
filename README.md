# Codex-cpp

> A single-file C++17 Codex-style coding agent runtime.  
> 单文件 C++17 Codex 风格编程 Agent Runtime。

`codex-cpp` 是一个独立的、clean-room 实现的 Codex 风格终端编程 Agent。

它不是 OpenAI Codex CLI 的逐行移植，也不依赖官方 Codex CLI Runtime。核心 Agent loop、工具调用、会话存储、TUI、权限控制和 Provider 接入均直接实现在一个 C++ 源文件中。

```text
codex.cpp
    ↓
clang++ / g++
    ↓
codex-cpp
```

运行时唯一必需的外部程序是系统 `curl`。

---

## Features / 功能

- 单文件 C++17 实现
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

可以先运行内置测试：

```bash
./codex-cpp --self-test --root .
```

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
- 无序列表
- Markdown Table

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

创建临时侧会话：

```text
/side
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
| `/model` | 修改模型 / reasoning effort |
| `/login` | ChatGPT Device Login |
| `/auth` | 查看登录状态 |
| `/permissions` | 修改 Sandbox 权限 |
| `/status` | 当前模型、Session、Sandbox、上下文状态 |
| `/sessions` | 查看保存的 Session |
| `/resume` | 恢复 Session |
| `/new` | 新建会话 |
| `/fork` | Fork 当前会话 |
| `/side` | 创建侧会话 |
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
| `/skills` | 查看 Skills |
| `/mcp` | 查看 MCP |
| `/init` | 创建基础 `AGENTS.md` |
| `/copy` | 复制上一条模型 Markdown 回复 |
| `/statusline` | 配置 TUI Status Line |
| `/title` | 修改终端标题 |
| `/logout` | 删除/清除当前认证 |
| `/clear` | 清屏并开始新会话 |
| `/help` | 命令帮助 |
| `/exit` | 退出 |

---

## 尚未实现的 Codex 能力

为了让命令提示尽可能接近 Codex，一些官方 Codex 风格命令目前会显示在 `/help` 中，但明确标记为 unsupported。

包括例如：

```text
/ide
/keymap
/vim
/experimental
/approve
/import
/hooks
/app
/agent
/raw
/usage
/theme
/pets
/plugins
/feedback
/subagents
/memories
```

它们目前只是保留命令入口，不代表已经实现对应能力。

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
--device-auth

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

--self-test
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

`codex-cpp` is a clean-room, single-file C++17 implementation of a Codex-style terminal coding agent.

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

The only required runtime executable is system `curl`.

---

## Build

macOS / Linux:

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
```

Run the built-in smoke tests:

```bash
./codex-cpp --self-test --root .
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

Several command names are retained for familiarity but currently report that the corresponding capability is unsupported.

The focus is a small, hackable and understandable coding-agent runtime rather than full feature parity.

---

## Why?

A complete modern coding-agent runtime normally includes a large application stack.

`codex-cpp` asks a different question:

> How much of a useful Codex-style coding agent can fit into one C++ source file?

The result is intended to be easy to inspect, modify, compile and experiment with.
