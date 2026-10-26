# 多用户远程工作区（第一版）

项目、Agent、Shell、凭据和会话均在服务器运行。本地 `codex-cpp connect` 调用 OpenSSH，显示服务器上的完整终端界面；不在本地执行项目工具。服务器使用一个 Linux 账号代表一个用户，每个账号有自己的项目和 tmux 会话。每位用户分别登录自己的 Codex 账号，或配置自己的 API Key。

```text
本地 codex-cpp connect
        │ SSH 公钥认证 + 加密终端
        ▼
专用 sshd（端口 2222）
        │ 根据已认证的 Linux UID 分配权限
        ▼
该用户的固定入口 → 该用户的 tmux → codex-cpp
        ├─ ~/workspaces/<项目名>/
        ├─ ~/.codex-cpp/          凭据、插件
        └─ 项目/.codex_cpp/       会话
```

这是面向受管理团队账号的共享 Linux 主机方案。用户能通过 Agent 在自己的权限范围内执行程序；隔离边界是 Linux UID、目录权限和操作系统内核。强对抗租户、独立网络策略或计费隔离应使用每用户独立 VM/容器。第一版不提供网页管理、注册、统一计费、自动仓库上传或 HTTP API。

用户协作、共同 Agent、分支发布和冲突处理见 [协作开发说明](COLLABORATION.md)。

## 服务器安装

支持安装 OpenSSH、Python 3、tmux、Bubblewrap 的 Debian/Ubuntu Linux + systemd 主机。先在服务器构建匹配该主机架构的二进制：

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
sudo sh server/install.sh "$PWD/codex-cpp"
```

安装器新增独立的 `codex-server.service`，配置位于 `/etc/codex-server/sshd_config`，已有配置会保留。它不会替换原有管理 SSH 的配置；按实际需要为 2222 端口配置防火墙，并用 `systemctl status codex-server` 查看状态。

**管理 SSH 也需要限制这些账号。** 在原有 SSH 服务的全局配置中追加 `DenyGroups codex-remote`（保留已有 DenyGroups），用 `sshd -t` 验证后重载原有服务。确认所有其他 SSH 实例及登录入口也采用相同账号策略。否则账号可以在自己的家目录添加密钥，经其他 SSH 入口绕过本服务的固定命令。保持现有管理员会话连接，先验证管理账号仍能登录。专用服务配置不包含此 DenyGroups。

添加两个用户，每个用户使用独立 SSH 公钥：

```bash
sudo codex-server-add-user alice /secure/path/alice.pub
sudo codex-server-add-user bob /secure/path/bob.pub
```

仅交给管理员公钥，不上传客户端私钥。命令拒绝覆盖已有账号、家目录或密钥文件。用户家目录在 `/srv/codex-users/用户名`，权限为 700；SSH 公钥保存在管理员所有的 `/etc/codex-server/keys/用户名`，Agent 无法改写认证入口。账号没有 sudo 权限，连接入口禁止 SSH 端口转发、Agent 转发、用户 rc 脚本以及任意远程命令。

## 本地连接

本地安装 OpenSSH 客户端，重新构建包含 `codex_remote.inc` 的 Codex.cpp：

```bash
./codex-cpp connect alice@server.example.com --port 2222 --project demo --check
./codex-cpp connect alice@server.example.com --port 2222 --project demo --login
./codex-cpp connect alice@server.example.com --port 2222 --project demo
```

第一次连接，向管理员核对 SSH 主机密钥指纹；管理员可用 `ssh-keygen -lf /etc/codex-server/ssh_host_ed25519_key.pub` 查看。保留主机密钥校验。私钥、跳板机等使用标准 `~/.ssh/config` 配置，也可直接使用 SSH Host 别名连接。项目名只允许以字母或数字开头的 1–64 位字母、数字、下划线、连字符，不接受服务器任意路径。未指定时为 `default`。

`--check` 返回依赖和真实 Bubblewrap 探测结果。所有项就绪后才允许启动 Agent；若 Linux 禁止用户命名空间，或嵌套容器限制 Bubblewrap，会拒绝启动。此时应调整服务器运行环境，不开启 `--unsafe-execution` 绕过检查。

`--login` 在服务器执行 Codex 设备登录，凭据只保存在该用户的服务器家目录。API 用户跳过此步骤，按下一节配置。在主界面继续使用 `/provider`、`/model`、审批、`/undo` 等现有命令。所有路径都指向服务器文件。

用 **Ctrl+B，再按 D** 断开终端，任务仍在服务器运行。重新连接同一账号和项目会回到同一个 Agent 进程；不同项目有独立的 tmux 会话。同一账号的多个客户端连接同一项目会共享终端，不应共用账号代表多个用户。`/quit` 结束该项目进程，再连接将开启新的 Agent，可用 `/resume` 恢复保存的对话。

如果任务正在等待审批，断线后继续等待，不会自动批准。服务器重启后 tmux 进程不会恢复，但已保存的会话仍可恢复。`/undo` 当前是进程内历史，服务器重启或 `/quit` 后会消失。

## 仓库和模型配置

管理员可先替用户导入服务器仓库：

```bash
sudo -u alice git clone https://github.com/OWNER/REPO.git /srv/codex-users/alice/workspaces/demo
sudo chmod 700 /srv/codex-users/alice/workspaces/demo
```

先连接一次 `--check` 会创建空的项目目录，Git 可克隆到该空目录。私有仓库访问由服务器管理员或账号所有者单独配置；服务不转发本地 SSH Agent，不共享管理员 Git 凭据。Shell 的网络权限仍遵循 Codex.cpp 的现有沙箱规则。

默认提供商是 `codex`。如需 API 或自定义服务，以对应账号的身份安全写入：

`~/.config/codex-server/config.json`：

```json
{"provider":"openai","model":"YOUR_MODEL"}
```

`~/.config/codex-server/credentials.json` 中填该用户自己的 `OPENAI_API_KEY`、`DEEPSEEK_API_KEY`；自定义提供商还可使用 `config.json` 中 `api_key_env` 指定的变量名。不要把真实凭据放入仓库、命令行参数或聊天。目录必须为 700，JSON 文件必须为 600 且由该用户所有，入口拒绝符号链接和其他可访问的配置文件。程序不打印凭据，密钥也不出现在 tmux 的启动参数中。

自定义服务配置示例：

```json
{"provider":"custom","api":"chat","base_url":"http://127.0.0.1:1234/v1","model":"local-model","no_api_key":true}
```

新 Agent 进程会重新读取配置；已运行的项目进程不自动重新加载。修改凭据或配置后用 `/quit` 退出该项目并重连。需要出口代理时，管理员在专用 `sshd_config` 中使用 `SetEnv HTTP_PROXY=... HTTPS_PROXY=...`，并把所需 CA 安装到服务器系统信任库；仍然启用 TLS 校验。

## 多用户资源和运维

安装器提供 PAM 的每账号进程数 256、文件描述符 4096、禁止 core dump 的基线限制。它们不限制 CPU、内存、磁盘或模型费用。

在启用 `pam_systemd`/systemd-logind 的主机上，可给每个用户的 slice 设置资源上限，例如：

```bash
account_uid=$(id -u alice)
sudo systemctl set-property "user-${account_uid}.slice" CPUQuota=200% MemoryMax=4G TasksMax=256
```

用 `systemd-cgls` 验证该用户的 SSH/tmux/Agent 进程确实位于受限 slice；不能仅凭保存了配置就认为限制生效。磁盘使用文件系统用户配额或每用户独立卷，网络和模型费用由管理员另行限制。这些资源策略需要在目标部署主机验证。

撤销账号访问时，管理员移走 `/etc/codex-server/keys/用户名`，再终止该账号已存在的登录、tmux 和 Agent 进程；仅移除公钥不会结束已认证连接和后台任务。先备份需保留的项目及会话，账号移除由管理员处理。

当前 Linux 执行器仍有已有的安全检查失败：工作区内的凭据路径排除和独立前端子进程限制尚未补齐。本方案将每个账号的认证和凭据置于项目工作区之外，并用真实跨 UID 权限测试验证账号间文件访问；它没有修复或宣称通过这些既有执行器测试。

## 验证

```bash
# 不需要服务、登录或真实密钥
python3 -m unittest discover -s tests -p test_remote_server.py -v

# 真实 OpenSSH + 两个 Linux 用户；需要本机 Docker，容器只映射到回环地址
python3 tests/remote_ssh_integration.py ./codex-cpp

# 真实 gateway + tmux + Agent 的断线重连；需要本机可用的 Bubblewrap
python3 tests/remote_terminal.py ./codex-cpp /usr/bin/tmux
```

当前验证：84 项 C++ 自测、14 项服务器基础测试、两账号真实 SSH 认证和跨账号权限测试通过；真实 tmux/Agent 断线重连后保留进程和撤销历史。测试容器中的嵌套 Bubblewrap 不可用，`--check` 正确返回未就绪；tmux/Agent 测试在支持 Bubblewrap 的外层环境单独通过。未在用户的目标服务器部署，未测试真实模型请求、Windows/macOS 客户端或生产资源配额。

## 网页入口

同一个二进制新增 `codex-cpp web`（本地）和 `codex-cpp serve`（多用户服务器），无需 TUI。直接部署与账号配置见 [WEB.md](WEB.md)。安装脚本安装网页服务，但需配置 HTTPS Origin 和账号后手动启用。
