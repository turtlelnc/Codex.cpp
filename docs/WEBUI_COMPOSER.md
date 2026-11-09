# WebUI 输入辅助

## 插件引用

在输入框行首或空格后输入 `@`，可筛选当前 Agent 可用插件。上下键选择，Enter/Tab 插入 `@插件ID`；保留光标前后的任务文字，不立即发送。电子邮件地址中间的 `@` 不触发菜单。

目录由经认证的 `plugin_catalog` 读取操作返回：显式加载的原生模组，以及 Agent 凭据目录里已启用、清单有效的用户插件。接口只返回 ID 和说明，不返回配置、密钥或路径。选择引用不安装或启用插件；其工具、技能和权限继续使用既有 Agent 插件机制。没有匹配项时明确显示空结果。

## 斜杠菜单

上下键循环选择时选中项始终滚入菜单可视区，保持输入框焦点，并通过 `aria-activedescendant` 标识选中项。Enter/Tab 选择，Esc 关闭。窄屏菜单高度受视口约束。

## Design

- `/design`、`/design on`：后续任务启用既有 Design 模式。
- `/design off` 或输入框上方“退出设计”：关闭模式。
- `/design 任务`：单次 Design 任务。
- `/design plan 任务`：单次只读设计方案。

WebUI 把模式作为该任务的字段交给服务端。服务端通过 `--design` 启用原有设计指令和审阅工具；plan 强制使用只读沙箱，不改变工作区默认权限。重试 ID 同时绑定文本、Design 和 plan 模式，普通任务继续兼容原有重试记录。新会话或项目切换会清除输入框的 Design 状态。需要服务端声明 `design.v1`；旧服务会显示更新提示，而不是把请求当成普通任务执行。

## 验证

`tests/webui_quality_test.cjs` 覆盖键盘滚动、插件筛选/插入、邮箱和空结果、Design 开关和 plan 请求字段，以及原有输入和交互回归。

`node tests/webui_design_test.cjs /path/to/codex-cpp` 使用真实 C++ 服务及本地 HTTP 模型桩，验证插件目录和引用上下文、可写 Design 审阅、只读 plan、模式绑定重试和普通模式隔离；不调用外部或付费模型。
