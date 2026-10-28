# libgit2 直接嵌入实验

Codex.cpp 直接静态链接 libgit2，不再使用 Gitx 源码、封装或 C++20。
启用后，本地网页的 diff 接口直接返回 HEAD、状态、未跟踪文件和标准 unified diff，
响应增加 `backend: libgit2`。关闭开关则使用原来的系统 Git 后端。

## 构建

```sh
cmake -S . -B build-libgit2 -DCMAKE_BUILD_TYPE=Release -DCODEX_WITH_LIBGIT2=ON
cmake --build build-libgit2 --parallel 4
./build-libgit2/codex-cpp web --root /path/to/repository
node tests/libgit2_trial.cjs build-libgit2/codex-cpp
```

CMake 首次配置下载上游 libgit2 v1.8.4，并校验固定 SHA-256。
之后复用构建目录；离线构建可通过 CMake 的
`-DFETCHCONTENT_SOURCE_DIR_CODEX_LIBGIT2=/path/to/libgit2` 指定预先取得的源码。
无需 Gitx checkout，也无需安装系统 libgit2；程序保持 C++17，libgit2 使用 C 编译器。
SSH、HTTPS 及 libgit2 CLI/测试构建关闭，实验只使用本地仓库功能。
分发二进制时须附带 libgit2 的 COPYING 及其依赖许可声明；上游采用 GPLv2 加链接例外，
不能因为静态链接而省略其许可证。源码存放在构建目录的 _deps/codex_libgit2-src。

## 范围与验证

测试使用系统 Git 准备仓库和独立的预期输出，应用自身以空 PATH 运行。
覆盖空仓库、HEAD、状态、已暂存/未暂存/删除的 Diff、未跟踪文件和输出截断。
Diff 保留 +/-/空格前缀，与系统 Git 输出对照；状态返回 libgit2 flags 数值，不依赖中文文本解析。

这仍是只读接入实验，多人服务的提交、发布、合并以及 Agent 的 git apply 仍依赖系统 Git。
后续需要实现外置裸仓库、精确 SHA 合并、冲突回滚、本地对象传输和中断恢复，
验证权限与隔离边界后再替换这些路径。它也不代表 Windows Agent 沙箱已经实现。
