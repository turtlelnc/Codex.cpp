# 网页、本地与服务器模式

同一个 `codex-cpp` 程序保留 TUI，并增加两个不依赖 TUI 的入口：

```sh
./codex-cpp                         # 原有本地 TUI
./codex-cpp web --root /path/project # 本机 Agent + 本机网页
codex-cpp serve --origin https://codex.example.com # 服务器 Agent + 多用户网页
```

浏览器就是客户端，不需要安装桌面应用。网页与 API 共用同一服务；服务器不启动或模拟 TUI。
当前服务托管支持 Linux；本地网页使用 Python 3 标准库，沿用 C++ Agent 的系统依赖。
macOS 本地入口代码已提供，但尚未进行实机验证；Windows 可使用浏览器连接 Linux 服务端，暂不支持在 Windows 托管网页服务。
不需要 Docker、Node.js、npm 或前端构建步骤。

## 本地使用

在仓库中编译，保留 `server/`、`web/` 两个配套目录：

```sh
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
./codex-cpp web --root /path/to/project
```

打开 `http://127.0.0.1:8765`。终端显示本地登录令牌文件的位置，默认是
`~/.codex-cpp/web-token`；在自己电脑上读取该文件，将令牌输入网页的登录框。
令牌不进入 URL、浏览器 localStorage 或请求日志。

可通过启动参数指定 Provider，或在网页“设置”中配置：

```sh
./codex-cpp web --root . --provider deepseek --model deepseek-chat
./codex-cpp web --root . --provider custom --api chat \
  --base-url http://127.0.0.1:1234/v1 --model local-model --no-api-key
```

API 密钥通过对应环境变量或网页密码输入框提供，不通过命令参数提供。
网页的 Provider 配置保存在用户私有的 Codex home 中，不放入 Agent 工作区；下次启动时恢复。
明确传入的 Provider 参数优先于保存的参数。配置 `codex` 后，网页显示设备登录通知，按通知完成登录。
本地会话可以新建或切换；单个工作区同一时刻执行一个任务，重复提交同一请求 ID 不重复执行。
本地登录令牌目前在该账户的本地网页服务之间共用；要撤销令牌，停止相关服务、删除令牌文件再启动。

本机文件与 Agent 都留在本机。关闭浏览器不会停止任务；停止网页服务会终止当前 Agent。
服务重启不会自动重放中断任务。默认只监听 loopback，不能绑定公网地址。

## 服务器安装（直接运行，无 Docker）

沿用 [多用户服务器安装](README.md)，每名成员使用独立 Unix 账户；网页用户名对应这些已开通的账户。
管理员负责开通服务器账户，项目创建者负责把已有账户邀请进项目。首版没有公开注册、找回密码或邮件邀请。
安装脚本会安装网页文件、建立专用 `codex-web` 服务账户、配置可信的 Unix socket 网关并安装 systemd 单元；
网页服务在配置域名及账号前不会自动启动。

```sh
sudo sh server/install.sh /absolute/path/to/codex-cpp
sudo codex-server-add-user alice /path/to/alice.pub
sudo codex-server-add-user bob /path/to/bob.pub
sudo -u codex-web /usr/local/bin/codex-cpp serve --add-user alice
sudo -u codex-web /usr/local/bin/codex-cpp serve --add-user bob
```

`--add-user` 交互输入至少 12 个字符的密码，保存 PBKDF2-SHA256（600000 次、随机盐）派生值；
不会打印明文。再次执行可重置密码；旧网页会话在下一次请求时失效。
账户文件是 `/var/lib/codex-web/accounts.json`，仅该服务账户可读写；删除其中一个账户也会撤销该用户的网页会话。
不要给 `codex-web` 加入 `codex-remote` 组，也不要用成员账户启动可信网关。

配置 `/etc/codex-server/web.env`：

```text
CODEX_WEB_ORIGIN=https://codex.example.com
```

在浏览器入口部署 HTTPS 反向代理，保持原始 Host，转发到 `127.0.0.1:8765`。例如 Caddy：

```caddyfile
codex.example.com {
    reverse_proxy 127.0.0.1:8765
}
```

先确认当前没有运行中的任务，再重启协调器，使新代码和网关身份配置生效：

```sh
sudo systemctl daemon-reload
sudo systemctl restart codex-collab
sudo systemctl enable --now codex-web
```

若手动安装，协调器需带 `--web-user codex-web` 参数；HTTP 服务必须由这个专用非 root 账户运行。
服务端通过 `--origin` 验证 Host/Origin。HTTPS 在反向代理终止，HTTP 后端始终只监听 loopback。

## 网页工作流

- 创建项目，可选 HTTPS 仓库；邀请已有服务器账户。
- 创建者在“设置”中选择“项目”凭据；成员可设置自己的“个人”凭据。密钥只写入，不提供读取 API。
- “共同会话”：所有成员可发送任务，按顺序执行；任何成员可审批，操作人会记录到事件中。
- “个人会话”：准备自己的独立工作区及分支，使用个人凭据。个人消息、模型事件、任务记录和审批仅本人可见；创建者也不能审批别人的个人任务。
- 浏览文件、查看 Diff 和工具审批参数；修改由 Agent 工具执行，网页没有另设任意命令或文件写入 API。
- 在“变更”中提交共同修改，或发布个人分支，再显式合并。确认绑定具体提交 SHA/文件指纹；等待队列期间发生变化会拒绝旧确认。
- 合并冲突自动中止并恢复目标。网页展示冲突文件；可让个人 Agent 合并 `FETCH_HEAD`、解决冲突并提交，再发布、重新确认合并。不会自动覆盖其他人的改动。
- 创建者检查中断项目当前 Diff 后可恢复队列；已中断的任务不会重放。

网页个人任务目前也进入该项目的统一队列，避免与发布、同步等 Git 操作同时修改工作区；不同项目可以并行。
不要同时用 SSH 个人 TUI 与网页操作同一个个人工作区；检测到活动 tmux 会话时，网页会拒绝个人执行、发布和同步。
个人 Provider 在网页配置后用于网页会话；原有 SSH 个人 TUI 仍使用其自己的配置文件。

## 接口与前端交接

前端在 `web/index.html`、`web/style.css`、`web/app.js`，全部同源、本地静态资源，无 CDN。
可以交给 Claude 调整界面；保留 API 调用、CSRF、请求 ID、SHA/文件指纹确认以及通过 `textContent` 展示非可信内容的边界。

| HTTP 接口 | 用途 |
| --- | --- |
| `POST /api/login` | `{username,password}`；本地仅需令牌作为 password |
| `GET /api/me` | 当前用户名、模式、CSRF token 和过期时间 |
| `POST /api/logout` | 撤销当前 Cookie 会话 |
| `POST /api/rpc` | `{op,project,...}`；返回 `{result:...}` |

登录后的 POST 要求 `X-CSRF-Token`。Cookie 为 HttpOnly、SameSite=Strict；服务器 HTTPS Origin 下还带 Secure。
会话有效期 8 小时，仅保存在内存，服务重启后需重新登录。更新密码/删除账号会撤销旧会话。
登录有速率及并发限制；反向代理可再按真实客户端 IP 限流。
前端每秒用事件游标轮询，断线后从上次游标继续，任务不会因关闭页面而取消。
首版采用轮询，不是 WebSocket/SSE。未确认提交的请求 ID 与原消息保存在当前标签页的 sessionStorage；网络恢复后由用户显式重试同一条消息，不自动重放。

服务器操作复用 [协作协议](COLLABORATION.md)，新增 `files`、`personal_files`、`personal_read`、`personal_diff`、
`personal_send`、`personal_login`、`personal_configure`。写入队列的操作需 `request_id`，重试必须使用相同 ID 和参数。
本地使用 `snapshot`、`send`、`approve`、`files`、`read`、`diff`、`configure`、`login`、`model`、`session`。
JSON Agent 新增 `model.delta` 的 `payload.text`，`model.output` 附带完整 `payload.text`，不影响原有事件名。

协调器用内核 `SO_PEERCRED` 验证网关 Unix UID；只有显式配置的网关 UID 可以转发网页登录身份。
普通 SSH 成员不能提交 `web.dispatch` 冒充其他成员。所有项目权限仍在协调器检查，Git/Agent 仍降权运行。
网关持有团队的认证权限，服务账户及账号数据库应视作可信服务端组件。
文件 API 不跟随符号链接，拒绝管道和硬链接，并屏蔽 `.git`、会话和认证目录。
这些保护不替代 [现有服务器隔离限制](COLLABORATION.md)；没有把该版本声明为恶意多租户安全边界。

## 验证

```sh
python3 -m unittest discover -s tests -p 'test_*.py' -q
node tests/web_browser.cjs ./codex-cpp
python3 tests/collaboration_integration.py ./codex-cpp --protocol-fixture --web-browser
```

浏览器测试额外需要 Playwright 和 Chromium，仅测试时需要 Node。
本地浏览器测试使用真实 C++ Agent 与本地假 Provider，覆盖登录、审批写入、会话恢复、Provider 设置和手机布局。
多用户容器测试使用真实 Unix UID、socket、HTTP 登录、Git 和协议夹具；`--web-browser` 还通过测试 HTTPS 代理验证双用户网页、Secure Cookie、跨成员审批、提交和精确 SHA 合并确认（使用本机端口 18766/18767）。当前云容器不支持嵌套 procfs，
因此不能把容器协议测试当作真实 Agent 沙箱验证。真实 Agent 的审批和写入由本地原生测试验证。
尚未连接真实收费模型账号，也未部署至用户服务器。
