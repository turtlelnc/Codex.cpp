const fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict"),
  { spawnSync } = require("child_process");
module.exports = async function (browser, probe) {
  const native = spawnSync(
    probe,
    ["--observers", path.resolve("miniweb/test/observers.html")],
    { encoding: "utf8", timeout: 30000 },
  );
  assert.equal(native.status, 0, native.stderr);
  const expected = JSON.parse(native.stdout.match(/OBSERVERS_RESULT (.*)/)[1]);
  const page = await browser.newPage({ viewport: { width: 640, height: 480 } });
  page.on("pageerror", () => {});
  try {
    await page.setContent(
      fs.readFileSync("miniweb/test/observers.html", "utf8"),
    );
    await page.waitForFunction(() => resizeLog.length === 1);
    await page.locator("#mutate").click();
    await page.waitForFunction(() => order.includes("promise"));
    await page.locator("#resize").click();
    await page.waitForFunction(() => resizeLog.length === 2);
    await page.locator("#hide").click();
    await page.waitForFunction(() => resizeLog.length === 3);
    await page.locator("#show").click();
    await page.waitForFunction(() => resizeLog.length === 4);
    await page.locator("#disconnect").click();
    assert.deepEqual(
      await page.evaluate(() =>
        JSON.parse(document.body.getAttribute("data-measure")),
      ),
      expected,
    );
    await page.setContent(
      fs.readFileSync("miniweb/test/observer-records.html", "utf8"),
    );
    await page.locator("#run").click();
    await page.waitForFunction(
      () =>
        document.body.hasAttribute("data-result") ||
        document.body.hasAttribute("data-error"),
    );
    assert.equal(await page.locator("body").getAttribute("data-error"), null);
    const records = JSON.parse(
      await page.locator("body").getAttribute("data-result"),
    );
    const nativeRecords = spawnSync(
      probe,
      [
        "--observer-records",
        path.resolve("miniweb/test/observer-records.html"),
      ],
      { encoding: "utf8", timeout: 30000 },
    );
    assert.equal(nativeRecords.status, 0, nativeRecords.stderr);
    assert.deepEqual(
      JSON.parse(nativeRecords.stdout.match(/RECORDS_RESULT (.*)/)[1]),
      records,
    );
    await page.setContent(
      fs.readFileSync("miniweb/test/observer-resize.html", "utf8"),
    );
    await page.waitForFunction(() => log.length === 2);
    await page.locator("#border").click();
    await page.waitForFunction(() => log.length === 3);
    await page.locator("#content").click();
    await page.waitForFunction(() => log.length === 5);
    await page.locator("#unobserve").click();
    await page.locator("#exception").click();
    await page.waitForFunction(() => following === 1);
    await page.locator("#disconnect").click();
    const resize = spawnSync(
      probe,
      ["--observer-resize", path.resolve("miniweb/test/observer-resize.html")],
      { encoding: "utf8", timeout: 30000 },
    );
    assert.equal(resize.status, 0, resize.stderr);
    assert.deepEqual(
      JSON.parse(resize.stdout.match(/RESIZE_RESULT (.*)/)[1]),
      JSON.parse(await page.locator("body").getAttribute("data-result")),
    );
    console.log(
      "MiniWeb/Chromium MutationObserver records, microtasks and ResizeObserver layout acceptance passed",
    );
  } finally {
    await page.close();
  }
};
