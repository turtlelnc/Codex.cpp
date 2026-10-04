# Remote access / 远程访问

网页入口由 `remote_bridge.py` 提供，运行在本机 `127.0.0.1:8765`，通过独立的 `cloudflared` Tunnel 连接到域名。它启动真实的 `codex-cpp --json` 进程；一轮结束后保存会话 ID，下一次提示词使用 `--resume`。会话和网页聊天历史保存在项目目录的 `.codex_cpp/` 下。

网页顶部的“会话”菜单会列出项目目录中的已保存会话。选择旧会话会加载它的聊天记录，下一次提示词会继续该会话；选择“新会话”则从下一次提示词开始创建新的会话。

## 本机准备

在仓库目录构建，先用终端完成所选 Provider 的登录：

```bash
clang++ -std=c++17 -O2 -Wall -Wextra -pedantic codex.cpp -o codex-cpp
./codex-cpp --provider codex --login
python3 remote_bridge.py --root /path/to/your/project
```

打开 `http://127.0.0.1:8765`，从 `~/.codex-cpp/remote-token` 读取令牌并在网页输入。令牌由服务首次启动时随机生成；此文件必须保持 `0600` 权限。若使用 API Key Provider，应在启动服务的进程环境中配置相应 Key。`--help` 可查看端口及 Provider 参数。

远程服务是单用户入口，同一时间只运行一个 Agent 回合。Agent 可按 `workspace-write` 沙箱策略改动 `--root` 下的文件，工具批准模式为 `on-request`；批准按钮会把确认输入传给当前进程。网页刷新后能继续查看当前回合。重启服务后能恢复最后完成的会话，但正在运行的回合不会自动重启。

## Cloudflare Access 与 Tunnel

推荐先在 Cloudflare Zero Trust 创建 **Self-hosted / Public hostname** Access 应用，域名填写将用于远程访问的完整域名。为应用创建 **Allow** 策略，只允许自己的邮箱。保存后在无登录状态的浏览器访问域名，确认 Cloudflare Access 登录页或拒绝响应出现。若无法启用 Access，也可只使用高强度随机令牌保护 API；这时公网登录页可见，持有令牌者可以读取聊天、提交提示词并批准工具。若域名已有站点或 DNS 记录，先核对并处理冲突，避免覆盖现有服务。

随后创建**独立**的本地管理 Tunnel（不要复用其他服务的 Tunnel）。下面把 `<tunnel-name>`、`<tunnel-uuid>`、`<hostname>` 替换为自己的值：

```bash
cloudflared tunnel create <tunnel-name>
```

在 `~/.cloudflared/codex-remote.yml` 写入：

```yaml
tunnel: <tunnel-uuid>
credentials-file: /Users/<username>/.cloudflared/<tunnel-uuid>.json
ingress:
  - hostname: <hostname>
    service: http://127.0.0.1:8765
  - service: http_status:404
```

然后创建 DNS 路由并启动 Tunnel：

```bash
cloudflared tunnel --config ~/.cloudflared/codex-remote.yml route dns <tunnel-uuid> <hostname>
cloudflared tunnel --config ~/.cloudflared/codex-remote.yml run
```

明确指定配置文件和 Tunnel UUID 可避免意外使用现有的默认 Tunnel。在浏览器输入网页令牌，测试发送提示词、查看进度和处理一次批准。`cloudflared` 和 `remote_bridge.py` 都必须在电脑上持续运行；电脑睡眠或断网时远程入口无法使用。要开机自启，可按 Cloudflare 官方服务安装说明配置 `cloudflared`，并用系统服务管理器运行 bridge。

启用 Access 时，它是第一层身份验证，本机随机令牌是第二层。仅用令牌时，静态登录页公开，API 仍要求令牌；令牌一旦泄露，应停止 Tunnel，删除 `~/.codex-cpp/remote-token`，重启 bridge 生成新令牌，再恢复 Tunnel。
