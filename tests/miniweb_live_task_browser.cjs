const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const { createInterface } = require("node:readline");
const { once } = require("node:events");

// Keep the original task app intact. Add a test-only help panel to exercise
// stacking, scrolling, and pointer targeting with its actual UI behind it.
const helpPanel = `
<style>
#scroll-mode { position: fixed; z-index: 4; top: 0; right: 0; width: 72px; height: 24px; background: #8ac8bf; border: 0; }
.app { overscroll-behavior: auto contain; }
#tasks { height: 148px; overflow: auto; overscroll-behavior: inherit; }
@media (max-width: 460px) { #tasks { overscroll-behavior: initial; overscroll-behavior-y: inherit; } }
#task-sticky { position: sticky; top: 0; z-index: 2; height: 22px; background: #abcbee; }
body { min-height: 150vh; }
#open-help { position: fixed; right: 12px; bottom: 12px; width: 88px; height: 36px; }
#help-shade { display: none; position: fixed; inset: 0; z-index: 10; background: rgba(0,0,0,.5); }
#task-help {
  position: absolute; left: 50%; top: 50%; transform: translate(-50%,-50%);
  width: 300px; height: 200px; background: #efc987;
}
#help-scroll {
  position: absolute; left: 12px; right: 12px; top: 34px; bottom: 12px;
  overflow: auto; background: #c8d9ff;
}
#help-spacer { height: 280px; }
#close-help { display: block; width: 120px; height: 36px; background: #66bb90; border: 0; }
</style>
<button type="button" id="scroll-mode" aria-label="Enable scroll chaining">Chain</button>
<button type="button" id="open-help">Task help</button>
<div id="help-shade">
  <section id="task-help" role="dialog" aria-modal="true" aria-label="Task help">
    <strong>Task help</strong>
    <div id="help-scroll">
      <div id="help-spacer">Scroll to the end to close this help panel.</div>
      <button type="button" id="close-help">Close help</button>
    </div>
  </section>
</div>
<script>
(function () {
  var shade = document.getElementById("help-shade");
  var list = document.getElementById("tasks");
  var sticky = document.createElement("li");
  sticky.id = "task-sticky";
  sticky.textContent = "Tasks";
  list.insertBefore(sticky, list.firstChild);
  var form = document.getElementById("add-form");
  var input = document.getElementById("new-task");
  for (var i = 1; i <= 8; i++) {
    input.value = "Test task " + i;
    form.dispatchEvent(new Event("submit", { cancelable: true }));
  }
  input.blur();
  var mode = document.getElementById("scroll-mode");
  mode.addEventListener("click", function () {
    var next = list.style.getPropertyValue("overscroll-behavior-y") === "none" ? "auto" : "none";
    list.style.setProperty("overscroll-behavior-y", next);
  });
  window.addEventListener("scroll", function () {
    mode.style.background = window.scrollY ? "#ee9955" : "#8ac8bf";
  });
  list.addEventListener("change", function (event) {
    if (event.target === list.lastElementChild.querySelector("input") && event.target.checked)
      sticky.style.background = "#66bb90";
  });
  document.getElementById("open-help").addEventListener("click", function () {
    shade.style.display = "block";
  });
  document.getElementById("close-help").addEventListener("click", function () {
    shade.style.display = "none";
  });
})();
</script>`;

module.exports = async function liveTaskParity(browser, binary) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "miniweb-live-task-"));
  const source = fs.readFileSync("miniweb/test/live-task.html", "utf8");
  const html = source.replace("</body>", helpPanel + "</body>");
  fs.writeFileSync(path.join(root, "task.html"), html);
  try {
    for (const [width, height] of [[640, 480], [390, 844]]) {
      const page = await browser.newPage({ viewport: { width, height } });
      const child = spawn(binary, ["preview", "session"], {
        cwd: root,
        env: {
          ...process.env,
          CODEX_CPP_HOME: path.join(root, "profile"),
          CODEX_CPP_PORTABLE_DIR: "",
          CODEX_CPP_PORTABLE_PROJECT: "",
        },
        timeout: 35000,
      });
      const finished = once(child, "close");
      const output = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
      let stderr = "";
      child.stderr.on("data", (chunk) => { stderr += chunk; });
      let session;
      async function request(operation, fields = {}) {
        child.stdin.write(JSON.stringify({ operation, session, ...fields }) + "\n");
        const line = await output.next();
        assert(!line.done, stderr || "MiniWeb session exited before responding");
        const response = JSON.parse(line.value);
        assert.equal(response.exit_code, 0, response.output);
        return response;
      }
      async function click(x, y) {
        await request("click", { x, y });
        await page.mouse.click(x, y);
      }
      async function capture(stage, points) {
        const mini = Buffer.from((await request("screenshot")).images[0].split(",")[1], "base64");
        const chrome = await page.screenshot();
        if (process.env.CODEX_ARTIFACT_DIR) {
          for (const [engine, image] of [["miniweb", mini], ["chromium", chrome]]) {
            fs.writeFileSync(path.join(process.env.CODEX_ARTIFACT_DIR, `task-${width}-${stage}-${engine}.png`), image);
          }
        }
        return page.evaluate(async ({ sources, points }) => {
          return Promise.all(sources.map(async (source) => {
            const image = new Image();
            image.src = "data:image/png;base64," + source;
            await image.decode();
            const canvas = document.createElement("canvas");
            canvas.width = image.width;
            canvas.height = image.height;
            const ctx = canvas.getContext("2d");
            ctx.drawImage(image, 0, 0);
            return points.map(([x, y]) => Array.from(ctx.getImageData(x, y, 1, 1).data).slice(0, 3));
          }));
        }, { sources: [mini.toString("base64"), chrome.toString("base64")], points });
      }
      try {
        const opened = await request("open", { path: "task.html", width, height, wait_ms: 0 });
        session = opened.output.match(/Opened preview session ([^. ]+)\./)[1];
        await page.setContent(html);

        const x = width / 2, y = height / 2;
        const panel = [x + 145, y - 85], outside = [x + 153, y - 85], shade = [5, 5];
        const gold = [239, 201, 135], green = [102, 187, 144], blue = [200, 217, 255];
        const before = await capture("before", [panel, shade]);
        await click(width - 56, height - 30);
        const openedPixels = await capture("open", [panel, outside, shade]);
        for (let i = 0; i < openedPixels.length; i++) {
          assert.deepEqual(openedPixels[i][0], gold, `Centered help panel at ${width}px, engine ${i}`);
          assert.notDeepEqual(openedPixels[i][1], gold, "Panel boundary");
          assert(openedPixels[i][2].every((value, channel) => value < before[i][1][channel]), "Shade dims the page");
        }
        await click(shade[0], shade[1]);
        const closePoint = [x - 35, y + 72];
        assert.deepEqual(await capture("clipped", [closePoint]), [ [blue], [blue] ]);

        await request("scroll", { x, y, delta_y: 500 });
        await page.mouse.move(x, y);
        await page.mouse.wheel(0, 500);
        await page.waitForFunction(() => document.getElementById("help-scroll").scrollTop > 100);
        assert.deepEqual(await capture("scrolled", [closePoint]), [ [green], [green] ]);
        await click(closePoint[0], closePoint[1]);
        await page.locator("#task-help").waitFor({ state: "hidden" });
        assert.deepEqual(await capture("closed", [panel, shade]), before, "Clicking the scrolled control restores the task page");
        const list = page.locator("#tasks");
        const rect = await list.boundingBox();
        const wheelX = Math.round(rect.x + rect.width / 2);
        const wheelY = Math.round(rect.y + 65);
        async function wheel(delta) {
          await request("scroll", { x: wheelX, y: wheelY, delta_y: delta });
          await page.mouse.move(wheelX, wheelY);
          await page.mouse.wheel(0, delta);
          // A frame boundary lets Chromium commit the wheel default action.
          await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
        }
        const indicator = [width - 5, 5];
        const stickyPoint = [wheelX, Math.round(rect.y + 7)];
        await wheel(800);
        await page.waitForFunction(() => {
          const list = document.getElementById("tasks");
          return list.scrollTop > 0 && list.scrollTop + list.clientHeight === list.scrollHeight;
        });
        assert.deepEqual(await capture("list-end", [stickyPoint]), [[[171, 203, 238]], [[171, 203, 238]]]);
        const checkbox = page.locator(".task-box").last();
        const box = await checkbox.boundingBox();
        await click(Math.round(box.x + box.width / 2), Math.round(box.y + box.height / 2));
        assert.equal(await checkbox.isChecked(), true);
        assert.equal(await page.locator("#count").textContent(), "9 tasks left");
        assert.deepEqual(await capture("task-checked", [stickyPoint]), [[green], [green]]);

        await wheel(80);
        const contained = [138, 200, 191];
        assert.deepEqual(await capture("contained", [indicator]), [[contained], [contained]], "Contained list must not scroll its parent");
        await click(width - 36, 12);
        await wheel(80);
        assert.deepEqual(await capture("no-chain", [indicator]), [[contained], [contained]], "An overscroll value of none also blocks chaining");
        await click(width - 36, 12);
        await wheel(80);
        await page.waitForFunction(() => scrollY > 0);
        const chained = [238, 153, 85];
        assert.deepEqual(await capture("chained", [indicator]), [[chained], [chained]], "An auto list chains to the page while the top control stays fixed");
        await request("close");
      } finally {
        child.stdin.end();
        child.kill();
        await finished;
        await page.close();
      }
    }
    console.log("MiniWeb and Chromium task modal, scroll chaining, sticky/fixed paint and checkbox targeting passed at desktop and mobile widths");
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
};
