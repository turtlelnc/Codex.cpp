# Everything is a Penguin（实验性原生模块架构）

内置模型、Agent 回合、提示词、工具执行、会话写入，以及 WebUI 渲染都通过命名服务调用。内置模块与扩展使用同一套注册、依赖和显式替换规则；没有加载扩展时仍是同一个自包含 C++17 可执行文件。原有 `plugin.json` 声明式扩展及 MCP 保持可用。

这借鉴 Everything is a Plugin 的组合方式，不兼容 DeepSeek/Cordis 的 Node.js 插件。它不是将整个工程重写为另一个前端框架，也不承诺每个内部函数都是公共 API。

## 运行示例

只在开发扩展时构建示例；普通构建和 Release 不额外编译它：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target codex-cpp penguin-demo --parallel 2

./build/codex-cpp --penguin "$PWD/build/penguin-demo/penguin.json" penguin inspect
./build/codex-cpp --penguin "$PWD/build/penguin-demo/penguin.json" web \
  --root /path/to/project --provider custom --api chat \
  --base-url http://127.0.0.1:9 --model penguin-demo --no-api-key
```

示例完全离线：替换模型、模型列表、包裹 Agent 驱动、增加 `penguin_status` 工具，同时在侧栏添加内容、装饰 Markdown 渲染。发送 `run_penguin_tool` 会实际调用原生工具再回复。默认内置模型与示例模型的回复不同，便于检查是否真正加载。

Windows 多配置生成器使用 `build/Release/codex-cpp.exe` 与 `build/penguin-demo/Release/penguin.json`；运行参数中必须提供绝对路径。macOS 示例产物为 `.dylib`，Linux 为 `.so`，Windows 为 `.dll`，架构必须与宿主一致。

`--penguin` 必须放在 `web`、`serve` 或普通 Agent 参数之前，可以重复使用；排列顺序就是覆盖顺序。清除此启动参数即可回到原有实现，无需删除用户会话。Web/多人服务会把管理员指定的模块列表传给 Agent 子进程；终端、网页用户或模型不能通过 RPC 安装原生代码。

原生模块具有宿主进程的完整权限；显式加载就等于信任其代码，不能把它当作沙箱里的普通工具。现有工具路由仍在模块回调之前做只读检查和审批；给原生工具声明 `mutates` 是模块作者的契约，无法约束恶意原生代码。

## 注册与生命周期

- 清单 `api: 1`，`id` 使用小写字母、数字、下划线、连字符，最长 32 字符。
- `requires` 是已经加载的模块 ID。依赖不存在、重复 ID、错误 ABI、未声明服务、越过包目录的库路径均启动失败。
- `services` 是“服务名 → 当前提供模块 ID”；新增服务填写空字符串。替换必须明确写当前 owner，不能靠后加载静默抢占。
- 一整个模块验证完成后才发布注册项，冲突不会留下半个服务。
- `next` 调用同一服务的上一层，可以装饰默认行为，也可以不调用它来完整替换行为。
- C++ 注册表启动后冻结。原生库保持到进程结束，不支持运行中卸载，避免悬空函数指针及执行中的 Agent 状态丢失。不要把注册事务回滚理解为能撤销任意原生初始化副作用。
- 浏览器模块有单独的生命周期；移除时先清理其 DOM/CSS，再恢复被覆盖服务。存在依赖或上层覆盖时拒绝移除。

查看当前服务：命令行 `penguin inspect` 输出 JSON；TUI `/penguin`；WebUI 左下角“模组”。它们显示 owner 和覆盖链，不显示密钥。

## C ABI

公共头文件为 [`penguin/native.h`](../penguin/native.h)，示例为 [`examples/penguin/native.cpp`](../examples/penguin/native.cpp)。模块导出 `codex_penguin_init_v1`，通过 `host.bind` 注册清单中的服务。

所有请求和最终结果都是 UTF-8 JSON，使用明确长度；不在模块间传递 STL 对象、分配器或应用对象。缓冲区、`next` 及回调只在当前调用期间有效，不能保存到异步线程。插件必须自行保证线程安全，禁止异常跨越 C ABI。长时间工作应定期调用 `call.poll(call.emit_context)`，非零时停止；模型服务的 poll 会泵送 UI 并检查取消。阻塞且不配合的原生代码无法安全强制终止单个线程，Web 服务仍可终止整个 Agent 子进程。

`emit` 的通道：

| 通道 | 内容 |
| --- | --- |
| `result` | 一个最终 JSON 结果，每次调用恰好一次 |
| `text` | 正文增量，普通 UTF-8 文本 |
| `reasoning` | 提供商真实返回的思考增量 |
| `error` | 错误文字，调用失败，不自动重试有副作用的操作 |

单条消息最多 16 MiB，一次调用输出累计最多 32 MiB；递归深度最多 32。一个库里声明多个服务即可实现跨模块协作，不需要链接宿主符号或使用同版本 C++ 标准库。

## 后端服务契约（v1）

| 服务 / 内置 owner | 请求与结果 |
| --- | --- |
| `model.request` / `builtin.model` | 请求 `kind` 为 `chat`、`responses` 或 `text`；携带 `config` 及原调用需要的 `messages`/`input`/`tools`/`instructions`/`text`。messages/input/tools 是原协议 JSON 字符串。返回 `{id,text,calls:[{name,call_id,arguments}]}`，无正文时 text 为 null。支持流式通道。 |
| `model.catalog` / `builtin.model` | 请求 `{config}`，返回 `[{id,description,default_effort,efforts:[]}]`。 |
| `agent.turn` / `builtin.agent` | 请求 `{prompt,review_only,continuing}`，返回 `{exit_code}`；`next` 保留完整内置 Agent、队列、流式、取消、审批与 Git 行为。完全替换驱动的模块需自己实现相应协议与状态事件。 |
| `prompt.build` / `builtin.prompt` | 请求/结果 `{instructions,user_prefix}`。这是最终提示词组装的替换点，不改变宿主实际权限。 |
| `tool.execute` / `builtin.tools` | 已经过审批的 `{name,arguments,workspace}`；返回 `{exit_code,output,images:[]}`。调用 next 时禁止更改刚获批的工具、参数或工作区。 |
| `tools.NAME` / 扩展自身 | 清单 tools 声明工具；请求 `{arguments,workspace}`，返回 `{exit_code,output}`。同样先经过宿主权限检查。 |
| `journal.append` / `builtin.journal` | `{path,item}`，item 为原始 JSON 对象字符串，返回 `{ok,item}`。只有确认成功后才更新会话内存。默认实现保留字节格式。 |
| `journal.replace` / `builtin.journal` | `{path,items:[原始JSON字符串]}`，返回 `{ok,items}`；默认实现先暂存再替换。 |

`config` 包含模型、地址、思考强度和认证模式，不把 API Key 复制进请求 JSON；内置下一层继续使用原凭据。原生代码本身仍是完全可信代码，这并不是密钥隔离边界。

会话读取、枚举、元数据和事件日志目前仍使用兼容的本地存储布局，因此仅替换写入服务并不能无缝换成远程数据库。Git、团队鉴权和 OS 隔离继续作为宿主基础设施；还没有承诺稳定的独立替换接口。

## WebUI 扩展

清单的 `web.entry` 指向一个自包含 ES 模块，`web.style` 可选。只提供这两项的包不需要原生库。资源只能来自显式加载的包目录，只有已认证的浏览器能访问；不提供通用文件服务器或任意 import 路径。

```js
export default ({ register, attach, element, onDispose }) => {
  register('markdown.render', (request, next) => {
    const body = next(request);
    body.dataset.custom = 'yes';
    return body;
  }, 'builtin.web');
  attach('sidebar', element('div', '我的扩展'));
};
```

`register` 同样要求显式 owner。可替换 `markdown.render`、`message.user`、`message.assistant`、`tool.card`。Markdown 渲染必须返回 `.md` HTML 元素；Agent 消息必须包含 `.md`，以便复用既有流式 DOM 和滚动逻辑。渲染服务必须同步，无法返回 Promise。默认实现仍通过 textContent 构造非可信文本。

`attach` 的位置是 `sidebar`、`composer`、`inspector`。通过 `onDispose` 登记自建定时器或事件监听器的清理函数；attach 的 DOM 和清单 CSS 自动清理。加载失败会回滚当前模块的声明式注册和已登记效果。插件脚本是可信代码，可访问当前网页与会话，不能当成跨用户权限边界。服务端只加载管理员启动时指定的脚本，项目成员无法上传并自动执行。

这不是完整第三方前端兼容协议；直接修改内部 DOM、替换整个页面或依赖未公开变量不能保证升级兼容。需要独立前端时仍可使用既有 `--web-dir` 或终端 frontend 协议。

## 平台与验证边界

- 默认可执行文件仍内嵌所有标准 WebUI 文件，不需要插件目录、Node.js 或 Python。
- 原生动态库需要系统动态加载器。现有 **Linux musl 全静态 Release 无法 dlopen 原生库**；该版本可用内置模块及纯 WebUI 扩展。Linux 原生库扩展请使用正常 glibc 构建。macOS/Windows 采用系统加载器。不要把默认单文件与“包含任意未安装模组”混为一谈。
- ABI v1、各服务字段均为实验协议。没有跨平台原生热卸载、远程插件市场或 Cordis 插件兼容层。
- `tests/penguin_runtime_test.cpp` 检查覆盖、回滚、依赖、并发、递归和冻结。
- `tests/penguin_integration.cjs` 用实际动态库验证模型、Agent、工具、默认回退、错误清单与只读拦截。
- `tests/penguin_browser_test.cjs` 用实际 C++ 服务验证模块传给 Agent 子进程、UI 替换/移除、认证资源和刷新恢复。
- 原有浏览器布局、文件保存竞态、Markdown 标题、渐显逐帧、工具审批与多人流程继续回归。

## Agent 制作插件

内置 Creator Skill、SDK 模板及制作/构建/静态检查/隔离探测流程见 [Penguin Creator](PENGUIN_CREATOR.md)。测试不会向当前服务启用插件；实际加载仍由用户或服务器管理员显式重启。
