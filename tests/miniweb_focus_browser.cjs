const fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict"),
  { spawnSync } = require("child_process");
module.exports = async function (browser, probe) {
  const run = spawnSync(
    probe,
    ["--reveal", path.resolve("miniweb/test/focus-reveal.html")],
    { encoding: "utf8", timeout: 30000 },
  );
  assert.equal(run.status, 0, run.stderr);
  const native = JSON.parse(run.stdout.match(/REVEAL_RESULT (.*)/)[1]);
  const page = await browser.newPage({ viewport: { width: 640, height: 480 } });
  try {
    await page.setContent(
      fs.readFileSync("miniweb/test/focus-reveal.html", "utf8"),
    );
    const state = () =>
      page.evaluate(() =>
        JSON.parse(document.body.getAttribute("data-measure")),
      );
    const check = async (px, py, wy, y) => {
      const s = await state();
      for (const [name, expected] of Object.entries({ px, py, wy, y }))
        assert(
          Math.abs(s[name] - expected) < 1,
          name + ": " + JSON.stringify(s),
        );
    };
    await page.keyboard.press("Tab");
    assert.equal((await state()).active, "far");
    await check(20, 260, 410, 230);
    await page.screenshot({ path: "/tmp/miniweb-focus-chromium-form.png" });
    await page.keyboard.insertText("task");
    await page.keyboard.press("Enter");
    assert.equal((await state()).submitted, 1);
    assert.equal((await state()).value, "task");
    await page.locator("#formreset").click();
    assert.equal((await state()).value, "seed");
    for (const [command, expected] of [
      ["prevent", [0, 0, 0, 900]],
      ["start", [20, 300, 600, 0]],
      ["end", [20, 220, 220, 460]],
      ["nearest", [20, 220, 220, 460]],
      ["center", [70, 260, 410, 230]],
      ["container", [20, 300, 0, 600]],
    ]) {
      await page.locator("#reset").click();
      await page.locator("#" + command).click();
      await check(...expected);
    }
    await page.locator("#reset").click();
    await page.locator("#hidden").click();
    assert.equal((await state()).wy, 0);
    assert.equal((await state()).py, 0);
    await page.locator("#reset").click();
    await page.locator("#detached").click();
    assert.equal((await state()).wy, 0);
    assert.equal((await state()).py, 0);
    await page.locator("#invalid").click();
    assert.equal(await page.locator("body").getAttribute("data-invalid"), null);
    assert.equal((await state()).wy, 0);
    await page.locator("#reset").click();
    await page.locator("#center").click();
    await page.locator("#settings").click();
    let s = await state();
    assert.equal(s.wy, 410);
    assert.equal(s.dy, 240);
    assert.equal(s.active, "dialog-input");
    await page.screenshot({ path: "/tmp/miniweb-focus-chromium-settings.png" });
    await page.keyboard.insertText("!");
    await page.keyboard.press("Escape");
    assert.deepEqual(await state(), native);
    console.log(
      "MiniWeb/Chromium form, settings and long-page reveal acceptance passed",
    );
  } finally {
    await page.close();
  }
};
