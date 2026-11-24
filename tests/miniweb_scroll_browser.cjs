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
    await page.setContent(
      fs.readFileSync("miniweb/test/scroll-clip-sticky.html", "utf8"),
    );
    await page.evaluate(() => scrollTo(0, 200));
    await page.waitForFunction(() => scrollY === 200);
    assert.equal(
      await page
        .locator("#header")
        .evaluate((n) => n.getBoundingClientRect().y),
      0,
    );
    await page.mouse.click(10, 10);
    assert.equal(
      await page.locator("body").getAttribute("data-click"),
      "sticky",
    );
    await page.setContent(
      fs.readFileSync("miniweb/test/scroll-sticky-flow.html", "utf8"),
    );
    await page.evaluate(() => scrollTo(0, 0));
    for (const [offset, top] of [
      [0, 300],
      [200, 100],
      [300, 0],
      [450, 0],
    ]) {
      await page
        .locator("#panel")
        .evaluate((n, y) => (n.scrollTop = y), offset);
      assert.equal(
        await page
          .locator("#late")
          .evaluate((n) => n.getBoundingClientRect().y),
        top,
      );
    }
    await page.mouse.click(5, 5);
    assert.equal(await page.locator("body").getAttribute("data-click"), "late");
    const sidebar = path.resolve("miniweb/test/sidebar-layout.html");
    for (const width of [1280, 390]) {
      await page.setViewportSize({ width, height: 800 });
      await page.setContent(fs.readFileSync(sidebar, "utf8"));
      const expected = await page.evaluate(() => Object.fromEntries(
        Array.from(document.querySelectorAll("[id]"), node => {
          const {x, y, width, height} = node.getBoundingClientRect();
          return [node.id, [x, y, width, height]];
        })
      ));
      const result = spawnSync(probe, [
        "--geometry", sidebar, String(width), Object.keys(expected).join(","),
      ], { encoding: "utf8", timeout: 10000 });
      assert.equal(result.status, 0, result.stderr);
      const actual = JSON.parse(result.stdout);
      for (const [id, rect] of Object.entries(expected)) {
        assert.deepEqual(actual[id], rect, `${width}px sidebar fixture: ${id}`);
      }
      await page.mouse.move(20, 220);
      await page.mouse.wheel(0, 100);
      await page.waitForFunction(() => document.getElementById("list").scrollTop === 100);
      assert.equal(await page.locator("#footer").evaluate(n => n.getBoundingClientRect().y), 430);
    }
    const disclosure = path.resolve("miniweb/test/details.html");
    await page.setContent(fs.readFileSync(disclosure, "utf8"));
    const nativeDetails = (clicks) => {
      const result = spawnSync(probe, [
        "--geometry", disclosure, "390", "files,content,footer,fallback,fallback-content", clicks,
      ], { encoding: "utf8", timeout: 10000 });
      assert.equal(result.status, 0, result.stderr);
      return JSON.parse(result.stdout);
    };
    const footerTop = () => page.locator("#footer").evaluate(n => n.getBoundingClientRect().top);
    assert.equal(nativeDetails("").footer[1], await footerTop());
    assert.equal(nativeDetails("").content[3], 0);
    assert.equal(await page.locator("#file").isVisible(), false);
    await page.keyboard.press("Tab");
    assert.equal(await page.evaluate(() => document.activeElement.id), "summary");
    await page.keyboard.press("Enter");
    await page.waitForFunction(() => document.body.dataset.toggle === "closed:open");
    assert.equal(await page.locator("#file").isVisible(), true);
    assert.equal(nativeDetails("12,10").content[3], 80);
    await page.keyboard.press("Space");
    assert.equal(await page.locator("#file").isVisible(), false);
    assert.equal(nativeDetails("12,10;12,10").footer[1], await footerTop());
    const fallback = nativeDetails("");
    assert(fallback.fallback[3] > 20);
    assert.equal(fallback["fallback-content"][3], 0);
    const fallbackClick = `12,${fallback.fallback[1] + 5}`;
    assert.equal(nativeDetails(fallbackClick)["fallback-content"][3], 60);
    await page.locator("#footer").focus();
    await page.keyboard.press("Tab");
    await page.keyboard.press("Enter");
    assert.equal(await page.locator("#fallback-file").isVisible(), true);
    await page.keyboard.press("Space");
    assert.equal(await page.locator("#fallback-file").isVisible(), false);
    console.log(
      "MiniWeb/Chromium scrolling geometry, sticky, fixed and click comparison passed",
    );
  } finally {
    await page.close();
  }
};
