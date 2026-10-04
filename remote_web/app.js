const $ = (id) => document.getElementById(id);
let token = sessionStorage.getItem('codexRemoteToken') || '';
let lastState = null;

async function api(path, body) {
  const response = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST',
    headers: { Authorization: `Bearer ${token}`, ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) },
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: 'no-store'
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
}

function render(state) {
  lastState = state;
  $('login').hidden = true;
  $('workspace').hidden = false;
  $('status').textContent = state.busy ? '运行中' : '已连接';
  const selector = $('session-select');
  selector.replaceChildren();
  const fresh = document.createElement('option');
  fresh.value = '';
  fresh.textContent = '新会话';
  selector.append(fresh);
  for (const session of state.sessions || []) {
    const option = document.createElement('option');
    option.value = session.id;
    option.textContent = `${session.title} · ${session.id.slice(-8)}`;
    selector.append(option);
  }
  selector.value = state.session_id || '';
  selector.disabled = state.busy;
  $('new-session').disabled = state.busy || !state.session_id;
  $('run-state').textContent = state.pending ? '等待批准' : state.busy ? 'Agent 正在处理' : '待命';
  $('send').disabled = state.busy;
  $('error').textContent = state.error || '';
  const messages = $('messages');
  messages.replaceChildren();
  for (const item of state.messages) {
    const block = document.createElement('article');
    block.className = `message ${item.role === 'user' ? 'user' : 'assistant'}`;
    const label = document.createElement('strong');
    label.textContent = item.role === 'user' ? '你' : 'Codex';
    const body = document.createElement('pre');
    body.textContent = item.content;
    block.append(label, body);
    messages.append(block);
  }
  const pending = state.pending;
  $('approval').hidden = !pending;
  if (pending) {
    $('approval-tool').textContent = `工具：${pending.tool}`;
    $('approval-args').textContent = pending.arguments || '参数尚未写入会话记录';
  }
  const events = $('events');
  events.replaceChildren();
  for (const item of state.events.slice(-30).reverse()) {
    const line = document.createElement('li');
    line.textContent = `${item.timestamp || ''} · ${item.type} · ${item.payload?.tool || item.payload?.status || ''}`;
    events.append(line);
  }
}

async function refresh() {
  if (!token) return;
  try {
    render(await api('/api/state'));
  } catch (error) {
    if (String(error).includes('unauthorized')) {
      token = '';
      sessionStorage.removeItem('codexRemoteToken');
      $('login').hidden = false;
      $('workspace').hidden = true;
      $('login-error').textContent = '访问令牌无效。';
    } else {
      $('status').textContent = '连接中断';
      $('error').textContent = String(error);
    }
  }
}

$('login-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  token = $('token').value.trim();
  try {
    render(await api('/api/state'));
    sessionStorage.setItem('codexRemoteToken', token);
    $('token').value = '';
    $('login-error').textContent = '';
  } catch (error) {
    $('login-error').textContent = String(error);
  }
});
$('prompt-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (lastState?.busy) return;
  const prompt = $('prompt').value;
  try {
    await api('/api/prompt', { prompt });
    $('prompt').value = '';
    await refresh();
  } catch (error) { $('error').textContent = String(error); }
});
$('session-select').addEventListener('change', async (event) => {
  try { await api('/api/session', { session_id: event.target.value || null }); await refresh(); }
  catch (error) { $('error').textContent = String(error); await refresh(); }
});
$('new-session').addEventListener('click', async () => {
  try { await api('/api/session', { session_id: null }); await refresh(); }
  catch (error) { $('error').textContent = String(error); }
});
for (const [id, allow] of [['allow', true], ['deny', false]]) {
  $(id).addEventListener('click', async () => {
    try { await api('/api/approval', { allow, call_id: lastState?.pending?.call_id }); await refresh(); }
    catch (error) { $('error').textContent = String(error); }
  });
}
setInterval(refresh, 1000);
refresh();
