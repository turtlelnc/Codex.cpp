'use strict';
(() => {

/* ---------- 基础 ---------- */

const $ = id => document.getElementById(id);
const newId = () => crypto.randomUUID().replaceAll('-', '');
const state = {
  user: null, project: null, scope: 'shared',
  cursor: 0, events: [], status: null, pending: null,
  polling: false, generation: 0, jobsSignature: '',
};

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

// 绑定按钮点击；异常统一显示为顶部提示。
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
  if (response.status === 401) {
    state.user = null;
    $('workspace').hidden = true;
    $('login').hidden = false;
  }
  if (!response.ok) throw new Error(data.error || '请求失败');
  return data;
}

async function rpc(op, values = {}) {
  return (await http('/api/rpc', {op, project: state.project, ...values})).result;
}

const scoped = (shared, personal) => state.scope === 'personal' ? personal : shared;

/* ---------- 对话框 ---------- */

function field(name, label, type = 'text', value = '', options = [], required = type !== 'password') {
  return {name, label, type, value, options, required};
}

// submit 可返回一个函数，对话框关闭后再调用（用于两步确认）。
function openDialog(title, fields, submit, preview = '') {
  $('dialog-title').textContent = title;
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

/* ---------- 登录与初始化 ---------- */

$('login-form').onsubmit = async event => {
  event.preventDefault();
  $('login-error').textContent = '';
  try {
    await http('/api/login', {username: $('username').value, password: $('password').value});
    $('password').value = '';
    await initialize();
  } catch (e) {
    $('login-error').textContent = e.message;
  }
};

async function initialize() {
  try {
    state.user = await http('/api/me');
  } catch (e) {
    if (state.user) throw e;
    const me = await fetch('/api/me').then(r => r.json());
    if (me.mode === 'local') {
      $('username').closest('label').hidden = true;
      $('login-note').textContent = '本地模式：输入启动时提示的文件中的登录令牌。';
      $('password-label').firstElementChild.textContent = '本地令牌';
    }
    return;
  }
  resetProjectState(null);
  $('login').hidden = true;
  $('workspace').hidden = false;
  $('identity').textContent = state.user.username;
  const local = state.user.mode === 'local';
  document.body.dataset.mode = local ? 'local' : 'server';
  $('mode-label').textContent = local ? 'LOCAL WORKSPACE' : 'SERVER WORKSPACE';
  $('scope-controls').hidden = local;
  $('create-project').hidden = local;
  $('sidebar-heading-label').textContent = local ? '最近' : '工作区';
  $('new-chat').hidden = !local;
  $('permission-chip').hidden = !local;
  $('model-chip').textContent = local ? '默认模型' : '模型';
  $('sessions-button').hidden = !local;
  for (const id of ['checkpoint', 'publish', 'sync', 'merge', 'team-panel']) $(id).hidden = local;
  document.querySelector('[data-panel="team"]').hidden = local;
  await loadProjects();
}

function resetProjectState(project) {
  state.project = project;
  state.generation++;
  state.cursor = 0;
  state.events = [];
  state.status = null;
  state.pending = null;
}

/* ---------- 项目 ---------- */

async function loadProjects() {
  const data = await rpc('list');
  if (state.user.mode === 'local') {
    if (!state.project && data.projects.length) await selectProject(data.projects[0]);
    return;
  }
  $('projects').replaceChildren(...data.projects.map(name => {
    const button = el('button', name, name === state.project ? 'selected' : '');
    if (name === state.project) button.setAttribute('aria-current', 'true');
    button.onclick = () => selectProject(name).catch(e => notice(e.message));
    return button;
  }));
  if (!state.project && data.projects.length) await selectProject(data.projects[0]);
}

function setScope(scope) {
  if (scope !== state.scope) { $('file-list').replaceChildren(); $('file-content').textContent = ''; $('file-title').textContent = ''; }
  state.scope = scope;
  $('shared-tab').setAttribute('aria-pressed', String(scope === 'shared'));
  $('personal-tab').setAttribute('aria-pressed', String(scope === 'personal'));
}

async function selectProject(name) {
  setScope('shared');
  resetProjectState(name);
  $('project-title').textContent = name;
  $('file-content').textContent = '';
  $('file-title').textContent = '';
  $('diff-content').textContent = '点击刷新查看当前修改。';
  notice('');
  await loadProjects();
  await refresh();
  await loadFiles();
  restorePending();
}

/* ---------- 消息渲染 ---------- */

function message(role, text, byline) {
  const node = el('div', undefined, 'message ' + role);
  node._text = text;
  node.append(el('span', byline, 'byline'), document.createTextNode(text));
  if (role === 'assistant') {
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

function drawMessages(items) {
  const box = $('messages');
  const signature = items.map(n => n.textContent).join('\u0000');
  if (box._signature === signature) return;
  box._signature = signature;
  const open = [...box.querySelectorAll('details')].map(d => d.open);
  const nearBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 100;
  box.replaceChildren(...items);
  box.querySelectorAll('details').forEach((d, i) => { d.open = !!open[i]; });
  if (!items.length) {
    const empty = el('div', undefined, 'empty');
    empty.append(el('h2', '从一个任务开始'), el('p', '描述目标、约束和期望的结果。'));
    box.append(empty);
  }
  if (nearBottom) box.scrollTop = box.scrollHeight;
}

const SESSION_KINDS = ['send', 'personal_send', 'login', 'personal_login'];

function mergeAbortedText(result) {
  return '合并已中止，原有修改已保留。冲突文件：\n' + (result.conflicts || []).join('\n') +
    '\n请在个人会话中让 Agent 合并 FETCH_HEAD、解决冲突并提交，然后重新发布和合并。';
}

function renderEvents() {
  const nodes = [];
  let steps = [];
  let draft = '';
  let actor = 'Agent';
  const push = node => { closeSteps(); nodes.push(node); };
  const closeSteps = () => {
    if (!steps.length) return;
    const group = el('details', undefined, 'steps');
    group.append(el('summary', `已处理 ${steps.length} 个步骤`), ...steps);
    nodes.push(group);
    steps = [];
  };
  const flush = () => {
    if (draft) push(message('assistant', draft, 'Codex'));
    draft = '';
  };
  for (const entry of state.events) {
    if ((entry.scope === 'personal') !== (state.scope === 'personal')) continue;
    if (entry.type === 'task.queued' && entry.text) {
      flush();
      push(message('user', entry.text, entry.actor));
    }
    if (entry.type === 'agent.event') {
      const event = entry.event, p = event.payload || {};
      actor = entry.actor;
      if (event.type === 'model.delta') draft += p.text || p.delta || '';
      else if (event.type === 'model.output') {
        const text = p.text || draft;
        draft = '';
        if (text) push(message('assistant', text, 'Codex'));
      } else if (event.type === 'tool.started' || event.type === 'tool.completed') {
        flush();
        steps.push(message('event', (p.tool || '工具') + ' · ' + event.type + (p.output ? '\n' + p.output : ''), actor));
      } else if (event.type === 'error') {
        flush();
        push(message('event error', p.message || JSON.stringify(p), '执行错误'));
      }
    }
    if (entry.type === 'task.completed' && !SESSION_KINDS.includes(entry.kind)) {
      flush();
      const result = entry.result || {};
      const text = result.merged === false ? mergeAbortedText(result) : JSON.stringify(result, null, 2);
      push(message('event', text, entry.actor + ' · ' + entry.kind));
    }
    if (entry.type === 'task.failed' || entry.type === 'task.interrupted') {
      flush();
      push(message('event error', entry.result?.error || '任务中断，需要检查后恢复', entry.actor));
    }
    if (entry.type === 'approval.decided') {
      flush();
      push(message('event', entry.allow ? '批准执行' : '拒绝执行', entry.actor));
    }
    if (entry.type === 'login.notice') push(message('event', entry.text, '设备登录'));
  }
  flush();
  closeSteps();
  drawMessages(nodes);
}

function showApproval(details) {
  $('approval').hidden = !state.pending;
  $('approval-details').textContent = details || '审批 ' + (state.pending || '');
}

/* ---------- 轮询刷新 ---------- */

function renderSessions(sessions, busy) {
  const signature = JSON.stringify(sessions.map(s => [s.id, s.title, s.selected])) + busy;
  if ($('projects')._signature === signature) return;
  $('projects')._signature = signature;
  $('projects').replaceChildren(...sessions.map(s => {
    const button = el('button', s.title || '空会话', s.selected ? 'selected' : '');
    button.title = s.title || '';
    if (s.selected) button.setAttribute('aria-current', 'true');
    button.disabled = busy && !s.selected;
    button.onclick = async () => {
      try { await rpc('session', {session_id: s.id}); await refresh(); } catch (e) { notice(e.message); }
    };
    return button;
  }));
}

async function refreshLocal(generation) {
  const snap = await rpc('snapshot');
  if (generation !== state.generation) return;
  state.pending = snap.pending?.call_id || null;
  const nodes = snap.messages.map(m => message(m.role, m.content, m.role === 'user' ? '你' : 'Codex'));
  if (snap.busy) {
    const draft = snap.events.filter(e => e.type === 'model.delta').map(e => e.payload?.text || '').join('');
    if (draft) nodes.push(message('assistant', draft, 'Codex · 正在生成'));
  }
  for (const e of snap.events.filter(e => e.type === 'login.notice')) {
    nodes.push(message('event', e.payload.text, '设备登录'));
  }
  drawMessages(nodes);
  renderSessions(snap.sessions || [], snap.busy);
  updateChips(snap);
  setQueueState(snap.busy ? '正在执行' : '就绪', snap.busy);
  $('send').disabled = snap.busy;
  showApproval(snap.pending ? JSON.stringify(snap.pending, null, 2) : '');
  if (snap.error) notice(snap.error);
}

function ingestEvents(events) {
  state.cursor = events.cursor;
  // 服务器只在入队事件上标记个人范围；后续事件按 job 继承。
  for (const event of events.events) {
    if (event.kind === 'personal_send') event.private = true;
    if (state.events.some(e => e.job === event.job && e.private)) event.private = true;
    state.events.push(event);
  }
  state.events = state.events.slice(-2000);
}

function renderTeam(status) {
  const owner = state.user.username === status.owner;
  $('owner').textContent = '项目创建者：' + status.owner;
  $('invite').hidden = !owner;
  $('recover').hidden = !owner || !status.blocked;
  $('members').replaceChildren(...status.members.map(name => {
    const row = el('div', name);
    if (owner && name !== status.owner) {
      const remove = el('button', '移除');
      remove.onclick = () => openDialog('移除成员', [], () => rpc('remove', {username: name}),
        '移除 ' + name + ' 后，将立即撤销项目访问。');
      row.append(remove);
    }
    return row;
  }));
}

function setQueueState(text, busy) {
  $('queue-state').textContent = text;
  $('queue-state').dataset.busy = busy ? 'true' : 'false';
}

async function refreshShared(generation) {
  const [events, status] = await Promise.all([rpc('events', {after: state.cursor}), rpc('status')]);
  if (generation !== state.generation) return;
  ingestEvents(events);
  state.status = status;
  state.pending = status.approval;

  const jobsSignature = status.jobs.map(j => j.job + j.status).join();
  if (jobsSignature !== state.jobsSignature) {
    state.jobsSignature = jobsSignature;
    if (!$('changes-panel').hidden) loadDiff().catch(e => notice(e.message));
  }

  renderEvents();
  const pendingEvent = [...state.events].reverse().find(e => e.type === 'approval.pending' && e.approval === state.pending);
  const details = status.approval_details || pendingEvent?.tool;
  showApproval(details ? JSON.stringify(details, null, 2) : '');
  $('allow').disabled = !!state.pending && !details;

  const queued = status.jobs.filter(j => j.status === 'queued').length;
  setQueueState(status.blocked ? '需要恢复' : `${queued} 个排队任务`, status.blocked || queued > 0);
  $('jobs').replaceChildren(...status.jobs.map(j => el('div', `${j.actor} · ${j.kind} · ${j.status}`)));
  renderTeam(status);
}

async function refresh() {
  if (!state.user || !state.project || state.polling) return;
  state.polling = true;
  const generation = state.generation;
  try {
    if (state.user.mode === 'local') await refreshLocal(generation);
    else await refreshShared(generation);
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

function restorePending() {
  const pending = pendingSubmission();
  if (pending && pending.user === state.user?.username && pending.project === state.project) {
    $('prompt').value = pending.text;
    notice('上次提交结果未确认。切到原会话范围后再次发送相同内容，可安全重试；任务不会重复执行。');
  }
}

async function submitTask(event) {
  event.preventDefault();
  const text = $('prompt').value;
  if (!text.trim()) return;
  if (tryCommand(text)) return;
  const previous = pendingSubmission();
  const retry = previous && previous.user === state.user.username && previous.project === state.project &&
    previous.scope === state.scope && previous.text === text;
  const pending = retry ? previous : {
    user: state.user.username, project: state.project, scope: state.scope, text, request_id: newId(),
  };
  sessionStorage.setItem('codex.pending', JSON.stringify(pending));
  $('send').disabled = true;
  try {
    await rpc(scoped('send', 'personal_send'), {text, request_id: pending.request_id});
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

/* ---------- 审批 / 登出 ---------- */

for (const [id, allow] of [['allow', true], ['deny', false]]) {
  onClick(id, async () => { await rpc('approve', {approval: state.pending, allow}); await refresh(); });
}

onClick('logout', async () => {
  await http('/api/logout', {});
  sessionStorage.removeItem('codex.pending');
  location.reload();
});

/* ---------- 文件与 Diff ---------- */

async function loadFiles() {
  if (!state.project) return;
  const generation = state.generation, scope = state.scope;
  const stale = () => generation !== state.generation || scope !== state.scope;
  const result = await rpc(scoped('files', 'personal_files'));
  if (stale()) return;
  $('file-list').replaceChildren(...result.files.map(path => {
    const button = el('button', path);
    button.onclick = async () => {
      try {
        const read = await rpc(scoped('read', 'personal_read'), {path});
        if (stale()) return;
        $('file-title').textContent = path;
        $('file-content').textContent = read.text;
      } catch (e) { notice(e.message); }
    };
    return button;
  }));
}

function renderDiff(text) {
  const box = $('diff-content');
  box.replaceChildren();
  const lines = text.split('\n');
  lines.forEach((line, index) => {
    let cls = '';
    if (line.startsWith('+++') || line.startsWith('---') || line.startsWith('diff ') || line.startsWith('index ')) cls = 'diff-meta';
    else if (line.startsWith('@@')) cls = 'diff-hunk';
    else if (line.startsWith('+')) cls = 'diff-add';
    else if (line.startsWith('-')) cls = 'diff-del';
    box.append(el('span', line + (index < lines.length - 1 ? '\n' : ''), cls));
  });
}

function showDiffStat(diff) {
  let add = 0, del = 0;
  for (const line of (diff || '').split('\n')) {
    if (line.startsWith('+') && !line.startsWith('+++')) add++;
    else if (line.startsWith('-') && !line.startsWith('---')) del++;
  }
  $('diff-stat').textContent = add || del ? ` +${add} −${del}` : '';
}

async function loadDiff() {
  const generation = state.generation, scope = state.scope;
  const result = await rpc(scoped('diff', 'personal_diff'));
  if (generation !== state.generation || scope !== state.scope) return result;
  showDiffStat(result.diff);
  renderDiff((result.diff || '无已跟踪文件差异。') +
    (result.untracked?.length ? '\n\n未跟踪文件：\n' + result.untracked.join('\n') : '') +
    (result.truncated ? '\n[内容已截断]' : ''));
  return result;
}

onClick('refresh-files', loadFiles);
onClick('refresh-diff', loadDiff);

for (const tab of document.querySelectorAll('[data-panel]')) {
  tab.onclick = () => {
    for (const other of document.querySelectorAll('[data-panel]')) {
      other.setAttribute('aria-pressed', String(other === tab));
      $(other.dataset.panel + '-panel').hidden = other !== tab;
    }
    if (tab.dataset.panel === 'changes') loadDiff().catch(e => notice(e.message));
  };
}

/* ---------- 会话范围 ---------- */

onClick('personal-tab', async () => {
  await rpc('personal');
  setScope('personal');
  await loadFiles();
  renderEvents();
  restorePending();
});

onClick('shared-tab', async () => {
  setScope('shared');
  await loadFiles();
  renderEvents();
});

/* ---------- 项目 / 成员 / 分支操作 ---------- */

onClick('create-project', () => openDialog('创建项目',
  [field('project', '项目名称'), field('repository', 'HTTPS Git 仓库（可留空）', 'text', '', [], false)],
  async data => { await rpc('create', data); state.project = null; await loadProjects(); }));

onClick('invite', () => openDialog('邀请项目成员',
  [field('username', '服务器上已开通的用户名')], data => rpc('invite', data)));

onClick('checkpoint', async () => {
  const preview = await rpc('diff');
  openDialog('提交共同工作区修改', [field('message', '提交说明')],
    data => rpc('checkpoint', {...data, fingerprint: preview.fingerprint, request_id: newId()}),
    JSON.stringify(preview, null, 2));
});

const BRANCH_OPS = {
  publish: {
    title: '发布个人分支', fields: [field('message', '提交说明')],
    note: '提交个人工作区并发布到个人分支。共同分支不会自动更新。',
  },
  sync: {
    title: '同步共同分支', fields: [],
    note: '将共同分支合入个人工作区；冲突时中止，保留原有内容。',
  },
};
for (const [op, spec] of Object.entries(BRANCH_OPS)) {
  onClick(op, () => openDialog(spec.title, spec.fields, data => rpc(op, {...data, request_id: newId()}), spec.note));
}

onClick('merge', () => openDialog('选择要合并的成员',
  [field('username', '成员', 'select', '', state.status?.members || [])],
  async data => {
    const status = await rpc('status');
    const source = status.branches['users/' + status.member_ids[data.username]];
    const target = status.shared.head;
    if (!source) throw Error('该成员尚未发布个人分支');
    return () => openDialog('确认分支合并', [],
      () => rpc('merge', {username: data.username, source, target, confirmed: true, request_id: newId()}),
      `成员：${data.username}\n来源：${source}\n目标：${target}\n\n冲突将中止合并；确认后不覆盖他人修改。`);
  }));

onClick('recover', async () => {
  const preview = await rpc('diff');
  openDialog('检查后恢复项目', [], () => rpc('recover', {confirmed: true, fingerprint: preview.fingerprint}),
    JSON.stringify(preview, null, 2) + '\n中断任务不会重放；仅恢复后续队列。');
});


/* ---------- 模型与权限 ---------- */

const PERMISSION_LABELS = {
  readonly: '只读',
  workspace: '默认权限',
  full: '完全访问',
};
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
  $('model-chip').textContent = (state.model || '默认模型') + (state.reasoning ? ' · ' + effortLabel(state.reasoning) : '');
  $('permission-chip').textContent = PERMISSION_LABELS[state.permission];
  $('permission-chip').dataset.level = state.permission;
  for (const id of ['model-chip', 'permission-chip']) $(id).disabled = snap.busy;
  if (snap.busy) closePopover();
}

/* 浮层选择器（与 Codex 一致：在输入框上方展开，不使用模态框） */

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

function drawModelPopover() {
  const box = $('popover');
  if (pop.kind !== 'model') return;
  const typed = box.querySelector('.pop-custom input')?.value || '';
  box.replaceChildren();
  const pick = value => async () => { closePopover(); await rpc('model', {model: value}); await refresh(); };
  box.append(el('div', '选择模型', 'pop-heading'));
  const refreshButton = el('button', '刷新模型列表'); refreshButton.type = 'button'; refreshButton.disabled = catalog.loading; refreshButton.onclick = () => loadCatalog(true); box.append(refreshButton);
  box.append(popItem('默认', '使用提供商的默认模型', !state.model, pick('')));
  const known = catalog.models || [];
  const list = state.model && !known.some(m => m.id === state.model) ? [...known, {id: state.model, description: '', efforts: []}] : known;
  for (const m of list) box.append(popItem(m.id, m.description, m.id === state.model, pick(m.id)));
  if (catalog.loading) box.append(el('div', '正在加载模型列表…', 'pop-note-plain'));
  else if (catalog.error) box.append(el('div', '无法获取模型列表：' + catalog.error, 'pop-note-plain'));

  const custom = el('form', undefined, 'pop-custom');
  const input = el('input');
  input.placeholder = '自定义模型名';
  input.setAttribute('aria-label', '自定义模型名');
  input.pattern = '[A-Za-z0-9._:/\\-]+';
  input.maxLength = 200;
  input.value = typed;
  custom.append(input, el('button', '应用'));
  custom.onsubmit = async event => {
    event.preventDefault();
    if (!input.value.trim()) return;
    try { await pick(input.value.trim())(); } catch (e) { notice(e.message); }
  };
  box.append(custom);

  const current = list.find(m => m.id === state.model);
  const efforts = current?.efforts?.length ? ['', ...current.efforts] : DEFAULT_EFFORTS;
  box.append(el('div', '推理强度', 'pop-heading'));
  const row = el('div', undefined, 'pop-efforts');
  for (const value of efforts) {
    const button = el('button', effortLabel(value));
    button.type = 'button';
    button.setAttribute('aria-pressed', String(value === state.reasoning));
    button.onclick = async () => {
      try { closePopover(); await rpc('reasoning', {effort: value}); await refresh(); } catch (e) { notice(e.message); }
    };
    row.append(button);
  }
  box.append(row);
  if ($('messages').querySelector('.message')) {
    box.append(el('div', '对话中途切换模型可能降低效果，建议新建对话。', 'pop-note'));
  }
  $('model-chip').setAttribute('aria-expanded', 'true');
  input.focus();
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
  const box = $('popover');
  box.replaceChildren();
  if (kind === 'model') {
    drawModelPopover();
    loadCatalog();
  } else {
    box.append(el('div', '执行权限', 'pop-heading'));
    for (const [mode, title, desc] of PERMISSION_CHOICES) {
      box.append(popItem(title, desc, mode === state.permission, async () => { closePopover(); await openPermissions(mode); }));
    }
    $('permission-chip').setAttribute('aria-expanded', 'true');
  }
  box.hidden = false;
}

function openModel(value) {
  if (state.user.mode !== 'local') return openSettings();
  if (value !== undefined) return rpc('model', {model: value}).then(refresh);
  openPopover('model');
}

async function openPermissions(choice) {
  if (state.user.mode !== 'local') return;
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

const localOnly = fn => (...args) => {
  if (state.user.mode !== 'local') throw new Error('该命令仅在本地模式可用');
  return fn(...args);
};

function showInspector(panel) {
  if ($('inspector-toggle').getAttribute('aria-pressed') !== 'true') $('inspector-toggle').click();
  document.querySelector(`[data-panel="${panel}"]`).click();
}

async function copyLast() {
  const last = [...$('messages').querySelectorAll('.message.assistant')].pop();
  if (!last) throw new Error('还没有可复制的回复');
  await navigator.clipboard.writeText(last._text);
  notice('已复制上一条回复');
}

function statusText() {
  const local = state.user.mode === 'local';
  return [
    `项目：${state.project || '-'}`,
    `模式：${local ? '本地' : '服务器'} · ${state.scope === 'personal' ? '个人会话' : '共同会话'}`,
    local ? `模型：${state.model || '默认'}` : null,
    local ? `权限：${PERMISSION_LABELS[state.permission] || '-'}` : null,
    `任务队列：${$('queue-state').textContent || '-'}`,
  ].filter(Boolean).join('\n');
}

const COMMANDS = [
  {name: '/model', desc: '切换模型（/model 名称）', takesArg: true, run: arg => openModel(arg || undefined)},
  {name: '/permissions', desc: '调整执行权限（readonly / default / full）', takesArg: true, run: localOnly(arg => {
    const alias = {readonly: 'readonly', 'read-only': 'readonly', default: 'workspace', workspace: 'workspace', full: 'full'};
    if (arg && !alias[arg]) throw new Error('可选：readonly、default、full');
    openPermissions(alias[arg]);
  })},
  {name: '/new', desc: '开始新对话', run: localOnly(newChat)},
  {name: '/sessions', desc: '切换本地会话', run: localOnly(openSessions)},
  {name: '/provider', desc: '模型提供商与密钥', run: () => openSettings()},
  {name: '/login', desc: 'ChatGPT 设备登录', run: async () => {
    await rpc(scoped('login', 'personal_login'), {request_id: newId()});
    notice('已发起设备登录，请查看对话中的登录提示');
  }},
  {name: '/status', desc: '显示当前配置', run: () => openDialog('当前状态', [], () => {}, statusText())},
  {name: '/diff', desc: '查看当前修改', run: () => showInspector('changes')},
  {name: '/files', desc: '浏览工作区文件', run: () => showInspector('files')},
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

const SECRET_NAMES = {custom: 'MY_API_KEY', deepseek: 'DEEPSEEK_API_KEY', openai: 'OPENAI_API_KEY'};

async function saveSettings(data) {
  const personal = data.scope === '个人';
  const settings = {provider: data.provider}, secrets = {};
  if (data.model) settings.model = data.model;
  if (data.provider === 'custom') {
    settings.base_url = data.base_url;
    settings.api = data.api;
    settings.api_key_env = SECRET_NAMES.custom;
  }
  if (data.provider !== 'codex') {
    if (data.key) secrets[SECRET_NAMES[data.provider]] = data.key;
    else settings.no_api_key = true;
  }
  await rpc(personal ? 'personal_configure' : 'configure', {settings, secrets});
  catalog.models = null;
  catalog.error = '';
  if (data.provider === 'codex') await rpc(personal ? 'personal_login' : 'login', {request_id: newId()});
}

function openSettings() {
  const local = state.user.mode === 'local';
  const owner = state.status?.owner === state.user.username;
  const scopes = local ? ['本地'] : owner ? ['个人', '项目'] : ['个人'];
  openDialog('模型与凭据', [
    field('scope', '凭据范围', 'select', local ? '本地' : state.scope === 'personal' ? '个人' : '项目', scopes),
    field('provider', '模型提供商', 'select', 'codex', ['codex', 'openai', 'deepseek', 'custom']),
    field('model', '模型（可留空）', 'text', '', [], false),
    field('base_url', '自定义服务地址（可留空）', 'text', '', [], false),
    field('api', 'API 类型', 'select', 'responses', ['responses', 'chat']),
    field('key', 'API 密钥（留空则不用密钥；不会回显）', 'password'),
  ], saveSettings);
}

onClick('settings-button', openSettings);

async function openSessions() {
  const snap = await rpc('snapshot');
  openDialog('切换本地会话',
    [field('session_id', '会话', 'select', '新建', ['新建', ...snap.sessions.map(s => s.id)])],
    data => rpc('session', {session_id: data.session_id === '新建' ? null : data.session_id}));
}

onClick('sessions-button', openSessions);

async function newChat() {
  await rpc('session', {session_id: null});
  $('prompt').focus();
  await refresh();
}
onClick('new-chat', newChat);

onClick('sidebar-toggle', () => {
  const open = $('sidebar-toggle').getAttribute('aria-pressed') !== 'true';
  $('sidebar-toggle').setAttribute('aria-pressed', String(open));
  $('workspace').classList.toggle('sidebar-collapsed', !open);
});

onClick('inspector-toggle', () => {
  const open = $('inspector-toggle').getAttribute('aria-pressed') !== 'true';
  $('inspector-toggle').setAttribute('aria-pressed', String(open));
  document.querySelector('.workspace-body').classList.toggle('no-inspector', !open);
});

/* ---------- 启动 ---------- */

initialize().catch(e => notice(e.message));
setInterval(() => refresh(), 1000);

})();
