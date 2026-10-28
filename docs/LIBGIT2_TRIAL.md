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
CODEX_TEST_NO_GIT=1 node tests/native_web_test.cjs build-libgit2/codex-cpp
```

第一项用 Git 准备独立预期输出，应用以空 PATH 运行；覆盖空仓库、状态、HEAD、
暂存/未暂存/删除 Diff、递归未跟踪文件、忽略规则与截断。
最后一项为服务进程提供一个执行即失败的 git 替身，验证初始化、提交、发布、精确 SHA 检查、
冲突不改目标、人工解决后发布、请求去重、SIGKILL 及正常停止后的恢复。
测试运行器仍用系统 Git 创建独立断言和模拟外部操作者；它不是协调器的运行依赖。
Linux 需要可用的 bubblewrap 沙箱与 curl；本地测试不消耗模型额度。

这个开关只替换网页与协作后端。Agent 主动执行 Git 命令、现有 apply_patch 工具等仍可能需要系统 Git，
尚未将整个 Agent 的 Git 使用都替换成库调用。curl 依赖同样不在本次替换范围内。
