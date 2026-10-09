# Remote feature archive

此目录完整保存远程版源码快照，来源提交为 `bb76432`。包含 C++ Runtime、Python 网页桥接、网页资源、部署说明及原有检查程序。

功能包括 Cloudflare Tunnel 入口、令牌认证、会话切换、新建会话、模型设置、模型文本增量显示，以及工具批准。源码保持封存时的行为与局限；本目录不会被根目录的普通构建命令编译。

根目录已恢复到加入远程功能之前的精简版本 `280e8cb`，继续使用根目录的 README 构建终端程序。

如需恢复远程版，在此目录按 README 构建 `codex-cpp`，然后按 REMOTE.md 启动桥接和 Tunnel。仓库不包含登录凭据、远程令牌、Tunnel 凭据或本机会话。

封存时，本机的远程桥接与 Tunnel 登录自启已停止。原 LaunchAgent 配置保存在本机 `~/Library/Application Support/Codex.cpp/remote-archive/`；会话和 Cloudflare 配置仍留在原来的本机位置。域名路由保留，恢复服务前远程入口不可用。
