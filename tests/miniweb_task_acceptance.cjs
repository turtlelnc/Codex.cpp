const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const {createInterface} = require('node:readline');

function taskPage() {
  const base = fs.readFileSync('miniweb/test/live-task.html', 'utf8');
  return base.replace('<ul class="tasks"', `
<style>
  #tasks { height: 180px; overflow: auto; }
  #confirm { width: 250px; padding: 16px; border: 0; background: #fff; }
  #confirm::backdrop { background-color: rgba(0, 0, 0, .4); }
</style>
<button id="clear-completed" type="button">Clear completed</button>
<dialog id="confirm" aria-labelledby="confirm-title">
  <h2 id="confirm-title">Clear completed tasks?</h2>
  <button id="cancel-clear" type="button" autofocus>Cancel</button>
  <button id="confirm-clear" type="button">Clear</button>
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
      async function capture(stage) {
        const response = await act('screenshot');
        const mini = Buffer.from(response.images[0].split(',')[1], 'base64');
        const chrome = await page.screenshot();
        if (artifacts) {
          fs.mkdirSync(artifacts, {recursive: true});
          for (const [engine, image] of [['miniweb', mini], ['chromium', chrome]])
            fs.writeFileSync(path.join(artifacts, `task-acceptance-${width}-${stage}-${engine}.png`), image);
        }
        return page.evaluate(async sources => Promise.all(sources.map(async source => {
          const image = new Image();
          image.src = 'data:image/png;base64,' + source;
          await image.decode();
          const canvas = document.createElement('canvas');
          canvas.width = image.width;
          canvas.height = image.height;
          const context = canvas.getContext('2d');
          context.drawImage(image, 0, 0);
          return Array.from(context.getImageData(2, 2, 1, 1).data).slice(0, 3);
        })), [mini, chrome].map(buffer => buffer.toString('base64')));
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
        const dimmed = await capture('dialog');
        for (let engine = 0; engine < dimmed.length; ++engine)
          assert(dimmed[engine].every((value, channel) => Math.abs(value - .6 * before[engine][channel]) <= 1));
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
