# libgit2 直接嵌入：本地网页与多人协作

Codex.cpp 直接静态链接 libgit2，不使用 Gitx 或 C++20。启用后，本地网页 Diff/状态，
以及多人服务的仓库初始化、暂存、提交、本地对象传输、分支枚举、发布、同步与合并都不调用系统 Git。
关闭构建开关时仍使用已有的系统 Git 后端。

## 构建

```sh
cmake -S . -B build-libgit2 -DCMAKE_BUILD_TYPE=Release -DCODEX_WITH_LIBGIT2=ON
cmake --build build-libgit2 --parallel 4
./build-libgit2/codex-cpp web --root /path/to/repository
```

多人服务的账户登记、HTTPS 代理与启动参数沿用 README：

```sh
./build-libgit2/codex-cpp serve --state-dir /path/to/private-state --origin https://codex.example.com
```

CMake 首次配置下载上游 libgit2 v1.8.4，并验证固定 SHA-256；以后复用构建目录。
离线构建可使用 `-DFETCHCONTENT_SOURCE_DIR_CODEX_LIBGIT2=/path/to/libgit2` 指定预先取得的源码。
无需安装系统 libgit2。主程序保持 C++17，libgit2 使用 C 编译器；SSH/HTTPS 传输、CLI 和依赖自测构建关闭。
服务只在本机不同仓库之间传输对象，不接受远程 URL。

分发二进制须附带 libgit2 的 COPYING 和依赖许可声明；libgit2 采用 GPLv2 加链接例外，
不能省略其许可证。依赖源码位于构建目录的 `_deps/codex_libgit2-src`。

## 执行边界与冲突

本地网页读取仓库使用 libgit2 API。多人服务仍将仓库操作放在受限子进程中运行：
同一可执行文件的内部 worker 直接调用 libgit2，只获得当前工作区、可信仓库以及明确允许读取的来源仓库。
保持原来的超时、输出限制、任务队列、成员权限、审批和请求去重。没有因嵌入库而移除系统沙箱。

合并前要求工作区干净，执行时重新校验来源和目标 SHA。先使用内存索引合并：
若有冲突，只返回冲突文件，不改分支、磁盘索引或工作文件；若无冲突，生成含两个父提交的合并提交。
首次修改工作区前写 MERGE_HEAD 恢复标记，检出完成后以旧 HEAD 为条件更新分支，再清理标记。
进程中断时仍阻塞项目，创建者必须检查并确认恢复；未完成的正式合并不能直接放行。
发生这种情况，管理员需先检查工作区、正式仓库及 MERGE_HEAD，再决定保留结果还是回退。

个人工作区的 `.git` 副本仍供 Agent 使用。发布会导入其中的提交，保留手工解决冲突的合并祖先。
人工解决冲突的外部工具可以是 Git 或其他兼容客户端；网页服务正常提交、发布、合并无需它们。
Windows 的 Agent 沙箱仍未实现，依赖该沙箱的多人 worker 会拒绝执行；这是独立的平台限制。

## 验证

```sh
node tests/libgit2_trial.cjs build-libgit2/codex-cpp
node tests/native_smoke.cjs build-libgit2/codex-cpp
node tests/agent_git_test.cjs build-libgit2/codex-cpp
CODEX_TEST_NO_GIT=1 node tests/native_web_test.cjs build-libgit2/codex-cpp
```

第一项用 Git 准备独立预期输出，应用以空 PATH 运行；覆盖空仓库、状态、HEAD、
暂存/未暂存/删除 Diff、递归未跟踪文件、忽略规则与截断。
最后一项为服务进程提供一个执行即失败的 git 替身，验证初始化、提交、发布、精确 SHA 检查、
冲突不改目标、人工解决后发布、请求去重、SIGKILL 及正常停止后的恢复。
测试运行器仍用系统 Git 创建独立断言和模拟外部操作者；它不是协调器的运行依赖。
Linux 需要可用的 bubblewrap 沙箱与 curl；本地测试不消耗模型额度。

## Agent 补丁与结构化 Git 工具

同一构建中的 apply_patch 直接使用 libgit2，不调用 git apply，也不经过 Shell。
输入仍是 unified Git diff（不是 *** Begin Patch 格式）。支持普通目录以及已有 Git 仓库中的
新增、修改、删除、重命名；不修改真实仓库的暂存区。先验证所有路径、再预检查整份补丁，
不匹配的补丁在实际写入前拒绝；操作系统 I/O 故障或中途取消后仍需检查实际文件，不能盲目重放。
拒绝目录逃逸、符号链接、硬链接以及 .git/.codex_cpp/.codex-cpp 内部路径，保留既有写审批与只读限制。

模型会收到两个工具及对应的 developer 指令，优先用它们替代 Shell Git：

- git_read：status、diff、log、branches，只读模式可调用，不要求写审批。
- git_write：init、stage、stage_all、commit、branch、checkout、merge；需要写权限，按当前审批策略确认。
  所有写操作携带上次观察到的 target HEAD；空仓库使用空字符串。commit 署名为 Codex.cpp。
  stage 接收显式文件路径，stage_all 才表示暂存全部允许的文件；跳过 Agent 内部状态。
  branch 只创建不切换；checkout 要求工作区干净；merge 必须提供精确来源 SHA，并保持冲突时目标不变。

例如（未使用的字段仍需给空值，以满足严格的工具 schema）：

```json
{"operation":"status","reference":"","limit":20}
```

```json
{"operation":"stage","reference":"","target":"上次读取的40位HEAD","message":"","paths":["src/main.cpp"]}
```

这些接口只操作当前工作区及其内部 Git 元数据；不接受命令字符串、任意 Git 参数或外部仓库路径。
没有开放 push/fetch/reset/clean/rebase/stash/强制覆盖等操作，切换/合并也不能覆盖 Agent 内部状态。
多人会话中的模型操作工作区 .git 副本，正式发布与共享分支合并仍由协调器完成。
工作区外置元数据的 linked worktree 不在这两个模型工具的支持范围内。

工具仍在 OS 沙箱子进程中执行；请求经 stdin JSON 传递（小于 900 KB），补丁原文上限 700 KB，
输出上限 200 KB，执行上限 120 秒，支持中断。Windows 默认沙箱未实现的限制仍然存在。
真实 Agent 回归使用本地假模型，确认工具 schema、读/写审批、拒绝与只读模式、各 Git 操作及补丁保护；
运行中的 Agent 使用一执行就失败的 git 替身，测试运行器的 Git 仅用于独立断言与准备恶意历史。

因此，启用此构建后，网页/多人协作、apply_patch 以及上述模型工具都不需要系统 Git。
模型仍能通过 shell 主动运行用户明确需要的其他 Git 命令，那些命令需要系统 Git；curl 依赖不在本次范围内。
