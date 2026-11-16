const fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict"),
  { spawnSync } = require("child_process");
module.exports = async function interactionBrowser(browser, probe) {
  const run = spawnSync(
    probe,
    ["--interaction", path.resolve("miniweb/test/interaction.html")],
    { encoding: "utf8", timeout: 30000 },
  );
  assert.equal(run.status, 0, run.stdout + run.stderr);
  const line = run.stdout
    .split("\n")
    .find((l) => l.startsWith("INTERACTION_RESULT "));
  assert(line);
  const native = JSON.parse(line.slice("INTERACTION_RESULT ".length));
  const page = await browser.newPage({ viewport: { width: 640, height: 480 } });
  try {
    await page.setContent(
      fs.readFileSync("miniweb/test/interaction.html", "utf8"),
    );
    for (const [op, value] of [
      ["press", "Tab"],
      ["insertText", "你好"],
      ["press", "Home"],
      ["insertText", "A"],
      ["press", "End"],
      ["press", "Backspace"],
      ["press", "Tab"],
      ["insertText", "!"],
      ["press", "Enter"],
      ["insertText", "line2"],
      ["press", "Tab"],
      ["press", "Space"],
      ["press", "Tab"],
      ["press", "Enter"],
    ])
      await page.keyboard[op](value);
    const chrome = JSON.parse(
      await page.locator("body").getAttribute("data-result"),
    );
    for (const key of [
      "name",
      "attr",
      "note",
      "checked",
      "submitted",
      "visits",
      "active",
    ])
      assert.deepEqual(native[key], chrome[key], key);
    for (const event of [
      "focus:name",
      "input:name",
      "change:name",
      "blur:name",
      "focus:note",
      "input:note",
      "change:note",
      "input:agree",
      "change:agree",
    ]) {
      assert(native.events.includes(event), "Native missing " + event);
      assert(chrome.events.includes(event), "Chromium missing " + event);
    }
    assert(!native.events.includes("focus:disabled"));
    await page.locator("#open").click();
    await page.keyboard.insertText("!");
    await page.keyboard.insertText("blocked");
    await page.keyboard.press("Escape");
    assert.equal(
      await page.locator("body").getAttribute("data-setting"),
      "!warm",
    );
    await page.locator("#open").click();
    await page.keyboard.press("Tab");
    await page.keyboard.insertText("mutate");
    assert.equal(await page.locator("#readonly").inputValue(), "locked");
    await page.keyboard.press("Tab");
    await page.keyboard.press("Enter");
    await page.locator("#name").click();
    await page.keyboard.insertText("tail");
    await page.locator("#stayfocus").click();
    await page.keyboard.insertText("x");
    const previous = await page.locator("#name").inputValue();
    await page.keyboard.press("Space");
    assert.equal(await page.locator("#name").inputValue(), previous + " ");
    await page.locator("#plain").click();
    assert.equal(
      await page.locator("body").getAttribute("data-committed"),
      previous + " ",
    );
    assert.equal(
      await page.evaluate(() => document.activeElement.tagName),
      "BODY",
    );
    await page.locator("#actionButton").focus();
    await page.keyboard.press("Enter");
    assert.equal(
      await page.locator("body").getAttribute("data-action"),
      "clicked",
    );
    assert.equal(
      await page.locator("body").getAttribute("data-action-count"),
      "1",
    );
    await page.locator("#actionSubmit").focus();
    await page.keyboard.press("Enter");
    assert.equal(
      await page.locator("body").getAttribute("data-submit-click"),
      "canceled",
    );
    assert.equal(
      await page.locator("body").getAttribute("data-submit-count"),
      "1",
    );
    assert.equal(
      await page.locator("body").getAttribute("data-wrong-submit"),
      null,
    );
    await page.locator("#implicitText").focus();
    await page.keyboard.press("Enter");
    assert.equal(
      await page.locator("body").getAttribute("data-default-clicks"),
      "1",
    );
    assert.equal(
      await page.locator("body").getAttribute("data-implicit-wrong"),
      null,
    );
    await page.locator("#disableDefault").click();
    await page.locator("#implicitText").focus();
    await page.keyboard.press("Enter");
    assert.equal(
      await page.locator("body").getAttribute("data-default-clicks"),
      "1",
    );
    assert.equal(
      await page.locator("body").getAttribute("data-implicit-wrong"),
      null,
    );
    await page.locator("#checkLabel").click();
    assert.equal(await page.locator("#labelCheck").isChecked(), true);
    await page.locator("#cancelLabel").click();
    assert.equal(await page.locator("#labelCheck").isChecked(), true);
    await page.locator("#textLabel").click();
    await page.keyboard.insertText("label focus");
    assert.equal(await page.locator("#labelText").inputValue(), "label focus");
    await page.locator("#nestedLabel").click();
    assert.equal(await page.locator("#nestedCheck").isChecked(), true);
    await page.locator("#programmatic").click();
    assert.equal(
      await page.locator("body").getAttribute("data-program-changes"),
      "0",
    );
    await page.locator("#unicode").click();
    await page.keyboard.press("ArrowLeft");
    await page.keyboard.insertText("X");
    assert.equal(await page.locator("#labelText").inputValue(), "X😀");
    await page.keyboard.press("Home");
    await page.keyboard.press("ArrowRight");
    await page.keyboard.press("ArrowRight");
    await page.keyboard.insertText("Y");
    assert.equal(await page.locator("#labelText").inputValue(), "X😀Y");
    await page.keyboard.press("Escape");
    await page.locator("#plain").click();
    assert.equal(
      await page.locator("body").getAttribute("data-program-changes"),
      "1",
    );
    await page.locator("#unicode").click();
    await page.keyboard.insertText("X");
    await page.keyboard.press("Backspace");
    await page.locator("#plain").click();
    assert.equal(
      await page.locator("body").getAttribute("data-program-changes"),
      "0",
    );
    await page.screenshot({ path: "/tmp/miniweb-interaction-chromium.png" });
    console.log(
      "MiniWeb/Chromium form and settings interaction comparison passed",
    );
  } finally {
    await page.close();
  }
};
