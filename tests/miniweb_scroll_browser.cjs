const fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict"),
  { spawnSync } = require("child_process");
module.exports = async function (browser, probe) {
  const page = await browser.newPage({ viewport: { width: 640, height: 480 } });
  try {
    await page.setContent(fs.readFileSync("miniweb/test/scroll.html", "utf8"));
    await page.locator("#panel").evaluate((n) => n.scrollTo(100, 260));
    await page.waitForFunction(
      () => document.body.getAttribute("data-scroll") === "100:260",
    );
    const actual = await page.evaluate(() => {
      const p = document.getElementById("panel"),
        r = document.getElementById("target").getBoundingClientRect(),
        s = document.getElementById("sticky").getBoundingClientRect();
      return {
        target: [r.x, r.y],
        sticky: [s.x, s.y],
        extent: [p.scrollWidth, p.scrollHeight],
        window: 400,
      };
    });
    await page.mouse.click(45, 105);
    assert.equal(
      await page.locator("body").getAttribute("data-click"),
      "target",
    );
    await page.evaluate(() => scrollTo(0, 400));
    await page.waitForFunction(() => scrollY === 400);
    assert.equal(
      await page.locator("#fixed").evaluate((n) => n.getBoundingClientRect().y),
      10,
    );
    await page.mouse.click(305, 15);
    assert.equal(
      await page.locator("body").getAttribute("data-click"),
      "fixed",
    );
    const result = spawnSync(
      probe,
      ["--scroll", path.resolve("miniweb/test/scroll.html")],
      { encoding: "utf8" },
    );
    assert.equal(result.status, 0, result.stderr);
    const native = JSON.parse(result.stdout.match(/SCROLL_RESULT (.*)/)[1]);
    assert.deepEqual(native, actual);
    console.log(
      "MiniWeb/Chromium scrolling geometry, sticky, fixed and click comparison passed",
    );
  } finally {
    await page.close();
  }
};
