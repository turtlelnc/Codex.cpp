# 给 Claude 的交接说明

这是 Codex.cpp 网页版本的完整交接快照，网页开发改动已随此快照保存到独立分支。请以包内文件为准，不要通过旧 main 分支覆盖它。

## 修改目标

用户会进一步说明需要怎样调整网页。优先修改界面、布局、样式与交互，保留现有本地/多人服务器功能。

- 活跃前端：`web/index.html`、`web/style.css`、`web/app.js`。
- HTTP 服务：`server/web_server.py`；本地 Agent 适配：`server/local_agent.py`。
- 多人协作与权限：`server/collab_daemon.py`、`server/collab_worker.py`。
- C++ 启动入口：`codex_web.inc`；Agent 核心：`codex.cpp`。
- 使用、部署及接口：`server/WEB.md`；协作规则：`server/COLLABORATION.md`。
- `archive/remote-web/` 是历史归档，不是当前网页，请不要改错目录。

## 已确定的产品行为

同一个程序支持原有 TUI、本地网页和多人服务器网页。浏览器就是客户端。
本地模式操作本机文件；服务器模式操作服务器工作区。先直接部署，不要求 Docker。
网页支持完整任务流程：消息、队列、审批、文件查看、Diff、个人/共同会话、成员管理、发布和合并。
个人分支与共同分支分开，完成后明确合并。共同会话任务排队，所有成员开发权限相同，任何成员可审批并记录操作人。
创建者管理成员及项目凭据。个人会话用个人凭据，消息/任务/审批仅本人可见；共同会话用项目凭据，密钥不提供读取接口。
分支冲突中止合并并恢复目标，不能覆盖他人改动；解决后重新发布并确认具体 SHA。

## 修改时应保留的接口和交互边界

- 同源 `/api/login`、`/api/me`、`/api/logout`、`/api/rpc`；保留 Cookie 与 CSRF 机制。
- 队列请求使用唯一 request_id；网络失败显式重试时沿用原 ID，不能自动重放未知结果。
- 审批绑定当前审批 ID，显示操作参数；个人任务不能由其他成员审批。
- 合并确认绑定来源/目标 SHA，提交确认绑定文件指纹。
- 模型输出、文件内容、Diff 等非可信数据按文本渲染；若引入 Markdown/HTML，必须防止脚本和危险链接执行。
- API 密钥输入不回显、不进入 URL/浏览器持久凭据存储，不添加读取密钥 API。
- 保留手机布局、键盘操作、表单标签和明确的错误提示。

## 运行

需要 C++17 编译器、Python 3、curl；Git 操作需要 git；Linux 受限执行需要 bubblewrap。

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
./codex-cpp web --root /path/to/project
```

本地默认 http://127.0.0.1:8765，登录令牌文件位置在启动终端显示。
配置 Provider 后才能调用真实模型。服务器需按 server/WEB.md 配置独立 Unix 账户、可信网关和 HTTPS 代理。
没有前端依赖安装或构建步骤；运行时要保留 server/ 与 web/ 配套目录。

## 已做验证

84 项 C++ 检查、41 项 Python 测试通过；真实 Chromium + C++ Agent + 本地假 Provider 验证本地流程。
另有实际多 Unix UID、SO_PEERCRED、Git、HTTP 及 HTTPS 双用户浏览器测试，覆盖权限、审批、提交和合并确认。
容器内 Agent 使用明确的协议夹具，不能当作容器沙箱验证；真实 Agent 的 Bubblewrap 测试在原生环境另行执行。
没有使用真实收费模型凭据，没有部署到用户服务器。现有执行器的已知限制仍以 README 和协作文档为准。

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -q
node tests/web_browser.cjs ./codex-cpp
```

浏览器测试额外需要 Playwright 和 Chromium。测试不会自动安装这些依赖。
`handoff/previews/` 是测试时的界面截图，`handoff/VALIDATION.md` 是验证记录。
包内不含 Git 数据、登录凭据、真实会话、构建产物或编译器工具链。
