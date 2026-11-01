const { chromium } = require("playwright"),
  { spawn } = require("node:child_process"),
  fs = require("node:fs"),
  os = require("node:os"),
  path = require("node:path"),
  http = require("node:http"),
  net = require("node:net"),
  assert = require("node:assert/strict"),
  { environment } = require("./creator_test_helpers.cjs");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "creator-browser-")),
  env = environment(root),
  artifactDir =
    process.env.CODEX_ARTIFACT_DIR ||
    path.join(os.tmpdir(), "creator-artifacts");
let model, service, browser, origin;
const client = `export default ({attach,element,onDispose})=>{let n=0;const panel=element('section'),label=element('p','点击次数：0'),button=element('button','增加');panel.setAttribute('aria-label','Creator 点击计数器');button.type='button';const click=()=>label.textContent='点击次数：'+(++n);button.addEventListener('click',click);onDispose(()=>button.removeEventListener('click',click));panel.append(label,button);attach('sidebar',panel);};`;
const plan = [
  ["read_skill", { name: "penguin-creator" }],
  [
    "penguin_scaffold",
    { path: "created-plugin", id: "creator-demo", kind: "native" },
  ],
  ["write_file", { path: "created-plugin/client.js", content: client }],
  ["penguin_build", { path: "created-plugin" }],
  ["penguin_test", { path: "created-plugin" }],
  ["penguin_check", { path: "created-plugin" }],
];
async function port() {
  const s = net.createServer();
  await new Promise((r) => s.listen(0, "127.0.0.1", r));
  const n = s.address().port;
  await new Promise((r) => s.close(r));
  return n;
}
async function wait(fn) {
  for (let i = 0; i < 1200; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 50));
  }
  throw Error("Creator browser timed out");
}
async function start(extra = []) {
  const p = await port();
  origin = "http://127.0.0.1:" + p;
  let logs = "";
  service = spawn(
    binary,
    [
      ...extra,
      "web",
      "--root",
      root,
      "--port",
      String(p),
      "--provider",
      "custom",
      "--api",
      "chat",
      "--model",
      "creator-fixture",
      "--base-url",
      "http://127.0.0.1:" + model.address().port,
      "--no-api-key",
    ],
    { env },
  );
  service.stdout.on("data", (b) => (logs += b));
  service.stderr.on("data", (b) => (logs += b));
  await wait(async () => {
    if (service.exitCode !== null) throw Error(logs);
    try {
      return (await fetch(origin)).ok;
    } catch {
      return false;
    }
  });
}
async function stop() {
  if (service && service.exitCode === null) {
    service.kill("SIGTERM");
    await new Promise((r) => service.once("exit", r));
  }
  service = null;
}
(async () => {
  try {
    model = http.createServer(async (req, res) => {
      let body = "";
      for await (const chunk of req) body += chunk;
      if (req.url === "/models") {
        res.end(JSON.stringify({ data: [{ id: "creator-fixture" }] }));
        return;
      }
      const request = JSON.parse(body),
        messages = request.messages,
        index = messages.map((m) => m.role).lastIndexOf("user"),
        prompt = messages[index].content,
        outputs = messages.slice(index + 1).filter((m) => m.role === "tool");
      let message;
      if (prompt.includes("USE_GENERATED_TOOL")) {
        const manifest = JSON.parse(
          fs.readFileSync(path.join(root, "created-plugin", "penguin.json")),
        );
        message = outputs.length
          ? { role: "assistant", content: "GENERATED_TOOL_WORKS" }
          : {
              role: "assistant",
              content: null,
              tool_calls: [
                {
                  id: "generated-status",
                  type: "function",
                  function: {
                    name: manifest.tools[0].spec.name,
                    arguments: "{}",
                  },
                },
              ],
            };
      } else if (prompt.includes("DENY_CREATE"))
        message = outputs.length
          ? { role: "assistant", content: "DENIED_DONE" }
          : {
              role: "assistant",
              content: null,
              tool_calls: [
                {
                  id: "deny-create",
                  type: "function",
                  function: {
                    name: "penguin_scaffold",
                    arguments: JSON.stringify({
                      path: "must-not-exist",
                      id: "denied-demo",
                      kind: "web",
                    }),
                  },
                },
              ],
            };
      else {
        const step = plan[outputs.length];
        if (
          outputs.length &&
          !outputs[0].content.startsWith("---\nname: penguin-creator")
        )
          message = {
            role: "assistant",
            content: "CREATOR_FAILED: embedded skill missing",
          };
        if (
          outputs.length &&
          [
            "penguin_scaffold",
            "penguin_build",
            "penguin_test",
            "penguin_check",
          ].includes(plan[outputs.length - 1][0])
        ) {
          try {
            JSON.parse(outputs.at(-1).content);
          } catch {
            message = {
              role: "assistant",
              content: "CREATOR_FAILED: " + outputs.at(-1).content,
            };
          }
        }
        if (!message)
          message = step
            ? {
                role: "assistant",
                content: null,
                tool_calls: [
                  {
                    id: "creator-step-" + outputs.length,
                    type: "function",
                    function: {
                      name: step[0],
                      arguments: JSON.stringify(step[1]),
                    },
                  },
                ],
              }
            : {
                role: "assistant",
                content: "CREATOR_READY: 插件已生成并测试，尚未启用。",
              };
      }
      res.setHeader("Content-Type", "application/json");
      res.end(
        JSON.stringify({ id: "creator-response", choices: [{ message }] }),
      );
    });
    await new Promise((r) => model.listen(0, "127.0.0.1", r));
    await start();
    browser = await chromium.launch({
      ...(process.env.CHROMIUM_PATH
        ? { executablePath: process.env.CHROMIUM_PATH }
        : fs.existsSync("/usr/bin/chromium")
          ? { executablePath: "/usr/bin/chromium" }
          : {}),
      args: ["--no-sandbox"],
    });
    const context = await browser.newContext({
        viewport: { width: 1440, height: 1000 },
        permissions: ["clipboard-read", "clipboard-write"],
      }),
      page = await context.newPage(),
      errors = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await page.goto(origin);
    await page.locator("#prompt").fill("DENY_CREATE");
    await page.locator("#send").click();
    await page.getByRole("button", { name: "拒绝", exact: true }).waitFor();
    await page.getByRole("button", { name: "拒绝", exact: true }).click();
    await page.waitForFunction(() =>
      document.getElementById("messages").textContent.includes("DENIED_DONE"),
    );
    assert(!fs.existsSync(path.join(root, "must-not-exist")));
    await page.locator("#new-chat").click();
    await page
      .locator("#prompt")
      .fill("创建一个状态工具和点击计数器插件，构建、检查，但不要自动启用。");
    await page.locator("#send").click();
    let approved = 0;
    await wait(async () => {
      const text = await page.locator("#messages").textContent();
      if (text.includes("CREATOR_FAILED")) throw Error(text);
      if (text.includes("CREATOR_READY")) return true;
      const allow = page.getByRole("button", { name: "允许", exact: true });
      if (await allow.count()) {
        await allow.click();
        approved++;
      }
      return false;
    });
    assert.equal(approved, 4);
    const card = page.locator(".creator-review");
    assert.equal(await card.count(), 1);
    assert((await card.textContent()).includes("隔离加载测试通过"));
    assert((await card.textContent()).includes("未启用"));
    assert.equal(
      await page.getByRole("region", { name: "Creator 点击计数器" }).count(),
      0,
    );
    await card.getByRole("button", { name: "复制手动启用命令" }).click();
    assert(
      (await page.evaluate(() => navigator.clipboard.readText())).includes(
        "--penguin",
      ),
    );
    assert(
      (await page.evaluate(() => navigator.clipboard.readText())).includes(
        "created-plugin",
      ),
    );
    assert(
      !(
        await page.evaluate(() => fetch("/api/penguins").then((r) => r.json()))
      ).some((service) => service.owner === "creator-demo"),
    );
    await card.getByRole("button", { name: "查看清单" }).click();
    await page.waitForFunction(() =>
      document.getElementById("editor-text").value.includes("creator-demo"),
    );
    await page.locator("#inspector-close").click();
    await page.setViewportSize({ width: 390, height: 844 });
    await card.scrollIntoViewIfNeeded();
    assert.equal(
      await page.evaluate(
        () => document.documentElement.scrollWidth > innerWidth,
      ),
      false,
    );
    fs.mkdirSync(artifactDir, { recursive: true });
    await page.screenshot({
      path: path.join(artifactDir, "creator-review-mobile.png"),
    });
    // Only the test/user's explicit new process enables the generated plugin.
    await stop();
    await start([
      "--penguin",
      path.join(root, "created-plugin", "penguin.json"),
    ]);
    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.goto(origin);
    const widget = page.getByRole("region", { name: "Creator 点击计数器" });
    await widget.waitFor();
    await widget.getByRole("button", { name: "增加", exact: true }).click();
    await widget.getByRole("button", { name: "增加", exact: true }).click();
    assert((await widget.textContent()).includes("点击次数：2"));
    await page.locator("#new-chat").click();
    await page.locator("#prompt").fill("USE_GENERATED_TOOL");
    await page.locator("#send").click();
    await page.waitForFunction(() =>
      document
        .getElementById("messages")
        .textContent.includes("GENERATED_TOOL_WORKS"),
    );
    const journals = fs
      .readdirSync(path.join(root, ".codex_cpp", "sessions"))
      .filter((n) => n.endsWith(".items.jsonl"))
      .map((n) =>
        fs.readFileSync(path.join(root, ".codex_cpp", "sessions", n), "utf8"),
      )
      .join("");
    assert(journals.includes("creator-demo is ready."));
    await page.screenshot({
      path: path.join(artifactDir, "creator-enabled-plugin.png"),
    });
    await page.evaluate(() => Penguin.unmount("creator-demo"));
    assert.equal(await widget.count(), 0);
    assert.deepEqual(errors, []);
    console.log(
      "PASS mock Agent reads embedded Creator skill, approval deny/allow, scaffold/edit/build/isolated probe/review, clipboard and file inspection, mobile, no auto-activation, explicit restart, generated native tool and working UI counter/disposal",
    );
  } finally {
    if (browser) await browser.close();
    await stop();
    if (model) await new Promise((r) => model.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
