# Gitx 嵌入实验

本实验将 turtlelnc/Gitx 的仓库核心与其自带 libgit2 静态链接进 Codex.cpp。
启用后，本地网页 `/api/rpc` 的 `diff` 操作使用嵌入实现，返回 HEAD、状态、未跟踪文件和差异。
已暂存和未暂存的修改一起对比 HEAD，保持网页现有语义；响应增加 `backend: gitx/libgit2`。

这是第一阶段验证。多人服务的提交、发布、合并，以及 Agent 的 `git apply` 仍调用系统 Git。
本次没有启用 Gitx 的 SSH/HTTPS、AI、系统编辑器或交互式冲突处理，也未把整个 Gitx CLI 搬入服务。

## 构建

使用 Gitx 提交 `7221600c914c835e6f8b78942d9a3ab15a4558c3`：

```sh
git clone https://github.com/turtlelnc/Gitx.git ../Gitx
git -C ../Gitx checkout 7221600c914c835e6f8b78942d9a3ab15a4558c3
cmake -S . -B build-gitx -DCMAKE_BUILD_TYPE=Release \
  -DCODEX_WITH_GITX=ON -DCODEX_GITX_SOURCE_DIR="$PWD/../Gitx"
cmake --build build-gitx --parallel 4
./build-gitx/codex-cpp web --root /path/to/repository
```

Gitx 核心使用 C++20，Codex.cpp 保持 C++17，构建标准只作用于对应目标。
依赖从 Gitx checkout 的 third_party 目录构建，不需要安装系统 libgit2。
Gitx 与 libgit2 的源代码和许可证保留在该 checkout 中，分发产物时须附带各自许可证及依赖声明。
此源码包不包含 Gitx 副本，普通构建不受影响；关闭实验开关即继续使用原来的系统 Git 实现。

## 验证与下一步

```sh
node tests/gitx_trial.cjs build-gitx/codex-cpp
```

测试用系统 Git 生成独立的仓库和预期 Diff，再以空 PATH 启动应用，确认网页 Diff 无须发现或执行 Git。
覆盖空仓库、HEAD、状态、已暂存与未暂存修改、删除、未跟踪文件和输出截断。
Node 和系统 Git 在该测试中属于测试工具。

多人服务下一步需要给 Gitx 补非交互接口：外置裸仓库与工作区、精确 SHA 合并、冲突回滚、
本地对象传输，以及重启后的中断恢复。保持现有队列、权限和沙箱边界后，才能替换生产路径。
本实验的本地 Diff 不启动工具进程，不能作为 Windows Agent 沙箱已补齐的证据。

另外，Gitx 当前的 working_diff 输出回调未为普通内容行添加 libgit2 的 origin 字符，
不能原样作为标准 unified diff 使用。因此实验复用 Gitx 状态核心，并使用 libgit2 原生回调保留 +/-/空格前缀；
相应行为由与系统 Git 对照的测试覆盖。
