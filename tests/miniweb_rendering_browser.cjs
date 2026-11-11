const assert = require('assert/strict');
const fs = require('fs'),
  path = require('path');

// Decode both screenshots through Chromium's PNG decoder. No extra image or
// browser dependency is added to the application or the CI test installation.
module.exports = async function compareTextVisibility(page, fixture, miniPNG) {
  await page.setViewportSize({ width: 640, height: 480 });
  await page.setContent(fixture);
  await page.evaluate(() => document.fonts.ready);
  const chromePNG = await page.screenshot();
  if (process.env.CODEX_ARTIFACT_DIR) {
    fs.mkdirSync(process.env.CODEX_ARTIFACT_DIR, { recursive: true });
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'miniweb-text.png'),
      miniPNG,
    );
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'chromium-text.png'),
      chromePNG,
    );
  }
  const metrics = await page.evaluate(
    async (screenshots) => {
      const areas = [
        [0, 0, 200, 80],
        [0, 100, 300, 80],
        [320, 0, 160, 140],
        [500, 0, 80, 100],
      ];
      const results = [];
      for (const data of screenshots) {
        const image = new Image();
        image.src = 'data:image/png;base64,' + data;
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = 640;
        canvas.height = 480;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(image, 0, 0);
        const pixels = ctx.getImageData(0, 0, 640, 480).data;
        const boxes = areas.map(([x, y, w, h]) => {
          let left = 640,
            right = -1,
            top = 480,
            bottom = -1,
            count = 0;
          for (let row = y; row < y + h; row++)
            for (let col = x; col < x + w; col++) {
              const i = (row * 640 + col) * 4;
              if (
                pixels[i] < 100 &&
                pixels[i + 1] < 100 &&
                pixels[i + 2] < 100
              ) {
                count++;
                left = Math.min(left, col);
                right = Math.max(right, col);
                top = Math.min(top, row);
                bottom = Math.max(bottom, row);
              }
            }
          return {
            count,
            x: (left + right) / 2,
            y: (top + bottom) / 2,
            width: right - left + 1,
            height: bottom - top + 1,
          };
        });
        const samples = [
          [0, 0],
          [50, 280],
          [15, 350],
          [130, 280],
          [420, 280],
          [260, 260],
        ].map(([x, y]) =>
          Array.from(pixels.slice((y * 640 + x) * 4, (y * 640 + x) * 4 + 3)),
        );
        results.push({ boxes, samples });
      }
      return results;
    },
    [miniPNG.toString('base64'), chromePNG.toString('base64')],
  );
  for (let i = 0; i < 4; i++) {
    const mini = metrics[0].boxes[i],
      chrome = metrics[1].boxes[i];
    assert(
      mini.count > 30 && chrome.count > 30,
      'Missing flex text in region ' + i,
    );
    assert(
      Math.abs(mini.width - chrome.width) < 4 &&
        Math.abs(mini.height - chrome.height) < 4,
      'CSS font size differs: ' + JSON.stringify({ i, mini, chrome }),
    );
    // Compare geometry, allowing glyph rasterization differences.
    assert(
      Math.abs(mini.x - chrome.x) < 5 && Math.abs(mini.y - chrome.y) < 5,
      'Flex text alignment differs: ' + JSON.stringify({ i, mini, chrome }),
    );
  }
  assert.deepEqual(
    metrics[0].samples,
    metrics[1].samples,
    'Visibility differs from Chromium',
  );
  await page.locator('#label').click();
  assert.equal(
    await page.locator('body').getAttribute('data-label-click'),
    'yes',
  );
  await page.locator('#visible').click();
  assert.equal(
    await page.locator('body').getAttribute('data-visible-click'),
    'yes',
  );
  await page.mouse.click(15, 350);
  assert.equal(
    await page.locator('body').getAttribute('data-hidden-click'),
    null,
  );
};

module.exports.borders = async function compareBorders(page, fixture, miniPNG) {
  await page.setViewportSize({ width: 640, height: 240 });
  await page.setContent(fixture);
  const chromePNG = await page.screenshot();
  if (process.env.CODEX_ARTIFACT_DIR) {
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'miniweb-borders.png'),
      miniPNG,
    );
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'chromium-borders.png'),
      chromePNG,
    );
  }
  const samples = await page.evaluate(
    async (images) => {
      const result = [];
      for (const data of images) {
        const image = new Image();
        image.src = 'data:image/png;base64,' + data;
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = 640;
        canvas.height = 240;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(image, 0, 0);
        result.push(
          [
            [10, 10],
            [70, 10],
            [130, 10],
            [190, 10],
            [260, 10],
            [330, 10],
            [400, 10],
            [460, 10],
            [530, 20],
            [10, 80],
            [70, 80],
            [130, 80],
            [330, 80],
            [400, 80],
            [460, 80],
            [530, 80],
            [10, 140],
            [70, 140],
            [130, 140],
          ].map(([x, y]) => Array.from(ctx.getImageData(x, y, 1, 1).data)),
        );
      }
      return result;
    },
    [miniPNG.toString('base64'), chromePNG.toString('base64')],
  );
  assert.deepEqual(
    samples[0],
    samples[1],
    'Border pixels differ from Chromium',
  );
};
