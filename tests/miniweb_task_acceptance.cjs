const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const {createInterface} = require('node:readline');

function taskPage({clipped = false} = {}) {
  const base = fs.readFileSync('miniweb/test/live-task.html', 'utf8');
  return base.replace('<ul class="tasks"', `
<style>
  #tasks { height: 180px; overflow: auto; }
  #confirm { width: 250px; padding: 16px; border: 0; background: #fff; }
  #confirm::backdrop { background-color: rgba(0, 0, 0, .4); }
  #confirm-clear { color: #000; background: #fff; border: 0; padding: 8px; }
  #confirm-clear span { display: block; white-space: nowrap; }
  ${clipped ? '@media (max-width: 460px) { #confirm-clear { width: 60px; } #confirm-clear span { overflow: hidden; } }' : ''}
</style>
<button id="clear-completed" type="button">Clear completed</button>
<dialog id="confirm" aria-labelledby="confirm-title">
  <h2 id="confirm-title">Clear completed tasks?</h2>
  <button id="cancel-clear" type="button" autofocus>Cancel</button>
  <button id="confirm-clear" type="button"><span>Clear completed tasks</span></button>
</dialog>
<ul class="tasks"`).replace('})();\n</script>', `
const dialog = document.getElementById('confirm');
for (let n = 1; n <= 12; n++) {
  input.value = 'Sample task ' + n;
  form.dispatchEvent(new Event('submit', {cancelable: true}));
}
input.blur();
form.addEventListener('submit', () => {
  const checkbox = list.lastElementChild.querySelector('input');
  if (checkbox.getAttribute('aria-label').includes('Visual acceptance')) checkbox.id = 'acceptance-task';
});
document.getElementById('clear-completed').addEventListener('click', () => dialog.showModal());
document.getElementById('cancel-clear').addEventListener('click', () => dialog.close());
document.getElementById('confirm-clear').addEventListener('click', () => {
  for (let n = items.length - 1; n >= 0; n--) {
    if (!items[n].task.done) continue;
    items[n].li.remove();
    items.splice(n, 1);
    tasks.splice(n, 1);
  }
  applyFilter();
  dialog.close();
});
})();
</script>`);
}

module.exports = async function taskAcceptance(browser, binary) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'miniweb-task-acceptance-'));
  const html = taskPage();
  fs.writeFileSync(path.join(root, 'task.html'), html);
  const clipped = taskPage({clipped: true});
  fs.writeFileSync(path.join(root, 'clipped.html'), clipped);
  try {
    for (const [width, height] of [[390, 600], [900, 700]]) {
      const page = await browser.newPage({viewport: {width, height}});
      const child = spawn(binary, ['preview', 'session'], {
        cwd: root, timeout: 30000,
        env: {...process.env, CODEX_CPP_HOME: path.join(root, 'profile')},
      });
      const finished = once(child, 'close');
      const lines = createInterface({input: child.stdout})[Symbol.asyncIterator]();
      const trace = [];
      let session, stderr = '';
      child.stderr.on('data', chunk => { stderr += chunk; });
      async function act(operation, fields = {}) {
        trace.push({operation, ...fields});
        child.stdin.write(JSON.stringify({operation, session, ...fields}) + '\n');
        const line = await lines.next();
        assert(!line.done, stderr || 'Preview exited');
        const response = JSON.parse(line.value);
        assert.equal(response.exit_code, 0, response.output);
        return response;
      }
      const inspect = async value => JSON.parse((await act('inspect', {value})).output);
      async function click(id) {
        const {element} = await inspect(id);
        assert(element?.center_hit, `MiniWeb control is unreachable: ${id}`);
        const [x, y, w, h] = element.rect;
        await act('click', {x: Math.floor(x + w / 2), y: Math.floor(y + h / 2)});
        await page.locator('#' + id).click();
      }
      const artifacts = process.env.CODEX_ARTIFACT_DIR;
      async function capture(stage, buttons = []) {
        const response = await act('screenshot');
        const mini = Buffer.from(response.images[0].split(',')[1], 'base64');
        const chrome = await page.screenshot();
        if (artifacts) {
          fs.mkdirSync(artifacts, {recursive: true});
          for (const [engine, image] of [['miniweb', mini], ['chromium', chrome]])
            fs.writeFileSync(path.join(artifacts, `task-acceptance-${width}-${stage}-${engine}.png`), image);
        }
        return page.evaluate(async ({sources, buttons}) => Promise.all(sources.map(async (source, engine) => {
          const image = new Image();
          image.src = 'data:image/png;base64,' + source;
          await image.decode();
          const canvas = document.createElement('canvas');
          canvas.width = image.width;
          canvas.height = image.height;
          const context = canvas.getContext('2d');
          context.drawImage(image, 0, 0);
          const shade = Array.from(context.getImageData(2, 2, 1, 1).data).slice(0, 3);
          const button = buttons[engine];
          let ink = 0;
          if (button) {
            const [x, y, w, h] = button.map(Math.floor);
            const pixels = context.getImageData(x + 3, y + 3, w - 6, h - 6).data;
            for (let i = 0; i < pixels.length; i += 4) {
              if (pixels[i] < 100 && pixels[i + 1] < 100 && pixels[i + 2] < 100) ink++;
            }
          }
          return {shade, ink};
        })), {sources: [mini, chrome].map(buffer => buffer.toString('base64')), buttons});
      }
      async function readLabel(stage) {
        const {element} = await inspect('confirm-clear');
        assert.equal(element.text, 'Clear completed tasks');
        assert(element.center_hit, 'Element inspection still sees a clickable button');
        const button = page.locator('#confirm-clear');
        assert.equal(await button.innerText(), 'Clear completed tasks');
        const {x, y, width, height} = await button.boundingBox();
        const overflow = await button.locator('span').evaluate(span => span.scrollWidth > span.clientWidth);
        const shots = await capture(stage, [element.rect, [x, y, width, height]]);
        return {shots, overflow};
      }
      try {
        const opened = await act('open', {path: 'task.html', width, height, wait_ms: 0});
        session = opened.output.match(/Opened preview session ([^. ]+)/)?.[1];
        assert(session, opened.output);
        await page.setContent(html);
        await click('new-task');
        await act('type', {value: 'Visual acceptance'});
        await act('key', {value: 'Enter'});
        await page.locator('#new-task').fill('Visual acceptance');
        await page.locator('#new-task').press('Enter');
        assert.equal((await inspect('count')).element.text, '15 tasks left');
        const {element: list} = await inspect('tasks');
        const [x, y, w, h] = list.rect;
        const wheelX = Math.floor(x + w / 2), wheelY = Math.floor(y + h / 2);
        await act('scroll', {x: wheelX, y: wheelY, delta_y: 1500});
        await page.locator('#tasks').evaluate(node => { node.scrollTop = node.scrollHeight; });
        assert((await inspect('tasks')).element.scroll_y > 0);
        assert.equal(await page.locator('#count').innerText(), '15 tasks left');
        await click('acceptance-task');
        assert.equal((await inspect('acceptance-task')).element.checked, true);
        const before = await capture('before-dialog');
        await click('clear-completed');
        assert.equal((await inspect('cancel-clear')).focused_id, 'cancel-clear');
        assert.equal(await page.evaluate(() => document.activeElement.id), 'cancel-clear');
        assert.equal((await inspect('clear-completed')).element.center_hit, false);
        const fullLabel = await readLabel('dialog');
        assert.equal(fullLabel.overflow, false);
        for (let engine = 0; engine < fullLabel.shots.length; ++engine)
          assert(fullLabel.shots[engine].shade.every((value, channel) =>
            Math.abs(value - .6 * before[engine].shade[channel]) <= 1));
        await click('cancel-clear');
        assert.equal((await inspect('acceptance-task')).element.checked, true);
        assert.equal(await page.locator('#acceptance-task').isChecked(), true);
        assert.equal((await inspect('clear-completed')).focused_id, 'clear-completed');
        assert.equal(await page.evaluate(() => document.activeElement.id), 'clear-completed');
        await click('clear-completed');
        await click('confirm-clear');
        assert.equal((await inspect('acceptance-task')).element, null);
        assert.equal(await page.locator('#acceptance-task').count(), 0);
        assert.equal((await inspect('count')).element.text, '14 tasks left');
        assert.equal(await page.locator('#count').innerText(), '14 tasks left');
        assert.equal(await page.locator('#tasks li').count(), 14);
        assert.equal((await inspect('clear-completed')).focused_id, 'clear-completed');
        await capture('cleared');
        await act('close');
        if (width === 390) {
          session = undefined;
          const reopened = await act('open', {path: 'clipped.html', width, height, wait_ms: 0});
          session = reopened.output.match(/Opened preview session ([^. ]+)/)?.[1];
          assert(session, reopened.output);
          await page.setContent(clipped);
          await click('clear-completed');
          const clippedLabel = await readLabel('clipped');
          assert.equal(clippedLabel.overflow, true);
          for (let engine = 0; engine < clippedLabel.shots.length; ++engine) {
            const ink = clippedLabel.shots[engine].ink;
            assert(ink > 0 && ink < fullLabel.shots[engine].ink * 0.65,
              `Clipped button should show less label ink, engine ${engine}`);
          }
          await act('close');
        }
      } catch (error) {
        if (artifacts && session) {
          await capture('failure').catch(() => {});
        }
        throw error;
      } finally {
        child.stdin.end();
        child.kill();
        await finished;
        await page.close();
        if (artifacts) {
          fs.mkdirSync(artifacts, {recursive: true});
          fs.writeFileSync(path.join(artifacts, `task-acceptance-${width}-trace.json`), JSON.stringify(trace, null, 2));
        }
      }
    }
    console.log('MiniWeb and Chromium task acceptance passed at 390×600 and 900×700');
  } finally {
    fs.rmSync(root, {recursive: true, force: true});
  }
};
module.exports.taskPage = taskPage;
