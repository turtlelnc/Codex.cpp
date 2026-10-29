'use strict';
(() => {

/* ---------- 基础 ---------- */

const $ = id => document.getElementById(id);
const newId = () => crypto.randomUUID().replaceAll('-', '');
const state = {
  generation: 0, project: 'local', scope: 'shared', user: null, snap: null, polling: false, signature: '', toolSignature: '',
  openTools: new Set(), file: null, panel: 'code', model: '', reasoning: '', permission: 'workspace',
  tree: null, diffSignature: '', fileList: [], expanded: new Set(), provider: null,
};
const editor = {path: null, version: '', crlf: false, editable: true, dirty: false, conflict: false, lines: 0, saving: false};

// 所有非可信文本（模型输出、文件、Diff）只经 textContent 写入。
function el(tag, text, cls) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (cls) node.className = cls;
  return node;
}

function notice(message) {
  $('notice').textContent = message;
  $('notice').hidden = !message;
}

function setConnection(label, ok) {
  $('connection').textContent = label;
  $('connection').dataset.state = ok ? 'ok' : 'error';
}

function onClick(id, fn) {
  $(id).addEventListener('click', async () => {
    try { await fn(); } catch (e) { notice(e.message); }
  });
}

/* ---------- 网络 ---------- */

async function http(path, body) {
  const options = body === undefined ? {} : {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-CSRF-Token': state.user?.csrf || ''},
    body: JSON.stringify(body),
  };
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({}));
  if (response.status === 401) { $('workspace').hidden = true; $('host-login').hidden = false; throw new Error('请登录服务器工作区'); }
  if (!response.ok) throw new Error(data.error || '请求失败');
  return data;
}

async function rpc(op, values = {}) {
  const generation=state.generation;const result=(await http('/api/rpc', {op, project: state.project, scope: state.scope, ...values})).result;if(generation!==state.generation)throw Error('工作区已切换，请重试');return result;
}

/* ---------- 对话框 ---------- */

function field(name, label, type = 'text', value = '', options = [], required = type !== 'password') {
  return {name, label, type, value, options, required};
}

function showDialog(title, body) {
  $('dialog-title').textContent = title;
  $('dialog-submit').hidden=false;$('dialog-cancel').hidden=false;
  $('dialog-body').replaceChildren(body);
  $('dialog-error').textContent = '';
  $('dialog-submit').hidden = true;
  $('dialog-cancel').textContent = '关闭';
  $('dialog-form').onsubmit = event => event.preventDefault();
  if (!$('dialog').open) $('dialog').showModal();
}

function openDialog(title, fields, submit, preview = '') {
  $('dialog-submit').hidden = false;
  $('dialog-cancel').textContent = '取消';
  $('dialog-title').textContent = title;
  $('dialog-submit').hidden=false;$('dialog-cancel').hidden=false;
  $('dialog-body').replaceChildren();
  $('dialog-error').textContent = '';
  for (const f of fields) {
    const label = el('label', f.label);
    const input = el(f.type === 'select' ? 'select' : 'input');
    input.name = f.name;
    if (f.type === 'select') for (const value of f.options) input.append(new Option(value, value));
    else input.type = f.type;
    input.value = f.value;
    input.required = f.required;
    if (f.type === 'password') input.autocomplete = 'new-password';
    label.append(input);
    $('dialog-body').append(label);
  }
  if (preview) $('dialog-body').append(el('pre', preview));
  $('dialog-form').onsubmit = async event => {
    event.preventDefault();
    $('dialog-submit').disabled = true;
    try {
      const next = await submit(Object.fromEntries(new FormData(event.target)));
      for (const input of $('dialog-body').querySelectorAll('input[type=password]')) input.value = '';
      $('dialog').close();
      notice('');
      if (typeof next === 'function') next();
      await refresh();
    } catch (e) {
      $('dialog-error').textContent = e.message;
    } finally {
      $('dialog-submit').disabled = false;
    }
  };
  $('dialog').showModal();
}

onClick('dialog-close', () => $('dialog').close());
onClick('dialog-cancel', () => $('dialog').close());

/* ---------- 代码高亮与 Diff 解析 ---------- */

const KW = new Set('int char bool void auto const static inline struct class return if else for while switch case break continue namespace using include std string size_t nullptr true false new delete public private virtual template typename function let var import export from async await def'.split(' '));
const CODE_EXT = new Set(['c', 'cc', 'cpp', 'h', 'hpp', 'inc', 'mm', 'js', 'ts', 'java', 'rs', 'go']);
const extOf = p => (String(p).split('.').pop() || '').toLowerCase();

// 返回 [{text, cls}] 片段，调用方用 textContent 写入。
function highlight(src, path) {
  if (path && !CODE_EXT.has(extOf(path))) return [{text: src}];
  const re = /(\/\/.*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\])*")|(^\s*#\s*\w+)|(\b\d+\b)|(\b[A-Za-z_]\w*\b)/gm;
  const out = [];
  let last = 0, m;
  while ((m = re.exec(src))) {
    if (m.index > last) out.push({text: src.slice(last, m.index)});
    const cls = m[1] ? 'c' : m[2] ? 's' : m[3] ? 'p' : m[4] ? 'nm' : KW.has(m[0]) ? 'k' : '';
    out.push({text: m[0], cls});
    last = m.index + m[0].length;
  }
  if (last < src.length) out.push({text: src.slice(last)});
  return out;
}

function codeLine(num, prefix, text, path, cls) {
  const row = el('div', undefined, 'ln' + (cls ? ' ' + cls : ''));
  const t = el('span', undefined, 't');
  if (prefix) t.append(prefix);
  for (const part of highlight(text, path)) t.append(part.cls ? el('span', part.text, part.cls) : document.createTextNode(part.text));
  if (!text && !prefix) t.append(' ');
  row.append(el('span', String(num), 'n'), t);
  return row;
}

function parsePatch(patch) {
  const files = [], rows = String(patch).split('\n');
  let cur = null, o = 0, n = 0, inHunk = false;
  for (let i = 0; i < rows.length; i++) {
    const raw = rows[i];
    if (raw.startsWith('diff ')) { inHunk = false; continue; }
    if (!inHunk || (raw.startsWith('--- ') && (rows[i + 1] || '').startsWith('+++ '))) {
      if (raw.startsWith('+++ ')) { cur = {path: raw.slice(4).replace(/^b\//, '').trim(), lines: []}; files.push(cur); inHunk = false; continue; }
      if (raw.startsWith('--- ')) { inHunk = false; continue; }
    }
    const h = /^@@ -(\d+)(?:,\d+)? \+(\d+)/.exec(raw);
    if (h) { o = +h[1]; n = +h[2]; inHunk = true; if (cur) cur.lines.push({t: 'hunk', n: '', s: raw}); continue; }
    if (!cur || !inHunk) continue;
    if (raw.startsWith('+')) cur.lines.push({t: 'add', n: n++, s: raw.slice(1)});
    else if (raw.startsWith('-')) cur.lines.push({t: 'del', n: o++, s: raw.slice(1)});
    else if (raw.startsWith(' ')) { cur.lines.push({t: 'ctx', n, s: raw.slice(1)}); o++; n++; }
  }
  return files;
}

function diffBlock(files, single) {
  const box = el('div', undefined, 'diff-files');
  for (const f of files) {
    if (!single || files.length > 1) box.append(el('div', f.path, 'diff-path'));
    const pre = el('div', undefined, 'code');
    for (const l of f.lines) {
      if (l.t === 'hunk') pre.append(codeLine('', '', l.s, '', 'dl hunk'));
      else pre.append(codeLine(l.n, l.t === 'add' ? '+ ' : l.t === 'del' ? '- ' : '  ', l.s, f.path, 'dl ' + l.t));
    }
    box.append(pre);
  }
  return box;
}

/* ---------- Markdown（DOM 构造，不使用 innerHTML） ---------- */

function inline(parent, text) {
  for (const part of text.split(/(`[^`\n]+`|\*\*[^*\n]+\*\*)/)) {
    if (!part) continue;
    if (part.startsWith('`') && part.endsWith('`') && part.length > 2) parent.append(el('code', part.slice(1, -1)));
    else if (part.startsWith('**') && part.endsWith('**') && part.length > 4) parent.append(el('b', part.slice(2, -2)));
    else parent.append(document.createTextNode(part));
  }
}

function markdown(text) {
  const box = el('div', undefined, 'md');
  text.split(/```[\w+-]*\n?/).forEach((chunk, i) => {
    if (i % 2) { box.append(el('pre', chunk.replace(/\n$/, ''))); return; }
    for (const para of chunk.split(/\n{2,}/)) {
      if (!para.trim()) continue;
      const p = el('p');
      inline(p, para.replace(/^\n+|\n+$/g, ''));
      box.append(p);
    }
  });
  return box;
}

/* ---------- 工具调用 ---------- */

function parseArgs(raw) { try { return JSON.parse(raw || '{}') || {}; } catch { return {}; } }

function toolSummary(name, rawArgs) {
  const a = parseArgs(rawArgs);
  if (name === 'shell') return a.command || '';
  if (name === 'apply_patch') return parsePatch(a.patch || '').map(f => f.path).join(', ') || 'patch';
  const first = a.path ?? a.name ?? Object.values(a)[0];
  return first === undefined ? '' : String(first);
}

function toolDetail(name, rawArgs) {
  const a = parseArgs(rawArgs);
  if (name === 'shell') return '$ ' + (a.command || '');
  if (name === 'write_file') return (a.path || '') + '\n\n' + (a.content || '');
  return rawArgs || '';
}

const exitFailed = output => /^exit_code=(?!0\b)\d+/.test(output || '');

async function decide(allow) {
  for (const b of document.querySelectorAll('[data-decide]')) b.disabled = true;
  try {
    await rpc('approve', {approval: state.snap?.pending?.call_id, allow});
  } catch (e) { notice(e.message); }
  await refresh();
}

function approvalBar(name) {
  const bar = el('div', undefined, 'approve');
  bar.append(el('span', name === 'apply_patch' ? '应用此补丁？' : name === 'shell' ? '运行此命令？' : '允许此操作？'), el('span', undefined, 'sp'));
  const deny = el('button', '拒绝'), allow = el('button', '允许', 'primary');
  deny.dataset.decide = allow.dataset.decide = '1';
  deny.type = allow.type = 'button';
  deny.onclick = () => decide(false);
  allow.onclick = () => decide(true);
  bar.append(deny, allow);
  return bar;
}

function toolCard(id, name, args, output, awaiting) {
  const done = output !== null && output !== undefined;
  const card = el('div', undefined, 'tool' + (done && exitFailed(output) ? ' err' : ''));
  const head = el('div', undefined, 'tool-h');
  head.append(el('span', done ? (exitFailed(output) ? '✗' : '✓') : '', done ? 'st mark' : 'st spin'),
    el('span', name, 'name'), el('span', toolSummary(name, args), 'arg'));
  const body = el('div', undefined, 'tool-b');
  if (name === 'apply_patch') {
    const files = parsePatch(parseArgs(args).patch || '');
    body.append(files.length ? diffBlock(files, true) : el('pre', args));
    if (done) body.append(el('pre', output, 'tool-out'));
  } else {
    body.append(el('pre', toolDetail(name, args) + (done ? '\n\n' + output : '')));
  }
  card.append(head, body);
  if (awaiting || state.openTools.has(id)) card.classList.add('open');
  head.onclick = () => {
    card.classList.toggle('open');
    if (card.classList.contains('open')) state.openTools.add(id); else state.openTools.delete(id);
  };
  if (awaiting) card.append(approvalBar(name));
  return card;
}

/* ---------- 对话渲染 ---------- */

function userBubble(text) {
  const node = el('div', undefined, 'message user');
  node.append(document.createTextNode(text));
  return node;
}

function assistantMessage(text, live) {
  const node = el('div', undefined, 'message assistant' + (live ? ' live' : ''));
  node._text = text;
  node.append(markdown(text));
  if (!live) {
    const copy = el('button', '复制', 'copy');
    copy.type = 'button';
    copy.onclick = async () => {
      try { await navigator.clipboard.writeText(text); copy.textContent = '已复制'; }
      catch { copy.textContent = '复制失败'; }
      setTimeout(() => { copy.textContent = '复制'; }, 1500);
    };
    node.append(copy);
  }
  return node;
}

function buildChat(snap) {
  const rows = snap.timeline || [];
  const pending = snap.pending;
  const nodes = [];
  let matched = false;
  for (const row of rows) {
    if (row.k === 'user') nodes.push(userBubble(row.text));
    else if (row.k === 'assistant') nodes.push(assistantMessage(row.text, false));
    else {
      const awaiting = !!pending && (pending.tool_call_id || pending.call_id) === row.id && row.output === null;
      matched ||= awaiting;
      nodes.push(toolCard(row.id, row.name, row.args, row.output, awaiting));
    }
  }
  if (!rows.length) for (const m of snap.messages || []) nodes.push(m.role === 'user' ? userBubble(m.content) : assistantMessage(m.content, false));
  // 用户消息写入会话日志之前先本地回显
  const lastSent = [...(snap.messages || [])].reverse().find(m => m.role === 'user');
  const lastShown = [...rows].reverse().find(r => r.k === 'user');
  if (snap.busy && lastSent && (!lastShown || !lastShown.text.includes(lastSent.content.trim()))) nodes.push(userBubble(lastSent.content));
  if (pending && !matched) nodes.push(toolCard(pending.call_id, pending.tool, pending.arguments, null, true));
  if (snap.busy && snap.draft) nodes.push(assistantMessage(snap.draft, true));
  else if (snap.busy && !pending && !rows.some(r => r.k === 'tool' && r.output === null)) nodes.push(el('div', '思考中…', 'message thinking'));
  return nodes;
}

function drawMessages(nodes) {
  const box = $('messages');
  const nearBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 120;
  document.querySelector('.conversation').classList.toggle('is-empty', !nodes.length);
  box.replaceChildren(...nodes);
  if (!nodes.length) {
    const empty = $('empty-state').content.cloneNode(true);
    empty.querySelectorAll('[data-q]').forEach(b => { b.onclick = () => { $('prompt').value = b.dataset.q; $('composer').requestSubmit(); }; });
    box.append(empty);
  }
  if (nearBottom) box.scrollTop = box.scrollHeight;
}

/* ---------- 右侧面板 ---------- */

function showPanel(name) {
  state.panel = name;
  for (const tab of document.querySelectorAll('[data-panel]')) {
    tab.setAttribute('aria-pressed', String(tab.dataset.panel === name));
    $(tab.dataset.panel + '-panel').hidden = tab.dataset.panel !== name;
  }
  if (name === 'changes') loadDiff().catch(e => notice(e.message));
}

for (const tab of document.querySelectorAll('[data-panel]')) tab.onclick = () => showPanel(tab.dataset.panel);

function showInspector(panel) {
  if ($('inspector-toggle').getAttribute('aria-pressed') !== 'true') $('inspector-toggle').click();
  showPanel(panel);
}

function renderTerminal(rows) {
  const shells = rows.filter(r => r.k === 'tool' && r.name === 'shell').slice(-30);
  const box = $('term');
  if (!shells.length) { box.replaceChildren(el('span', '命令的输出会显示在这里', 'term-empty')); return; }
  const nodes = [];
  for (const r of shells) {
    const p = el('div', undefined, 'term-cmd');
    p.append(el('span', '$ ', 'pr'), document.createTextNode(toolSummary('shell', r.args)));
    nodes.push(p, el('pre', r.output === null ? '运行中…' : r.output, 'term-out'));
  }
  const stick = box.parentElement.scrollHeight - box.parentElement.scrollTop - box.parentElement.clientHeight < 80;
  box.replaceChildren(...nodes);
  if (stick) box.parentElement.scrollTop = box.parentElement.scrollHeight;
}

function renderPendingDiff(snap) {
  const host = $('pending-diff');
  const p = snap.pending;
  if (!p || p.tool !== 'apply_patch') { host.replaceChildren(); return; }
  const files = parsePatch(parseArgs(p.arguments).patch || '');
  if (!files.length) { host.replaceChildren(); return; }
  const block = el('div', undefined, 'dblock wait');
  const head = el('div', undefined, 'dblock-h');
  head.append(el('span', files.map(f => f.path).join(', '), 'path'), el('span', '待审批', 'tag'));
  const deny = el('button', '拒绝'), allow = el('button', '接受并应用', 'primary');
  deny.dataset.decide = allow.dataset.decide = '1';
  deny.type = allow.type = 'button';
  deny.onclick = () => decide(false);
  allow.onclick = () => decide(true);
  head.append(deny, allow);
  block.append(head, diffBlock(files, true));
  host.replaceChildren(block);
}

async function loadDiff() {
  let result;
  try { result = await rpc('diff'); }
  catch (e) {
    $('diff-stat').textContent = '';
    $('diff-view').replaceChildren(noneNode('无法显示变更', '需要在已有提交的 Git 仓库中查看工作区差异（' + e.message + '）'));
    return;
  }
  const files = parsePatch(result.diff || '');
  let add = 0, del = 0;
  for (const f of files) for (const l of f.lines) { if (l.t === 'add') add++; else if (l.t === 'del') del++; }
  $('diff-stat').textContent = add || del ? ` +${add} −${del}` : '';
  const nodes = files.map(f => { const b = el('div', undefined, 'dblock'); b.append(el('div', f.path, 'dblock-h path'), diffBlock([f], true)); return b; });
  if (result.untracked?.length) nodes.push(el('pre', '未跟踪文件：\n' + result.untracked.join('\n'), 'untracked'));
  if (result.truncated) nodes.push(el('p', '[内容已截断]'));
  $('diff-view').replaceChildren(...(nodes.length ? nodes : [noneNode('暂无变更', '工作区与 HEAD 没有差异')]));
}

function noneNode(title, text) {
  const n = el('div', undefined, 'none');
  n.append(el('b', title), document.createTextNode(text));
  return n;
}

/* ---------- 文件树 ---------- */

function buildTree(paths) {
  const root = {dirs: new Map(), files: []};
  for (const path of paths) {
    const parts = path.split('/');
    let node = root;
    for (const part of parts.slice(0, -1)) {
      if (!node.dirs.has(part)) node.dirs.set(part, {dirs: new Map(), files: []});
      node = node.dirs.get(part);
    }
    node.files.push({name: parts[parts.length - 1], path});
  }
  return root;
}

function renderTreeNode(node, host, prefix = '') {
  for (const [name, child] of [...node.dirs].sort((a, b) => a[0].localeCompare(b[0]))) {
    const dirPath = prefix + name;
    const open = state.expanded.has(dirPath);
    const row = el('div', undefined, 'row dir');
    row.append(el('span', open ? '▾' : '▸', 'ic'), el('span', name));
    const kids = el('div', undefined, 'kids');
    kids.hidden = !open;
    renderTreeNode(child, kids, dirPath + '/');
    row.onclick = () => {
      const hide = !kids.hidden;
      kids.hidden = hide;
      row.firstChild.textContent = hide ? '▸' : '▾';
      if (hide) state.expanded.delete(dirPath); else state.expanded.add(dirPath);
    };
    host.append(row, kids);
  }
  for (const f of node.files.sort((a, b) => a.name.localeCompare(b.name))) {
    const row = el('div', undefined, 'row file' + (f.path === state.file ? ' on' : ''));
    row.dataset.path = f.path;
    row.append(el('span', undefined, 'ic'), el('span', f.name));
    row.onclick = () => openFile(f.path).catch(e => notice(e.message));
    host.append(row);
  }
}

function renderTree() {
  const host = $('tree');
  host.replaceChildren();
  if (!state.fileList.length) { host.append(el('div', '工作区还没有文件', 'tree-empty')); return; }
  renderTreeNode(buildTree(state.fileList), host);
}

async function loadFiles() {
  state.fileList = (await rpc('files')).files;
  renderTree();
}

function expandTo(path) {
  let dir = '';
  for (const part of path.split('/').slice(0, -1)) {
    dir += part;
    state.expanded.add(dir);
    dir += '/';
  }
}

/* ---------- 编辑器 ---------- */

let paintTimer = 0;
function paintEditor() {
  const value = $('editor-text').value;
  const hl = $('editor-hl');
  hl.replaceChildren();
  if (value.length < 200000) {
    for (const part of highlight(value, editor.path)) hl.append(part.cls ? el('span', part.text, part.cls) : document.createTextNode(part.text));
  } else hl.append(document.createTextNode(value));
  hl.append(document.createTextNode('\n'));
  const lines = value.split('\n').length;
  if (lines !== editor.lines) {
    editor.lines = lines;
    $('editor-gutter').textContent = Array.from({length: lines}, (_, i) => i + 1).join('\n');
  }
  syncEditorScroll();
}

function syncEditorScroll() {
  const ta = $('editor-text');
  $('editor-hl').scrollTop = ta.scrollTop;
  $('editor-hl').scrollLeft = ta.scrollLeft;
  $('editor-gutter').scrollTop = ta.scrollTop;
}

function updateEditorState(message = '') {
  const label = $('editor-state');
  label.className = 'editor-state' + (editor.conflict ? ' conflict' : editor.dirty ? ' dirty' : '');
  label.textContent = message || (!editor.editable ? '二进制文件，只读' : editor.conflict ? '磁盘上的文件已变化' : editor.dirty ? '● 未保存' : '');
  $('editor-save').disabled = !editor.editable || !editor.dirty || editor.saving;
}

async function loadIntoEditor(path) {
  const read = await rpc('read', {path});
  Object.assign(editor, {path, version: read.version, editable: read.editable, crlf: read.text.includes('\r\n'), dirty: false, conflict: false});
  state.file = path;
  const ta = $('editor-text');
  ta.value = read.text.replaceAll('\r\n', '\n');
  ta.readOnly = !read.editable;
  ta.scrollTop = ta.scrollLeft = 0;
  $('editor-path').textContent = path;
  $('editor-path').title = path;
  $('file-view').hidden = true;
  $('editor').hidden = false;
  expandTo(path);
  renderTree();
  paintEditor();
  updateEditorState();
  showInspector('code');
}

async function openFile(path) {
  if (editor.dirty && editor.path !== path) {
    return openDialog('放弃未保存的修改？', [], () => () => loadIntoEditor(path).catch(e => notice(e.message)),
      editor.path + ' 有未保存的修改，打开其他文件会丢弃它们。');
  }
  await loadIntoEditor(path);
}

async function saveFile() {
  if (!editor.path || !editor.dirty || !editor.editable || editor.saving) return;
  let text = $('editor-text').value;
  if (editor.crlf) text = text.replaceAll('\n', '\r\n');
  editor.saving = true;
  updateEditorState();
  try {
    const result = await rpc('write', {path: editor.path, text, base: editor.version});
    editor.version = result.version;
    editor.dirty = editor.conflict = false;
    notice('');
    editor.saving = false;
    updateEditorState('已保存 ✓');
    setTimeout(() => { if (!editor.dirty) updateEditorState(); }, 1800);
    loadDiff().catch(() => {});
  } catch (e) {
    editor.saving = false;
    if (/已变化/.test(e.message)) editor.conflict = true;
    notice(e.message);
    updateEditorState();
  }
}

// Agent 或外部程序改了磁盘上的文件：未编辑时静默更新，已编辑则提示冲突。
async function syncOpenFile() {
  if (!editor.path || editor.saving) return;
  let read;
  try { read = await rpc('read', {path: editor.path}); } catch { return; }
  if (read.version === editor.version) return;
  if (editor.dirty) { editor.conflict = true; updateEditorState(); return; }
  const ta = $('editor-text');
  const top = ta.scrollTop, left = ta.scrollLeft;
  editor.version = read.version;
  editor.crlf = read.text.includes('\r\n');
  ta.value = read.text.replaceAll('\r\n', '\n');
  ta.scrollTop = top; ta.scrollLeft = left;
  paintEditor();
  updateEditorState();
}

$('editor-text').addEventListener('input', () => {
  editor.dirty = true;
  updateEditorState();
  cancelAnimationFrame(paintTimer);
  paintTimer = requestAnimationFrame(paintEditor);
});
$('editor-text').addEventListener('scroll', syncEditorScroll);
$('editor-text').addEventListener('keydown', event => {
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 's') { event.preventDefault(); saveFile(); return; }
  if (event.key === 'Tab' && !event.shiftKey && !event.isComposing && editor.editable) {
    event.preventDefault();
    const ta = event.target;
    ta.setRangeText('  ', ta.selectionStart, ta.selectionEnd, 'end');
    ta.dispatchEvent(new Event('input'));
  }
});
onClick('editor-save', saveFile);
onClick('editor-reload', async () => {
  if (!editor.path) return;
  if (!editor.dirty) return loadIntoEditor(editor.path);
  openDialog('重新载入文件？', [], () => () => loadIntoEditor(editor.path).catch(e => notice(e.message)),
    '未保存的修改将被丢弃，并重新读取磁盘上的内容。');
});
window.addEventListener('beforeunload', event => {
  if (editor.dirty) { event.preventDefault(); event.returnValue = ''; }
});

onClick('refresh-files', loadFiles);
onClick('refresh-diff', loadDiff);

/* ---------- 轮询刷新 ---------- */

function renderSessions(sessions, busy) {
  const signature = JSON.stringify(sessions.map(s => [s.id, s.title, s.selected])) + busy;
  if ($('projects')._signature === signature) return;
  $('projects')._signature = signature;
  if (!sessions.length) { $('projects').replaceChildren(el('div', '暂无历史会话', 'tree-empty')); return; }
  $('projects').replaceChildren(...sessions.map(s => {
    const button = el('button', s.title || '空会话', s.selected ? 'selected' : '');
    button.title = s.title || '';
    if (s.selected) button.setAttribute('aria-current', 'true');
    button.disabled = busy && !s.selected;
    button.onclick = async () => {
      try { await rpc('session', {session_id: s.id}); state.openTools.clear(); await refresh(); } catch (e) { notice(e.message); }
    };
    return button;
  }));
}

function setQueueState(text, busy) {
  $('queue-state').textContent = text;
  $('queue-state').dataset.busy = busy ? 'true' : 'false';
}

function setSendMode(busy) {
  const b = $('send');
  if (state.user?.mode === 'server') {
    b.textContent = '↑'; b.type = 'submit'; b.classList.remove('stop');
    b.setAttribute('aria-label', busy ? '加入任务队列' : '发送任务');
    b.title = '发送（Enter）；项目正在执行时加入队列';
    $('stop-task').hidden = !busy;
    return;
  }
  $('stop-task').hidden = true;
  b.textContent = busy ? '■' : '↑';
  b.classList.toggle('stop', busy);
  b.setAttribute('aria-label', busy ? '停止任务' : '发送任务');
  b.type = busy ? 'button' : 'submit';
}

async function refresh() {
  if ((state.user?.mode === 'server' && !state.project) || !state.user || state.polling) return;
  state.polling = true;
  try {
    const snap = await rpc('snapshot');
    if (state.user.mode === 'server') { const info = await rpc('project_status'); state.projectInfo = info; const queued=info.jobs.filter(j=>j.status==='queued').length; $('host-jobs').textContent=info.blocked?'项目需要检查后恢复':(queued?`${queued} 个排队任务`:info.busy?'项目处理中':'无排队任务'); }
    state.snap = snap;
    const rows = snap.timeline || [];
    const signature = JSON.stringify([rows.length, rows.length ? rows[rows.length - 1] : null, snap.draft?.length, snap.busy, snap.pending?.call_id, snap.messages?.length, snap.session_id]);
    if (signature !== state.signature) {
      state.signature = signature;
      drawMessages(buildChat(snap));
      renderTerminal(rows);
      renderPendingDiff(snap);
      if (snap.pending?.tool === 'apply_patch') showInspector('changes');
    }
    // 工具完成数变化 → 刷新文件树与 Diff
    const finished = rows.filter(r => r.k === 'tool' && r.output !== null && r.name !== 'read_file' && r.name !== 'list_dir').length + ':' + snap.busy;
    if (finished !== state.toolSignature) {
      const first = state.toolSignature === '';
      state.toolSignature = finished;
      if (!first) { loadFiles().catch(() => {}); loadDiff().catch(() => {}); syncOpenFile(); }
    }
    renderSessions(snap.sessions || [], snap.busy);
    state.provider = snap.provider || null;
    updateChips(snap);
    const current = (snap.sessions || []).find(s => s.selected);
    $('project-title').textContent = current?.title || '新会话';
    setQueueState(snap.pending ? '等待批准' : snap.busy ? '正在执行' : '就绪', snap.busy);
    setSendMode(snap.busy);
    if (snap.error) { notice(snap.error); state.snapNotice = true; }
    else if (state.snapNotice) { notice(''); state.snapNotice = false; }
    setConnection('已连接', true);
  } catch (e) {
    setConnection('连接异常', false);
    notice(e.message);
  } finally {
    state.polling = false;
  }
}

/* ---------- 任务提交（沿用请求 ID 以安全重试） ---------- */

function pendingSubmission() {
  try { return JSON.parse(sessionStorage.getItem('codex.pending') || 'null'); } catch { return null; }
}

async function submitTask(event) {
  event.preventDefault();
  if (state.snap?.busy && state.user?.mode !== 'server') return;
  const text = $('prompt').value;
  if (!text.trim()) return;
  if (tryCommand(text)) return;
  const previous = pendingSubmission();
  const pending = previous && previous.text === text && previous.project===state.project && previous.scope===state.scope && previous.user===state.user.username ? previous : {text, project:state.project,scope:state.scope,user:state.user.username,request_id: newId()};
  sessionStorage.setItem('codex.pending', JSON.stringify(pending));
  $('send').disabled = true;
  try {
    await rpc('send', {text, request_id: pending.request_id});
    sessionStorage.removeItem('codex.pending');
    $('prompt').value = '';
    notice('');
    await refresh();
  } catch (e) {
    notice(e.message + '。保留相同内容再次发送可使用原请求 ID 安全重试。');
  } finally {
    $('send').disabled = false;
  }
}

$('composer').onsubmit = submitTask;
$('send').addEventListener('click', async event => {
  if (!state.snap?.busy || state.user?.mode === 'server') return;
  event.preventDefault();
  try { await rpc('stop'); await refresh(); } catch (e) { notice(e.message); }
});
onClick('stop-task', async () => { await rpc('stop'); await refresh(); });
$('prompt').addEventListener('keydown', event => {
  if (event.isComposing || event.keyCode === 229) return;
  if (menu.items.length) {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      menu.index = (menu.index + (event.key === 'ArrowDown' ? 1 : menu.items.length - 1)) % menu.items.length;
      drawMenu();
      return;
    }
    if (event.key === 'Tab' || (event.key === 'Enter' && !event.shiftKey)) {
      event.preventDefault();
      chooseCommand(menu.items[menu.index]);
      return;
    }
    if (event.key === 'Escape') { event.preventDefault(); closeMenu(); return; }
  }
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    $('composer').requestSubmit();
  }
});

/* ---------- 模型与权限 ---------- */

const PERMISSION_LABELS = {readonly: '只读', workspace: '默认权限', full: '完全访问'};
const PERMISSION_CHOICES = [
  ['readonly', '只读', '只能查看，不能修改文件'],
  ['workspace', '默认权限', '可写工作区，敏感操作需批准'],
  ['full', '完全访问', '无沙箱，不再请求批准'],
];

const EFFORT_LABELS = {'': '默认', none: '无', minimal: '极低', low: '低', medium: '中', high: '高', xhigh: '超高', max: '最大', ultra: 'Ultra'};
const DEFAULT_EFFORTS = ['', 'low', 'medium', 'high', 'xhigh'];
const effortLabel = value => EFFORT_LABELS[value] || value;
const catalog = {models: null, error: '', loading: false};

function updateChips(snap) {
  state.model = snap.model || '';
  state.reasoning = snap.reasoning || '';
  state.permission = snap.permission || 'workspace';
  $('model-chip').textContent = (state.model ? modelLabel(state.model) + (state.reasoning ? ' ' + effortTitle(state.reasoning) : '') : '默认模型') + ' ⌄';
  $('model-chip').title = '提供商：' + (snap.provider?.name || 'Codex');
  $('permission-chip').textContent = PERMISSION_LABELS[state.permission];
  $('permission-chip').dataset.level = state.permission;
  for (const id of ['model-chip', 'permission-chip']) $(id).disabled = snap.busy;
  if (snap.busy) closePopover();
}

const pop = {kind: null};

function closePopover() {
  pop.kind = null;
  $('popover').hidden = true;
  $('model-chip').setAttribute('aria-expanded', 'false');
  $('permission-chip').setAttribute('aria-expanded', 'false');
}

function popItem(title, desc, checked, onPick) {
  const item = el('button', undefined, 'pop-item');
  item.type = 'button';
  item.setAttribute('role', 'menuitemradio');
  item.setAttribute('aria-checked', String(checked));
  const text = el('span', undefined, 'pop-text');
  text.append(el('span', title, 'pop-title'));
  if (desc) text.append(el('span', desc, 'pop-desc'));
  item.append(text, el('span', checked ? '✓' : '', 'pop-check'));
  item.onclick = async () => {
    try { await onPick(); } catch (e) { notice(e.message); }
  };
  return item;
}

function modelLabel(id) {
  if (!id) return '默认';
  return /^gpt-/i.test(id) ? id.replace(/^gpt-/i, 'GPT-').replace(/-(sol|astra|terra|luna)$/i, (_, name) => ' ' + name[0].toUpperCase() + name.slice(1)) : id;
}
const effortTitle = value => ({'': '默认', none: '无', minimal: '极低', low: '轻', medium: '标准', high: '高', xhigh: '超高', max: '最大', ultra: 'Ultra'}[value] || value);
function currentModelInfo() { return (catalog.models || []).find(model => model.id === state.model); }
function drawEffortPopover() {
  const box = $('popover');
  box.dataset.view = 'effort'; box.setAttribute('role', 'dialog'); box.setAttribute('aria-label', '推理强度'); box.replaceChildren();
  const info = currentModelInfo();
  const levels = [...new Set((info?.efforts || []).filter(level => typeof level === 'string' && level))];
  const current = state.reasoning || info?.default_effort || '';
  const head = el('div', undefined, 'effort-header');
  const choice = el('button', undefined, 'effort-choice'); choice.type = 'button'; choice.setAttribute('aria-label', '选择模型');
  const title = el('span', effortTitle(current), 'effort-title');
  choice.append(title, el('span', modelLabel(state.model) + ' ›', 'effort-model'));
  choice.onclick = () => { pop.view = 'catalog'; drawModelPopover(); $('popover').querySelector('.model-back').focus(); };
  head.append(choice); box.append(head);
  if (levels.length) {
    const track = el('div', undefined, 'effort-track');
    const input = el('input'); input.type = 'range'; input.min = '0'; input.max = String(levels.length - 1); input.step = '1'; input.value = String(Math.max(0, levels.indexOf(current))); input.disabled = levels.length < 2;
    input.setAttribute('aria-label', '推理强度'); input.setAttribute('aria-valuetext', effortTitle(current || levels[0]));
    const dots = el('div', undefined, 'effort-dots');
    levels.forEach(level => { const dot = el('span'); dot.title = effortTitle(level); dots.append(dot); });
    input.oninput = () => { title.textContent = effortTitle(levels[Number(input.value)]); input.setAttribute('aria-valuetext', title.textContent); };
    input.onchange = async () => { try { await rpc('reasoning', {effort: levels[Number(input.value)]}); await refresh(); drawModelPopover(); $('popover').querySelector('input[type=range]')?.focus(); } catch (error) { notice(error.message); drawModelPopover(); } };
    track.append(dots, input); box.append(track);
  } else {
    box.append(el('div', catalog.loading ? '正在获取模型能力…' : '提供商未声明推理强度，可手动指定。', 'pop-note-plain'));
    if (!catalog.loading) {
      const select = el('select'); select.setAttribute('aria-label', '手动指定推理强度');
      ['', ...new Set([...DEFAULT_EFFORTS.filter(Boolean), ...(state.reasoning ? [state.reasoning] : [])])].forEach(value => select.append(new Option(effortTitle(value), value)));
      select.value = state.reasoning || ''; select.onchange = async () => { try { await rpc('reasoning', {effort: select.value}); await refresh(); drawModelPopover(); } catch (error) { notice(error.message); } };
      box.append(select);
    }
  }
  if (catalog.error) box.append(el('div', '无法获取模型列表：' + catalog.error, 'pop-note-plain'));
}
function drawModelPopover() {
  const box = $('popover');
  if (pop.kind !== 'model') return;
  $('model-chip').setAttribute('aria-expanded', 'true');
  if (pop.view !== 'catalog') return drawEffortPopover();
  const typed = box.querySelector('.pop-custom input')?.value || '';
  box.dataset.view = 'catalog'; box.setAttribute('role', 'menu'); box.setAttribute('aria-label', '选择模型'); box.replaceChildren();
  const back = el('button', '‹ 选择模型', 'model-back'); back.type = 'button'; back.setAttribute('aria-label', '返回推理强度'); back.onclick = () => { pop.view = 'effort'; drawModelPopover(); box.querySelector('.effort-choice').focus(); }; box.append(back);
  const pick = value => async () => { await rpc('model', {model: value}); await refresh(); pop.view = 'effort'; drawModelPopover(); box.querySelector('.effort-choice').focus(); };
  box.append(popItem('默认', '使用提供商默认模型', !state.model, pick('')));
  const fetched = catalog.models || [];
  const own = (state.provider?.models || []).filter(id => !fetched.some(m => m.id === id)).map(id => ({id, description: '自定义模型'}));
  const known = [...own, ...fetched];
  const list = state.model && !known.some(model => model.id === state.model) ? [...known, {id: state.model, description: ''}] : known;
  for (const model of list) box.append(popItem(modelLabel(model.id), model.description, model.id === state.model, pick(model.id)));
  if (catalog.loading) box.append(el('div', '正在加载模型列表…', 'pop-note-plain'));
  else if (catalog.error && !own.length) box.append(el('div', '无法获取模型列表：' + catalog.error + '。可在“提供商”里手动添加模型，或在下方输入模型名。', 'pop-note-plain'));
  const refreshButton = el('button', '刷新模型列表', 'model-refresh'); refreshButton.type = 'button'; refreshButton.disabled = catalog.loading; refreshButton.onclick = () => loadCatalog(true); box.append(refreshButton);
  const custom = el('form', undefined, 'pop-custom'); const input = el('input'); input.placeholder = '自定义模型名'; input.setAttribute('aria-label', '自定义模型名'); input.pattern = '[A-Za-z0-9._:/\\-]+'; input.maxLength = 200; input.value = typed;
  const apply = el('button', '应用'); apply.type = 'submit'; custom.append(input, apply);
  custom.onsubmit = async event => { event.preventDefault(); if (!input.value.trim()) return; try { await pick(input.value.trim())(); } catch (error) { notice(error.message); } };
  box.append(custom);
}

async function loadCatalog(force = false) {
  if (catalog.loading || (!force && catalog.models)) return;
  catalog.loading = true;
  drawModelPopover();
  try {
    const result = await rpc('models', {refresh: force});
    catalog.models = result.models.length ? result.models : null;
    catalog.error = result.error || '';
  } catch (e) {
    catalog.error = e.message;
  } finally {
    catalog.loading = false;
    drawModelPopover();
  }
}

function openPopover(kind) {
  if (pop.kind === kind) return closePopover();
  pop.kind = kind;
  pop.view = 'effort';
  const box = $('popover');
  box.replaceChildren();
  if (kind === 'model') {
    drawModelPopover();
    loadCatalog();
  } else {
    box.dataset.view = 'permissions'; box.setAttribute('role', 'menu'); box.setAttribute('aria-label', '执行权限');
    box.append(el('div', '执行权限', 'pop-heading'));
    for (const [mode, title, desc] of PERMISSION_CHOICES) {
      if (mode === 'full' && state.user?.mode === 'server') continue;
      box.append(popItem(title, desc, mode === state.permission, async () => { closePopover(); await openPermissions(mode); }));
    }
    $('permission-chip').setAttribute('aria-expanded', 'true');
  }
  box.hidden = false;
  if (kind === 'model') box.querySelector('.effort-choice')?.focus();
}

function openModel(value) {
  if (value !== undefined) return rpc('model', {model: value}).then(refresh);
  openPopover('model');
}

async function openPermissions(choice) {
  if (!choice) return openPopover('permission');
  if (choice === state.permission) return;
  if (choice !== 'full') { await rpc('permission', {permission: choice}); return refresh(); }
  openDialog('确认完全访问', [], () => rpc('permission', {permission: 'full'}),
    '完全访问会关闭沙箱，Agent 可在本机读写任意文件并运行任意命令，且不再请求批准。\n仅在信任当前任务时开启；重启服务后自动恢复默认权限。');
}

$('model-chip').onclick = () => openModel();
$('permission-chip').onclick = () => openPermissions();
document.addEventListener('mousedown', event => {
  if (pop.kind && !event.target.closest('#popover, .chip')) closePopover();
});
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && pop.kind) { closePopover(); $('prompt').focus(); }
});

/* ---------- 斜杠命令 ---------- */

async function copyLast() {
  const last = [...$('messages').querySelectorAll('.message.assistant')].pop();
  if (!last) throw new Error('还没有可复制的回复');
  await navigator.clipboard.writeText(last._text);
  notice('已复制上一条回复');
}

function statusText() {
  return [
    `工作区：${$('root-name').textContent || '-'}`,
    `模型：${state.model || '默认'}${state.reasoning ? ' · ' + effortTitle(state.reasoning) : ''}`,
    `权限：${PERMISSION_LABELS[state.permission] || '-'}`,
    `状态：${$('queue-state').textContent || '-'}`,
  ].join('\n');
}

const COMMANDS = [
  {name: '/model', desc: '切换模型（/model 名称）', takesArg: true, run: arg => openModel(arg || undefined)},
  {name: '/permissions', desc: '调整执行权限（readonly / default / full）', takesArg: true, run: arg => {
    const alias = {readonly: 'readonly', 'read-only': 'readonly', default: 'workspace', workspace: 'workspace', full: 'full'};
    if (arg && !alias[arg]) throw new Error('可选：readonly、default、full');
    openPermissions(alias[arg]);
  }},
  {name: '/new', desc: '开始新会话', run: newChat},
  {name: '/sessions', desc: '切换本地会话', run: openSessions},
  {name: '/provider', desc: '模型提供商与密钥', run: () => openSettings()},
  {name: '/login', desc: 'ChatGPT 设备登录', run: async () => {
    await rpc('login', {request_id: newId()});
    notice('已发起设备登录，请查看对话中的登录提示');
  }},
  {name: '/status', desc: '显示当前配置', run: () => openDialog('当前状态', [], () => {}, statusText())},
  {name: '/diff', desc: '查看当前修改', run: () => showInspector('changes')},
  {name: '/files', desc: '查看代码面板', run: () => showInspector('code')},
  {name: '/copy', desc: '复制上一条回复', run: copyLast},
  {name: '/help', desc: '列出所有命令', run: () => openDialog('斜杠命令', [], () => {},
    COMMANDS.map(c => `${c.name.padEnd(14)}${c.desc}`).join('\n'))},
];

const TERMINAL_ONLY = ['/plan', '/design', '/review', '/compact', '/init', '/resume', '/fork', '/rename',
  '/undo', '/memory', '/skills', '/plugins', '/mcp', '/image', '/mention', '/goal', '/personality',
  '/continue', '/archive', '/delete', '/clear', '/logout', '/auth', '/quit'];

const menu = {items: [], index: 0};

function menuMatches(value) {
  if (!value.startsWith('/') || /\s/.test(value)) return [];
  return COMMANDS.filter(c => c.name.startsWith(value.toLowerCase()));
}

function closeMenu() {
  menu.items = [];
  $('slash-menu').hidden = true;
}

function drawMenu() {
  const box = $('slash-menu');
  box.hidden = !menu.items.length;
  box.replaceChildren(...menu.items.map((c, i) => {
    const row = el('button', undefined, 'slash-item');
    row.type = 'button';
    row.setAttribute('role', 'option');
    row.setAttribute('aria-selected', String(i === menu.index));
    row.append(el('span', c.name, 'slash-name'), el('span', c.desc, 'slash-desc'));
    row.onmousedown = event => { event.preventDefault(); chooseCommand(c); };
    return row;
  }));
}

function chooseCommand(command) {
  closeMenu();
  if (command.takesArg) {
    $('prompt').value = command.name + ' ';
    $('prompt').focus();
    return;
  }
  $('prompt').value = '';
  executeCommand(command, '');
}

async function executeCommand(command, arg) {
  try { await command.run(arg); } catch (e) { notice(e.message); }
}

// 返回 true 表示输入已作为命令处理，不应发送给 Agent。
function tryCommand(text) {
  const match = /^(\/[A-Za-z-]+)(?:\s+([\s\S]*))?$/.exec(text.trim());
  if (!match) return false;
  const name = match[1].toLowerCase();
  const command = COMMANDS.find(c => c.name === name);
  if (command) {
    $('prompt').value = '';
    closeMenu();
    executeCommand(command, (match[2] || '').trim());
    return true;
  }
  if (TERMINAL_ONLY.includes(name)) {
    notice(`${name} 目前只能在终端界面使用；输入 /help 查看网页支持的命令。`);
    return true;
  }
  return false;
}

$('prompt').addEventListener('input', () => {
  menu.items = menuMatches($('prompt').value);
  menu.index = 0;
  drawMenu();
});
$('prompt').addEventListener('blur', closeMenu);

/* ---------- 设置与会话 ---------- */

function providerForm(p) {
  openDialog(p ? '编辑提供商' : '添加提供商', [
    field('name', '名称', 'text', p?.name || ''),
    field('base_url', '服务地址（如 https://api.example.com/v1）', 'text', p?.base_url || ''),
    field('api', 'API 类型', 'select', p?.api || 'chat', ['chat', 'responses']),
    field('models', '模型（多个用逗号分隔，第一个为默认；可留空）', 'text', (p?.models || []).join(', '), [], false),
    field('key', p?.has_key ? 'API 密钥（留空保持不变）' : 'API 密钥（本地服务可留空）', 'password'),
  ], async data => {
    await rpc('provider_save', {id: p?.id, ...data});
    catalog.models = null; catalog.error = '';
    return () => openProviders().catch(e => notice(e.message));
  });
}

async function fillProviders() {
  const data = await rpc('providers');
  const editable=state.user.mode!=='server'||state.scope==='personal'||state.projectInfo?.owner===state.user.username;
  const list = el('div', undefined, 'prov-list');
  const act = (label, fn, cls) => {
    const b = el('button', label, cls); b.type = 'button';
    b.onclick = async () => { try { await fn(); await fillProviders(); await refresh(); } catch (e) { $('dialog-error').textContent = e.message; } };
    return b;
  };
  for (const p of data.providers) {
    const row = el('div', undefined, 'prov' + (p.id === data.active ? ' on' : ''));
    const info = el('div', undefined, 'prov-info');
    const sub = p.id === 'codex' ? '内置 · 使用 ChatGPT / Codex 订阅登录'
      : p.builtin ? '由启动参数指定'
      : `${p.base_url} · ${p.api} · ${p.models.length ? p.models.length + ' 个模型' : '未设置模型'}${p.has_key ? ' · 已保存密钥' : ''}`;
    info.append(el('b', p.name), el('span', sub, 'prov-sub'));
    const actions = el('div', undefined, 'prov-actions');
    if (p.id === data.active) actions.append(el('span', '使用中', 'prov-badge'));
    else if (p.id !== 'cli') actions.append(act('使用', async () => {
      await rpc('provider_use', {id: p.id}); catalog.models = null; catalog.error = '';
    }, 'primary'));
    if (p.id === 'codex' && editable) actions.append(act('设备登录', async () => { await rpc('login', {request_id: newId()}); $('dialog').close(); notice('已发起设备登录，请查看对话中的登录提示'); }));
    if (!p.builtin && editable) {
      const edit = el('button', '编辑'); edit.type = 'button';
      edit.onclick = () => { $('dialog').close(); providerForm(p); };
      actions.append(edit, act('删除', async () => { await rpc('provider_delete', {id: p.id}); catalog.models = null; catalog.error = ''; }));
    }
    row.append(info, actions);
    list.append(row);
  }
  const add = el('button', '＋ 添加提供商', 'prov-add'); add.type = 'button';
  add.onclick = () => { $('dialog').close(); providerForm(null); };
  if(editable) list.append(add);
  list.append(el('p', '只内置 Codex；其他兼容服务可手动添加。密钥只写入当前会话所属的私有凭据目录，不提供读取接口。', 'prov-note'));
  showDialog('模型提供商', list);
}

async function openProviders() { await fillProviders(); }
function openSettings() { return openProviders(); }

onClick('settings-button', openProviders);

async function openSessions() {
  const snap = await rpc('snapshot');
  openDialog('切换本地会话',
    [field('session_id', '会话', 'select', '新建', ['新建', ...snap.sessions.map(s => s.id)])],
    data => rpc('session', {session_id: data.session_id === '新建' ? null : data.session_id}));
}

async function newChat() {
  await rpc('session', {session_id: null});
  state.openTools.clear();
  $('prompt').focus();
  await refresh();
}
onClick('new-chat', newChat);

/* ---------- 侧栏与检查器开关 ---------- */

const narrowScreen = matchMedia('(max-width: 760px)');
function setSidebar(open, restoreFocus = false) {
  $('sidebar-toggle').setAttribute('aria-pressed', String(open));
  $('workspace').classList.toggle('sidebar-collapsed', !open);
  $('workspace').classList.toggle('sidebar-open', open);
  if (narrowScreen.matches) {
    document.querySelector('main').inert = open;
    if (open) document.querySelector('.sidebar a').focus();
    else if (restoreFocus) $('sidebar-toggle').focus();
  } else document.querySelector('main').inert = false;
}
setSidebar(!narrowScreen.matches);
narrowScreen.addEventListener('change', () => setSidebar(!narrowScreen.matches));
onClick('sidebar-toggle', () => setSidebar($('sidebar-toggle').getAttribute('aria-pressed') !== 'true'));
onClick('sidebar-backdrop', () => setSidebar(false, true));
document.querySelector('.sidebar').addEventListener('click', event => {
  if (narrowScreen.matches && event.target.closest('#projects button, #new-chat')) setSidebar(false);
});
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && narrowScreen.matches && $('workspace').classList.contains('sidebar-open')) setSidebar(false, true);
});

function setInspector(open) {
  $('inspector-toggle').setAttribute('aria-pressed', String(open));
  document.querySelector('.workspace-body').classList.toggle('no-inspector', !open);
}
setInspector(!narrowScreen.matches);
onClick('inspector-toggle', () => setInspector($('inspector-toggle').getAttribute('aria-pressed') !== 'true'));

/* ---------- 启动 ---------- */

async function initialize() {
  state.user = await http('/api/me');
  document.body.dataset.mode = state.user.mode;
  $('workspace').hidden=false; $('host-login').hidden=true;
  if(state.user.mode==='server'){ $('host-controls').hidden=false; await hostProjects(); if(!state.project){$('team-button').disabled=false;notice('先创建项目，或请项目创建者邀请你。'); if(!state.loopStarted){state.loopStarted=true;loop();} return;} }
  const status = await rpc('status');
  $('root-name').textContent = String(status.workspace || '').split('/').filter(Boolean).pop() || '';
  await refresh();
  await loadFiles().catch(e => notice(e.message));
  $('team-button').disabled=false;
  if(!state.loopStarted){state.loopStarted=true;loop();}
}

function loop() {
  setTimeout(async () => { await refresh(); loop(); }, state.snap?.busy ? 400 : 1200);
}

async function hostProjects(preferred) {
 const {projects}=await rpc('projects'); $('host-project').replaceChildren(...projects.map(id=>new Option(id,id)));
 state.project=preferred&&projects.includes(preferred)?preferred:projects.includes(state.project)?state.project:(projects[0]||'');$('host-project').value=state.project;
}
async function switchContext(){
 state.generation++;
 state.signature='';state.toolSignature='';state.snap=null;state.openTools.clear();state.file=null;editor.path=null;editor.dirty=false;state.expanded.clear();catalog.models=null;catalog.error='';$('tree').replaceChildren();$('projects').replaceChildren();$('messages').replaceChildren();
 closePopover(); state.diffSignature=''; $('editor').hidden=true; $('file-view').hidden=false; $('editor-text').value='';
 $('pending-diff').replaceChildren(); $('diff-view').replaceChildren(); $('term').replaceChildren();
 $('prompt').value=''; $('root-name').textContent=''; state.projectInfo=null;
 if(state.project){if(state.scope==='personal')await rpc('personal');const status=await rpc('status');$('root-name').textContent=String(status.workspace||'').split(/[\\/]/).filter(Boolean).pop()||'';await refresh();await loadFiles();}
}
$('host-login-form').onsubmit=async event=>{event.preventDefault();try{await http('/api/login',{username:$('host-user').value,password:$('host-token').value});$('host-token').value='';await initialize();}catch(error){$('host-login-error').textContent=error.message;}};
onClick('host-logout',async()=>{await http('/api/logout',{});location.reload();});
$('host-project').onchange=async()=>{if(editor.dirty&&!confirm('放弃未保存的编辑？')){$('host-project').value=state.project;return;}state.project=$('host-project').value;try{await switchContext();}catch(error){notice(error.message);}};
$('host-scope').onchange=async()=>{if(editor.dirty&&!confirm('放弃未保存的编辑？')){$('host-scope').value=state.scope;return;}state.scope=$('host-scope').value;try{await switchContext();}catch(error){notice(error.message);}};
onClick('host-create',openTeamProjectCreate);
onClick('host-manage', () => openTeam('members'));


/* ---------- 团队设置 ---------- */
let teamTicket=0,teamTimer=null,teamDraft=null;
function closeTeam(){if($('team-dialog').open)$('team-dialog').close();}
function teamAction(label,fn,primary=false){const b=el('button',label,primary?'primary':'');b.type='button';b.onclick=async()=>{b.disabled=true;try{await fn();}catch(e){$('team-error').textContent=e.message;}finally{if(b.isConnected&&!b.hasAttribute('data-stopped-only')&&!b.hasAttribute('data-running-only'))b.disabled=false;}};return b;}
function teamNote(text){return el('p',text,'team-note');}
function teamRow(title,subtitle){const row=el('div',undefined,'team-row'),content=el('div');content.append(el('strong',title));if(subtitle)content.append(el('small',subtitle));row.append(content);return row;}
function teamInput(form,label,name,value,type='text'){const group=el('label',label),input=el('input');input.name=name;input.type=type;input.value=value;input.required=true;group.append(input);form.append(group);return input;}
function serverOrigin(value){const url=new URL(value);if(!['http:','https:'].includes(url.protocol)||url.username||url.password||url.search||url.hash||url.pathname!=='/')throw Error('请填写不含路径或账号的 HTTP/HTTPS 地址');return url.origin;}
function openTeamProjectCreate(){closeTeam();openDialog('创建项目',[field('name','项目名称')],async data=>{const result=await rpc('create',data);state.scope='shared';$('host-scope').value='shared';await hostProjects(result.project);await switchContext();return()=>openTeam('members').catch(e=>notice(e.message));});}
function showTeamToken(result){$('team-token-title').textContent='保存 '+result.username+' 的访问令牌';$('team-token').value=result.token;$('team-token').type='password';$('team-token-show').setAttribute('aria-pressed','false');$('team-token-show').textContent='显示令牌';$('team-token-feedback').textContent='';$('team-token-dialog').showModal();}
onClick('team-token-show',()=>{const input=$('team-token');const show=input.type==='password';input.type=show?'text':'password';$('team-token-show').setAttribute('aria-pressed',String(show));$('team-token-show').textContent=show?'隐藏令牌':'显示令牌';});
onClick('team-token-copy',async()=>{try{await navigator.clipboard.writeText($('team-token').value);$('team-token-feedback').textContent='已复制';}catch{$('team-token').type='text';$('team-token').select();$('team-token-feedback').textContent='请手动复制选中的令牌';}});
onClick('team-token-close',()=>{$('team-token-dialog').close();});
$('team-token-dialog').addEventListener('close',()=>{$('team-token').value='';$('team-token').type='password';openTeam('accounts').catch(e=>notice(e.message));});
onClick('team-close',closeTeam);
$('team-dialog').addEventListener('close',()=>{teamTicket++;clearTimeout(teamTimer);teamTimer=null;});
onClick('team-button',()=>openTeam());
async function openTeam(selected,keyboard=false){
 const ticket=++teamTicket;clearTimeout(teamTimer);$('team-error').textContent='';
 const local=state.user?.mode==='local';const tabs=local?[['service','本机服务'],['accounts','账号'],['remote','连接远程']]:[['members','成员'],['models','共同模型'],['branches','分支'],['activity','活动']];
 selected=tabs.some(x=>x[0]===selected)?selected:tabs[0][0];
 $('team-title').textContent='团队设置';$('team-subtitle').textContent=local?'在本机管理团队服务，或连接已有服务器':state.project?'项目：'+state.project:'先创建项目，或请创建者邀请你';
 $('team-tabs').replaceChildren();for(const [id,label] of tabs){const button=el('button',label);button.type='button';button.id='team-tab-'+id;button.setAttribute('role','tab');button.setAttribute('aria-selected',String(id===selected));button.setAttribute('aria-controls','team-body');button.tabIndex=id===selected?0:-1;button.onclick=()=>openTeam(id,true).catch(e=>notice(e.message));button.onkeydown=event=>{const i=tabs.findIndex(t=>t[0]===id);let next;if(event.key==='ArrowRight')next=(i+1)%tabs.length;else if(event.key==='ArrowLeft')next=(i+tabs.length-1)%tabs.length;else if(event.key==='Home')next=0;else if(event.key==='End')next=tabs.length-1;else return;event.preventDefault();openTeam(tabs[next][0],true).catch(e=>notice(e.message));};$('team-tabs').append(button);}
 $('team-body').setAttribute('aria-labelledby','team-tab-'+selected);$('team-body').replaceChildren(el('p','正在加载…'));
 if(!$('team-dialog').open)$('team-dialog').showModal();if(keyboard)$('team-tab-'+selected).focus();
 try {if(local){const data=await rpc('team_get');if(ticket!==teamTicket)return;renderLocalTeam(selected,data,ticket);}else {if(!state.project){$('team-body').replaceChildren(teamNote('创建项目后即可管理成员、共同模型和分支。'),teamAction('创建项目',openTeamProjectCreate,true));return;}const info=await rpc('project_status');if(ticket!==teamTicket)return;state.projectInfo=info;await renderHostedTeam(selected,info,ticket);}}catch(e){if(ticket===teamTicket){$('team-body').replaceChildren();$('team-error').textContent=e.message;}}
}
function renderLocalTeam(tab,data,ticket){
 const body=$('team-body');body.replaceChildren();let busy=false,current=data;
 const status=el('p',undefined,'team-note');status.id='team-server-state';
 const paint=next=>{current=next;status.textContent=next.stopping?'停止中…':next.running?'团队服务运行中':'团队服务已停止';for(const control of body.querySelectorAll('[data-stopped-only]'))control.disabled=next.running||busy;for(const control of body.querySelectorAll('[data-running-only]'))control.disabled=!next.running||next.stopping||busy;if(next.error)$('team-error').textContent=next.error;};
 const act=async fn=>{busy=true;paint(current);try{const result=await fn();if(ticket!==teamTicket)return;paint(result);}finally{busy=false;if(ticket===teamTicket)paint(current);}};
 const stopped=b=>{b.dataset.stoppedOnly='';return b;};
 body.append(status);
 if(!data.sandbox_available)body.append(teamNote('当前系统沙箱不可用，Agent 与协作仓库任务会被拒绝执行；服务配置和账号管理仍可使用。'));
 if(tab==='service'){
  const form=el('form');const host=teamInput(form,'监听 IPv4 地址','host',teamDraft?.host??data.host);host.placeholder='0.0.0.0';const origin=teamInput(form,'访问地址（可选）','origin',teamDraft?.origin??data.origin,'url');origin.required=false;origin.placeholder='留空即可通过 IP 和端口直接连接';
  const port=teamInput(form,'本机监听端口','port',teamDraft?.port??String(data.port),'number');port.min='1';port.max='65535';
  for(const input of [host,origin,port]){input.dataset.stoppedOnly='';input.oninput=()=>{teamDraft={host:host.value,origin:origin.value,port:port.value};};}
  const feedback=el('p',undefined,'team-note');feedback.setAttribute('role','status');
  const save=async()=>{if(!form.reportValidity())throw Error('请填写有效地址和端口');const result=await rpc('team_save',{host:host.value,origin:origin.value.trim()?serverOrigin(origin.value.trim()):'',port:Number(port.value)});origin.value=result.origin;port.value=String(result.port);teamDraft=null;return result;};
  const saveButton=stopped(el('button','保存配置'));saveButton.type='submit';
  form.onsubmit=async event=>{event.preventDefault();try{await act(save);feedback.textContent='配置已保存';$('team-error').textContent='';}catch(e){$('team-error').textContent=e.message;}};
  const start=stopped(teamAction('启动团队服务',()=>act(async()=>{await save();return rpc('team_start');}),true));
  const stop=teamAction('停止团队服务',()=>{closeTeam();openDialog('停止团队服务',[],async()=>{await rpc('team_stop',{confirmed:true});return()=>openTeam('service').catch(e=>notice(e.message));},'正在执行的团队任务会被中断，文件和排队任务仍会保留。');});stop.dataset.runningOnly='';
  const actions=el('div',undefined,'team-actions');actions.append(saveButton,start,stop);form.append(actions,feedback);body.append(form);
  body.append(teamNote('0.0.0.0 允许其他设备通过服务器 IP 和端口连接；127.0.0.1 只允许本机连接。访问地址可以留空，HTTPS 代理按需配置。HTTP 不加密传输内容。关闭本地程序也会停止它启动的团队服务。'));
  const details=el('details');details.append(el('summary','部署与数据位置'),el('pre',`直接连接：http://服务器IP:${data.port}\n本机访问：${data.url}\n\n可选 HTTPS 代理示例：\ncodex.example.com {\n    reverse_proxy 127.0.0.1:${data.port}\n}\n\n数据目录：${data.state_dir}`));body.append(details);
  if(data.url)body.append(teamAction('打开团队网页',()=>window.open(serverOrigin(current.url),'_blank','noopener,noreferrer')));
 } else if(tab==='accounts'){
  body.append(teamNote('先停止本机托管的团队服务，再登记或重置登录账号；项目成员仍由项目创建者管理。'));
  body.append(stopped(teamAction('登记账号',()=>{closeTeam();openDialog('登记团队账号',[field('username','用户名')],async fields=>{const result=await rpc('team_account',fields);return()=>showTeamToken(result);});},true)));
  if(!data.users.length)body.append(teamNote('还没有登录账号。登记至少一个账号后即可启动服务。'));
  for(const name of data.users){const row=teamRow(name,'访问令牌不会在这里回显');row.append(stopped(teamAction('重置令牌',()=>{closeTeam();openDialog('重置 '+name+' 的访问令牌',[],async()=>{const result=await rpc('team_account',{username:name,confirmed:true});return()=>showTeamToken(result);},'旧令牌将失效。服务重启后，该用户需要用新令牌重新登录。');})),stopped(teamAction('停用账号',()=>{closeTeam();openDialog('停用 '+name,[],async()=>{await rpc('team_revoke',{username:name,confirmed:true});return()=>openTeam('accounts').catch(e=>notice(e.message));},'账号将无法登录，不删除项目文件。同名账号重新登记后会恢复原有的成员关系。');})));body.append(row);}
 } else {
  const form=el('form'),url=teamInput(form,'服务器 HTTP/HTTPS 地址','server',data.url,'url');url.placeholder='http://192.168.1.10:8766';const button=el('button','打开服务器登录页','primary');button.type='submit';form.append(button);form.onsubmit=event=>{event.preventDefault();try{window.open(serverOrigin(url.value),'_blank','noopener,noreferrer');}catch(e){$('team-error').textContent=e.message;}};body.append(form,teamNote('账号和访问令牌在服务器登录页填写，不会发送给当前本地服务。'));
 }
 paint(data);
 if(tab!=='remote'){const poll=async()=>{try{const next=await rpc('team_get');if(ticket!==teamTicket)return;paint(next);}catch(e){if(ticket===teamTicket)$('team-error').textContent=e.message;}if(ticket===teamTicket&&$('team-dialog').open)teamTimer=setTimeout(poll,1000);};teamTimer=setTimeout(poll,1000);}
}
async function sharedProviderSettings(){if(editor.dirty&&!confirm('切换到共同会话会丢弃未保存的编辑，是否继续？'))return;closeTeam();state.scope='shared';$('host-scope').value='shared';await switchContext();await fillProviders();}
async function renderHostedTeam(tab,info,ticket){
 const body=$('team-body');body.replaceChildren();const owner=info.owner===state.user.username;
 const reopen=()=>openTeam(tab).catch(e=>notice(e.message));
 const enqueue=(op,data={})=>rpc(op,{...data,request_id:newId()});
 if(tab==='members'){
  body.append(teamNote('创建者管理成员和共同凭据；所有成员的开发与审批权限相同。'));
  if(owner)body.append(teamAction('邀请成员',async()=>{const {users}=await rpc('member_candidates');if(ticket!==teamTicket)return;if(!users.length){$('team-error').textContent='没有可邀请的账号，请先在服务器本机的团队设置中登记账号。';return;}closeTeam();openDialog('邀请项目成员',[field('username','用户名')],async fields=>{await rpc('invite',fields);return reopen;});const input=$('dialog-body').querySelector('[name=username]'),list=el('datalist');list.id='team-candidates';for(const name of users)list.append(new Option(name,name));input.setAttribute('list',list.id);$('dialog-body').append(list);},true));
  for(const member of info.members){const row=teamRow(member,member===info.owner?'项目创建者':'项目成员');if(owner&&member!==info.owner)row.append(teamAction('移除成员',()=>{closeTeam();openDialog('移除 '+member,[],async()=>{await rpc('remove',{username:member});return reopen;},'此成员将失去项目访问权限，排队任务会被取消，个人文件仍保留。');}));body.append(row);}
  body.append(teamAction('创建另一个项目',openTeamProjectCreate));
 } else if(tab==='models'){
  const data=await rpc('providers',{scope:'shared'});if(ticket!==teamTicket)return;
  body.append(teamNote(owner?'共同会话使用这里的项目凭据；个人会话仍使用每位成员自己的凭据。':'共同凭据由项目创建者配置；你可以使用，但无法读取或修改密钥。'));
  for(const provider of data.providers)body.append(teamRow(provider.name,(provider.id===data.active?'正在使用 · ':'')+(provider.has_key?'密钥已配置':provider.id==='codex'?'订阅登录':'未配置密钥')));
  body.append(teamAction(owner?'配置共同模型':'选择共同模型',sharedProviderSettings,true));
 } else if(tab==='branches'){
  body.append(teamNote('个人与共同工作区各自独立。先发布个人分支，再明确合并到共同分支。'));
  body.append(teamAction('查看 Diff',()=>{closeTeam();showInspector('changes');return loadDiff();}));
  const actions=el('div',undefined,'team-actions');
  actions.append(teamAction('发布个人分支',()=>{closeTeam();openDialog('发布个人分支',[field('message','提交说明')],fields=>enqueue('publish',fields));}),teamAction('同步共同分支',()=>{closeTeam();openDialog('同步共同分支',[],()=>enqueue('sync'),'个人工作区必须干净；冲突时保留原目标。');}),teamAction('提交共同修改',async()=>{const preview=await rpc('diff',{scope:'shared'});closeTeam();openDialog('检查并提交共同修改',[field('message','提交说明')],fields=>enqueue('checkpoint',{...fields,scope:'shared',fingerprint:preview.fingerprint}),JSON.stringify(preview,null,2));}),teamAction('合并成员分支',()=>{closeTeam();openDialog('选择成员',[field('username','用户名','select','',info.members)],async fields=>{const latest=await rpc('project_status'),source=latest.branches['users/'+fields.username];if(!source)throw Error('成员尚未发布分支');return()=>openDialog('确认合并',[],()=>enqueue('merge',{...fields,scope:'shared',source,target:latest.head,confirmed:true}),`来源 ${source}\n目标 ${latest.head}\n冲突时保留原目标。`);});}));body.append(actions);
  for(const [name,sha] of Object.entries(info.branches))body.append(teamRow(name,sha));
 } else {
  if(info.blocked){body.append(teamNote('任务中断后需要检查文件再恢复队列。'));if(owner)body.append(teamAction('检查并恢复队列',async()=>{const preview=await rpc('diff',{scope:'shared'});closeTeam();openDialog('恢复队列',[],()=>rpc('recover',{fingerprint:preview.fingerprint,confirmed:true}),JSON.stringify(preview,null,2));}));}
  const labels={queued:'排队中',running:'执行中',completed:'已完成',failed:'失败',interrupted:'已中断',cancelled:'已取消'};
  body.append(el('h3','最近任务'));if(!info.jobs.length)body.append(teamNote('还没有任务'));
  for(const job of info.jobs.slice(-20).reverse()){const row=teamRow(job.actor+' · '+job.op,(labels[job.status]||job.status)+(job.scope==='personal'?' · 个人会话':' · 共同会话'));if(job.result?.error)row.firstChild.append(el('small',job.result.error));body.append(row);}
  body.append(el('h3','操作记录'));if(!info.audit?.length)body.append(teamNote('还没有操作记录'));
  const operations={invite:'邀请成员',remove:'移除成员',approve:'审批操作',provider_save:'配置提供商',provider_use:'切换提供商',model:'切换模型',reasoning:'调整推理强度',recover:'恢复队列',stop:'停止任务'};
  for(const entry of (info.audit||[]).slice(-20).reverse())body.append(teamRow(entry.actor+' · '+(operations[entry.op]||entry.op),new Date(entry.time*1000).toLocaleString()));
 }
}

initialize().catch(e => notice(e.message));

})();
