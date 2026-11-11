const assert = require('assert/strict'),
  fs = require('fs'),
  path = require('path');

module.exports = async function compareTables(
  page,
  fixture,
  miniPNG,
  actual,
  narrowActual,
) {
  await page.setViewportSize({ width: 640, height: 800 });
  await page.setContent(fixture);
  await page.evaluate(() => document.fonts.ready);
  const chrome = await page.evaluate(
    (ids) =>
      Object.fromEntries(
        ids.map((id) => {
          const r = document.getElementById(id).getBoundingClientRect();
          return [id, [r.x, r.y, r.width, r.height]];
        }),
      ),
    Object.keys(actual),
  );
  for (const id of Object.keys(chrome))
    for (let i = 0; i < 4; i++)
      assert(
        Math.abs(actual[id][i] - chrome[id][i]) < 1,
        'Table geometry differs: ' +
          JSON.stringify({ id, mini: actual[id], chrome: chrome[id] }),
      );
  const chromePNG = await page.screenshot();
  if (process.env.CODEX_ARTIFACT_DIR) {
    fs.mkdirSync(process.env.CODEX_ARTIFACT_DIR, { recursive: true });
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'miniweb-tables.png'),
      miniPNG,
    );
    fs.writeFileSync(
      path.join(process.env.CODEX_ARTIFACT_DIR, 'chromium-tables.png'),
      chromePNG,
    );
  }
  const metrics = await page.evaluate(
    async ({ images, geometry }) => {
      const output = [];
      for (const bytes of images) {
        const image = new Image();
        image.src = 'data:image/png;base64,' + bytes;
        await image.decode();
        const canvas = document.createElement('canvas');
        canvas.width = 640;
        canvas.height = 800;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(image, 0, 0);
        const pixels = ctx.getImageData(0, 0, 640, 800).data;
        const text = ['a2', 'b2', 'c2', 'resp-d'].map((id) => {
          const [x, y, w, h] = geometry[id];
          let left = 640,
            right = -1,
            top = 800,
            bottom = -1,
            count = 0;
          for (let row = Math.ceil(y + 8); row < y + h - 8; row++)
            for (let col = Math.ceil(x + 8); col < x + w - 8; col++) {
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
          return { count, x: (left + right) / 2, y: (top + bottom) / 2 };
        });
        const colors = [
          [10, 10],
          [13, 13],
          [10, 350],
          [11, 351],
        ].map(([x, y]) =>
          Array.from(pixels.slice((y * 640 + x) * 4, (y * 640 + x) * 4 + 3)),
        );
        output.push({ text, colors });
      }
      return output;
    },
    {
      images: [miniPNG.toString('base64'), chromePNG.toString('base64')],
      geometry: chrome,
    },
  );
  assert.deepEqual(
    metrics[0].colors,
    metrics[1].colors,
    'Table background/border pixels differ',
  );
  for (let i = 0; i < 4; i++) {
    const a = metrics[0].text[i],
      b = metrics[1].text[i];
    assert(a.count > 10 && b.count > 10, 'Cell text is missing');
    assert(
      Math.abs(a.x - b.x) < 3 && Math.abs(a.y - b.y) < 3,
      'Cell vertical/text alignment differs: ' + JSON.stringify({ i, a, b }),
    );
  }
  await page.locator('#add').click();
  assert.equal(await page.locator('#body > tr:not([style])').count(), 3);
  assert(
    await page.evaluate(() => {
      const added = document.getElementById('added').getBoundingClientRect(),
        foot = document.getElementById('footer').getBoundingClientRect();
      return foot.y >= added.bottom;
    }),
  );
  await page.setViewportSize({ width: 320, height: 800 });
  await page.setContent(fixture);
  await page.evaluate(() => document.fonts.ready);
  const narrowChrome = await page.evaluate(
    (ids) =>
      Object.fromEntries(
        ids.map((id) => {
          const r = document.getElementById(id).getBoundingClientRect();
          return [id, [r.x, r.y, r.width, r.height]];
        }),
      ),
    Object.keys(narrowActual),
  );
  for (const id of Object.keys(narrowChrome))
    for (let i = 0; i < 4; i++)
      assert(
        Math.abs(narrowActual[id][i] - narrowChrome[id][i]) < 1,
        'Narrow table geometry differs: ' + id,
      );
};
