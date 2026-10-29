# WebUI 状态与交互回归

保留原有米白主题、暖色强调和中文界面，继续使用原生 JavaScript、HTTP 快照轮询与 C++17 服务。此次没有改变 API、权限或沙箱协议，也没有增加应用运行时依赖。

## 状态边界

- 工作区、项目、作用域和会话切换使旧请求失效；会话切换期间暂停快照应用。
- 文件加载另外使用请求序号；输入修订号防止读取期间的新编辑被覆盖。
- 保存以提交时的内容为基线，保存期间的新编辑继续显示为未保存。失败保留文本；磁盘冲突继续走原有版本校验。
- 发送操作使用逻辑锁和持久化请求 ID，失败后保留文本并允许安全重试。明确切换会话后开始新的发送上下文。
- 快照比较实际内容；历史消息按会话、位置/工具 ID 和内容缓存 DOM。流式更新仅替换变化节点；终端和待审批 Diff 单独检测变化。
- 保留忙时 400ms、空闲 1200ms 轮询。网络请求 35 秒超时，防止单次挂起永久阻塞轮询；自动恢复加手动重连。超时不会自动重发写操作。
- 未发送的输入按会话在当前页面内暂存，切换回来可继续编辑；工具卡片支持键盘展开，阅读历史时提供返回最新消息按钮。
- 桌面和窄屏侧栏分别记忆状态，主内容固定在明确的网格列，避免折叠后的隐式自动布局。

## 浏览器测试

开发验证需要 Node.js、Playwright 与 Chromium；软件本身不需要这些工具。

```sh
npm install --prefix /tmp/codex-webui-tests playwright@1.62.1
/tmp/codex-webui-tests/node_modules/.bin/playwright install chromium
NODE_PATH=/tmp/codex-webui-tests/node_modules node tests/webui_quality_test.cjs
```

可用 `CHROMIUM_PATH` 指定浏览器，`WEBUI_SCREENSHOTS` 指定已存在的截图目录。CI 单独安装测试工具，不给应用添加 package.json 或前端构建过程。

`webui_quality_test.cjs` 在真实 Chromium 中使用可控 HTTP 响应验证布局断点、保存期间编辑、文件响应乱序、600 条消息的节点/选择/滚动保留、等长草稿变化、连接恢复、发送去重、会话切换旧响应、刷新恢复、审批失败重试、停止任务、保存失败保留编辑和项目/作用域隔离。它是前端协议回归，不代替真实后端集成测试。

`node tests/native_browser_test.cjs build/codex-cpp` 验证真实 C++ 服务的本地编辑器、模型菜单、审批与 Agent 写入、移动端，以及 HTTPS 多用户登录、项目邀请和个人任务。

## 边界

本轮自动交互验证使用 Linux Chromium 的桌面和移动视口；尚未在实体触屏设备、Safari、Firefox 或屏幕阅读器中验证。长会话仍传输完整快照并进行线性内容比较，未引入虚拟列表或 SSE；测试覆盖 600 条消息，不代表任意规模下的性能承诺。
