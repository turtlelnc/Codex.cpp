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
    await page
      .locator("body")
      .evaluate((n) => n.setAttribute("data-coalesce", ""));
    await page.locator("#fixed").click();
    await page.waitForFunction(
      () => document.body.getAttribute("data-scroll-count") === "2",
    );
    assert.equal(await page.locator("#panel").evaluate((n) => n.scrollTop), 30);
    await page
      .locator("body")
      .evaluate((n) => n.removeAttribute("data-coalesce"));
    await page.locator("#panel").evaluate((n) => n.scrollTo(100, 260));
    await page.waitForFunction(
      () => document.body.getAttribute("data-scroll") === "100:260",
    );
    await page.locator("#panel").evaluate((n) => {
      n.style.overflowX = "hidden";
      n.style.overflowY = "auto";
      n.scrollTo(0, 0);
    });
    await page.mouse.move(50, 150);
    await page.mouse.wheel(50, 50);
    await page.waitForFunction(
      () => document.getElementById("panel").scrollTop === 50,
    );
    assert.equal(
      await page.locator("#panel").evaluate((n) => n.scrollLeft),
      0,
      "Diagonal wheel must not scroll hidden horizontal overflow",
    );
    await page.locator("#panel").evaluate((n) => {
      n.removeAttribute("style");
      n.scrollTo(100, 260);
    });
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
    await page.mouse.move(305, 15);
    await page.mouse.wheel(0, 100);
    await page.waitForFunction(
      () => document.body.getAttribute("data-wheel") === "canceled",
    );
    assert.equal(await page.evaluate(() => scrollY), 400);
    const deep = spawnSync(
      probe,
      [
        "--geometry",
        path.resolve("miniweb/test/scroll-deep.html"),
        "640",
        "deep",
        "10,10",
      ],
      { encoding: "utf8", timeout: 5000 },
    );
    assert.equal(deep.status, 0, deep.stderr + " " + deep.error);
    assert.equal(
      JSON.parse(deep.stdout)._measure,
      "clicked",
      "Deep sticky render and hit testing finish within bounded time",
    );
    await page.locator("#panel").evaluate((n) => n.scrollTo(100, 520));
    await page.waitForFunction(
      () => document.body.getAttribute("data-scroll") === "100:520",
    );
    await page.evaluate(() => {
      document.getElementById("target").remove();
      document.getElementById("content").style.height = "100px";
    });
    await page.waitForFunction(
      () => document.body.getAttribute("data-scroll") === "100:0",
    );
    await page.evaluate(() => (document.body.style.height = "500px"));
    await page.waitForFunction(
      () => document.body.getAttribute("data-window-scroll") === "20",
    );
    await page.locator("#panel").evaluate((n) => (n.style.display = "none"));
    assert.deepEqual(
      await page
        .locator("#panel")
        .evaluate((n) => [n.clientHeight, n.scrollHeight, n.scrollTop]),
      [0, 0, 0],
    );
    assert.deepEqual(
      await page.evaluate(() => {
        const n = document.getElementById("panel");
        n.remove();
        return [n.clientHeight, n.scrollHeight, n.scrollTop];
      }),
      [0, 0, 0],
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
