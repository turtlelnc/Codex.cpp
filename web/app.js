'use strict';
const $ = id => document.getElementById(id);
const state = {user:null, project:null, scope:'shared', cursor:0, events:[], status:null, pending:null, polling:false, generation:0};
function el(tag, text, cls) {const node=document.createElement(tag); if(text!==undefined)node.textContent=text; if(cls)node.className=cls; return node;}
async function http(path, body) {
  const options=body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':state.user?.csrf||''},body:JSON.stringify(body)};
  const response=await fetch(path,options); const data=await response.json();
  if(response.status===401){state.user=null; $('workspace').hidden=true; $('login').hidden=false;}
  if(!response.ok)throw new Error(data.error||'请求失败'); return data;
}
async function rpc(op, values={}) {return (await http('/api/rpc',{op,project:state.project,...values})).result;}
function notice(message) {$('notice').textContent=message; $('notice').hidden=!message;}
function action(id, fn) {$(id).addEventListener('click',async()=>{try{await fn();}catch(e){notice(e.message);}});}
function field(name,label,type='text',value='',options=[]) {return {name,label,type,value,options};}
function dialog(title, fields, submit, preview='') {
  $('dialog-title').textContent=title; $('dialog-body').replaceChildren(); $('dialog-error').textContent='';
  for(const f of fields){const label=el('label',f.label);const input=el(f.type==='select'?'select':'input');input.name=f.name;
    if(f.type==='select')for(const value of f.options)input.append(new Option(value,value));else input.type=f.type;
    input.value=f.value;input.required=f.type!=='password';if(f.type==='password')input.autocomplete='new-password';label.append(input);$('dialog-body').append(label);}
  if(preview)$('dialog-body').append(el('pre',preview));
  $('dialog-form').onsubmit=async event=>{event.preventDefault();$('dialog-submit').disabled=true;try{await submit(Object.fromEntries(new FormData(event.target)));for(const input of $('dialog-body').querySelectorAll('input[type=password]'))input.value='';$('dialog').close();notice('');await refresh();}catch(e){$('dialog-error').textContent=e.message;}finally{$('dialog-submit').disabled=false;}};
  $('dialog').showModal();
}
action('dialog-close',()=>$('dialog').close());action('dialog-cancel',()=>$('dialog').close());
$('login-form').onsubmit=async event=>{event.preventDefault();$('login-error').textContent='';try{await http('/api/login',{username:$('username').value,password:$('password').value});$('password').value='';await initialize();}catch(e){$('login-error').textContent=e.message;}};
async function initialize(){
  try{state.user=await http('/api/me');}catch(e){
    if(!state.user){const me=await fetch('/api/me').then(r=>r.json());if(me.mode==='local'){$('username').closest('label').hidden=true;$('login-note').textContent='本地模式：输入启动时提示的文件中的登录令牌。';$('password-label').firstChild.textContent='本地令牌';}return;}throw e;
  }
  state.project=null;state.generation++;state.cursor=0;state.events=[];state.status=null;
  $('login').hidden=true;$('workspace').hidden=false;$('identity').textContent=state.user.username;
  const local=state.user.mode==='local';$('mode-label').textContent=local?'LOCAL WORKSPACE':'SERVER WORKSPACE';$('scope-controls').hidden=local;$('create-project').hidden=local;$('sessions-button').hidden=!local;
  for(const id of ['checkpoint','publish','sync','merge','team-panel'])$(id).hidden=local;
  document.querySelector('[data-panel="team"]').hidden=local;
  $('composer-hint').textContent=local?'在本机工作区执行':'在服务器工作区执行 · 任务依次排队';
  await projects();
}
async function projects(){const data=await rpc('list');$('projects').replaceChildren();for(const name of data.projects){const b=el('button',name,name===state.project?'selected':'');b.onclick=()=>select(name).catch(e=>notice(e.message));$('projects').append(b);}if(!state.project&&data.projects.length)await select(data.projects[0]);}
async function select(name){state.scope='shared';$('shared-tab').setAttribute('aria-pressed','true');$('personal-tab').setAttribute('aria-pressed','false');state.project=name;state.generation++;state.cursor=0;state.events=[];state.status=null;state.pending=null;$('project-title').textContent=name;$('file-content').textContent='';$('file-title').textContent='';$('diff-content').textContent='点击刷新查看当前修改。';notice('');await projects();await refresh();await files();restorePending();}
function message(role,text,byline){const n=el('div',undefined,'message '+role);n.append(el('span',byline,'byline'),document.createTextNode(text));return n;}
function drawMessages(items){const box=$('messages');const signature=items.map(n=>n.textContent).join('\u0000');if(box._signature===signature)return;box._signature=signature;const near=box.scrollHeight-box.scrollTop-box.clientHeight<100;box.replaceChildren(...items);if(!items.length){const empty=el('div',undefined,'empty');empty.append(el('h2','从一个任务开始'),el('p','描述目标、约束和期望的结果。'));box.append(empty);}if(near)box.scrollTop=box.scrollHeight;}
function renderEvents(){const nodes=[];let draft='';let actor='Agent';
  const flush=()=>{if(draft){nodes.push(message('assistant',draft,'Codex'));draft='';}};
  for(const entry of state.events){if((entry.scope==='personal') !== (state.scope==='personal'))continue;
    if(entry.type==='task.queued'&&entry.text){flush();nodes.push(message('user',entry.text,entry.actor));}
    if(entry.type==='agent.event'){const event=entry.event,p=event.payload||{};actor=entry.actor;
      if(event.type==='model.delta')draft+=p.text||p.delta||'';
      else if(event.type==='model.output'){const text=p.text||draft;draft='';if(text)nodes.push(message('assistant',text,'Codex'));}
      else if(event.type==='tool.started'||event.type==='tool.completed'){flush();nodes.push(message('event',(p.tool||'工具')+' · '+event.type+(p.output?'\n'+p.output:''),actor));}
      else if(event.type==='error'){flush();nodes.push(message('event',p.message||JSON.stringify(p),'执行错误'));}}
    if(entry.type==='task.completed'&&!['send','personal_send','login','personal_login'].includes(entry.kind)){flush();const result=entry.result||{};const text=result.merged===false?'合并已中止，原有修改已保留。冲突文件：\n'+(result.conflicts||[]).join('\n')+'\n请在个人会话中让 Agent 合并 FETCH_HEAD、解决冲突并提交，然后重新发布和合并。':JSON.stringify(result,null,2);nodes.push(message('event',text,entry.actor+' · '+entry.kind));}
    if(entry.type==='task.failed'||entry.type==='task.interrupted'){flush();nodes.push(message('event',entry.result?.error||'任务中断，需要检查后恢复',entry.actor));}
    if(entry.type==='approval.decided'){flush();nodes.push(message('event',entry.allow?'批准执行':'拒绝执行',entry.actor));}
    if(entry.type==='login.notice')nodes.push(message('event',entry.text,'设备登录'));
  }flush();drawMessages(nodes);
}
async function refresh(){if(!state.user||!state.project||state.polling)return;state.polling=true;const generation=state.generation;
  try{
    if(state.user.mode==='local'){
      const snap=await rpc('snapshot');if(generation!==state.generation)return;
      state.pending=snap.pending?.call_id||null;
      const nodes=snap.messages.map(m=>message(m.role,m.content,m.role==='user'?'你':'Codex'));
      if(snap.busy){const draft=snap.events.filter(e=>e.type==='model.delta').map(e=>e.payload?.text||'').join('');if(draft)nodes.push(message('assistant',draft,'Codex · 正在生成'));}
      for(const e of snap.events.filter(e=>e.type==='login.notice'))nodes.push(message('event',e.payload.text,'设备登录'));
      drawMessages(nodes);$('queue-state').textContent=snap.busy?'正在执行':'就绪';$('send').disabled=snap.busy;
      approval(snap.pending?JSON.stringify(snap.pending,null,2):'');if(snap.error)notice(snap.error);
    }else{
      const [events,status]=await Promise.all([rpc('events',{after:state.cursor}),rpc('status')]);if(generation!==state.generation)return;
      state.cursor=events.cursor;
      // The server labels personal events; retain scope across later event frames.
      for(const event of events.events){if(event.kind==='personal_send')event.private=true;const previous=state.events.find(e=>e.job===event.job&&e.private);if(previous)event.private=true;state.events.push(event);}
      state.events=state.events.slice(-2000);state.status=status;state.pending=status.approval;
      const jobsSignature=status.jobs.map(j=>j.job+j.status).join();if(jobsSignature!==state.jobsSignature){state.jobsSignature=jobsSignature;if(!$('changes-panel').hidden)diff().catch(e=>notice(e.message));}
      renderEvents();const pendingEvent=[...state.events].reverse().find(e=>e.type==='approval.pending'&&e.approval===state.pending);
      const details=status.approval_details||pendingEvent?.tool;approval(details?JSON.stringify(details,null,2):'');$('allow').disabled=!!state.pending&&!details;
      $('queue-state').textContent=status.blocked?'需要恢复':`${status.jobs.filter(j=>j.status==='queued').length} 个排队任务`;
      $('jobs').replaceChildren(...status.jobs.map(j=>el('div',`${j.actor} · ${j.kind} · ${j.status}`)));
      $('owner').textContent='项目创建者：'+status.owner;const owner=state.user.username===status.owner;
      $('invite').hidden=!owner;$('recover').hidden=!owner||!status.blocked;
      $('members').replaceChildren(...status.members.map(name=>{const row=el('div',name);if(owner&&name!==status.owner){const b=el('button','移除');b.onclick=()=>dialog('移除成员',[],()=>rpc('remove',{username:name}),'移除 '+name+' 后，将立即撤销项目访问。');row.append(b);}return row;}));
    }
    $('connection').textContent='已连接';
  }catch(e){$('connection').textContent='连接异常';notice(e.message);}finally{state.polling=false;}
}
function approval(details){$('approval').hidden=!state.pending;$('approval-details').textContent=details||'审批 '+(state.pending||'');}
function pendingSubmission(){try{return JSON.parse(sessionStorage.getItem('codex.pending')||'null');}catch{return null;}}
function restorePending(){const pending=pendingSubmission();if(pending&&pending.user===state.user?.username&&pending.project===state.project){$('prompt').value=pending.text;notice('上次提交结果未确认。切到原会话范围后再次发送相同内容，可安全重试；任务不会重复执行。');}}
$('composer').onsubmit=async event=>{event.preventDefault();const text=$('prompt').value;if(!text.trim())return;
 const previous=pendingSubmission();const retry=previous&&previous.user===state.user.username&&previous.project===state.project&&previous.scope===state.scope&&previous.text===text;
 const pending=retry?previous:{user:state.user.username,project:state.project,scope:state.scope,text,request_id:crypto.randomUUID().replaceAll('-','')};
 sessionStorage.setItem('codex.pending',JSON.stringify(pending));$('send').disabled=true;
 try{await rpc(state.scope==='personal'?'personal_send':'send',{text,request_id:pending.request_id});sessionStorage.removeItem('codex.pending');$('prompt').value='';notice('');await refresh();}
 catch(e){notice(e.message+'。保留相同内容再次发送可使用原请求 ID 安全重试。');}
 finally{$('send').disabled=false;}};

for(const [id,allow] of [['allow',true],['deny',false]])action(id,async()=>{await rpc('approve',{approval:state.pending,allow});await refresh();});
action('logout',async()=>{await http('/api/logout',{});sessionStorage.removeItem('codex.pending');location.reload();});
action('create-project',()=>dialog('创建项目',[field('project','项目名称'),field('repository','HTTPS Git 仓库（可留空）')],async data=>{await rpc('create',data);state.project=null;await projects();}));
// Repository is optional, while project names remain required.
$('create-project').addEventListener('click',()=>{const input=$('dialog-body').querySelector('[name="repository"]');if(input)input.required=false;});
async function files(){if(!state.project)return;const generation=state.generation,scope=state.scope;const result=await rpc(scope==='personal'?'personal_files':'files');if(generation!==state.generation||scope!==state.scope)return;$('file-list').replaceChildren(...result.files.map(path=>{const b=el('button',path);b.onclick=async()=>{try{const generation=state.generation,scope=state.scope;const result=await rpc(scope==='personal'?'personal_read':'read',{path});if(generation!==state.generation||scope!==state.scope)return;$('file-title').textContent=path;$('file-content').textContent=result.text;}catch(e){notice(e.message);}};return b;}));}
action('refresh-files',files);
async function diff(){const generation=state.generation,scope=state.scope;const result=await rpc(scope==='personal'?'personal_diff':'diff');if(generation!==state.generation||scope!==state.scope)return result;$('diff-content').textContent=(result.diff||'无已跟踪文件差异。')+(result.untracked?.length?'\n\n未跟踪文件：\n'+result.untracked.join('\n'):'')+(result.truncated?'\n[内容已截断]':'');return result;}
action('refresh-diff',diff);
for(const b of document.querySelectorAll('[data-panel]'))b.onclick=()=>{for(const tab of document.querySelectorAll('[data-panel]')){tab.setAttribute('aria-pressed',String(tab===b));$(tab.dataset.panel+'-panel').hidden=tab!==b;}if(b.dataset.panel==='changes')diff().catch(e=>notice(e.message));};
action('personal-tab',async()=>{await rpc('personal');state.scope='personal';$('personal-tab').setAttribute('aria-pressed','true');$('shared-tab').setAttribute('aria-pressed','false');await files();renderEvents();restorePending();});
action('shared-tab',async()=>{state.scope='shared';$('shared-tab').setAttribute('aria-pressed','true');$('personal-tab').setAttribute('aria-pressed','false');await files();renderEvents();});
action('invite',()=>dialog('邀请项目成员',[field('username','服务器上已开通的用户名')],data=>rpc('invite',data)));
action('checkpoint',async()=>{const preview=await rpc('diff');dialog('提交共同工作区修改',[field('message','提交说明')],data=>rpc('checkpoint',{...data,fingerprint:preview.fingerprint,request_id:crypto.randomUUID()}),JSON.stringify(preview,null,2));});
for(const op of ['publish','sync'])action(op,()=>dialog(op==='publish'?'发布个人分支':'同步共同分支',op==='publish'?[field('message','提交说明')]:[],data=>rpc(op,{...data,request_id:crypto.randomUUID()}),op==='publish'?'提交个人工作区并发布到个人分支。共同分支不会自动更新。':'将共同分支合入个人工作区；冲突时中止，保留原有内容。'));
action('merge',()=>dialog('选择要合并的成员',[field('username','成员','select','',state.status?.members||[])],async data=>{const status=await rpc('status');const source=status.branches['users/'+status.member_ids[data.username]],target=status.shared.head;if(!source)throw Error('该成员尚未发布个人分支');
  // Replace the first dialog only after its submit finishes.
  setTimeout(()=>dialog('确认分支合并',[],()=>rpc('merge',{username:data.username,source,target,confirmed:true,request_id:crypto.randomUUID()}),`成员：${data.username}\n来源：${source}\n目标：${target}\n\n冲突将中止合并；确认后不覆盖他人修改。`),0);
}));
action('recover',async()=>{const preview=await rpc('diff');dialog('检查后恢复项目',[],()=>rpc('recover',{confirmed:true,fingerprint:preview.fingerprint}),JSON.stringify(preview,null,2)+'\n中断任务不会重放；仅恢复后续队列。');});
action('settings-button',()=>{
  const local=state.user.mode==='local';
  const owner=state.status?.owner===state.user.username;
  const fields=[field('scope','凭据范围','select',local?'本地':state.scope==='personal'?'个人':'项目',local?['本地']:owner?['个人','项目']:['个人']),field('provider','模型提供商','select','codex',['codex','openai','deepseek','custom']),field('model','模型（可留空）'),field('base_url','自定义服务地址（可留空）'),field('api','API 类型','select','responses',['responses','chat']),field('key','API 密钥（留空则不用密钥；不会回显）','password')];
  dialog('模型与凭据',fields,async data=>{const settings={provider:data.provider},secrets={};if(data.model)settings.model=data.model;if(data.provider==='custom'){settings.base_url=data.base_url;settings.api=data.api;settings.api_key_env='MY_API_KEY';}if(data.provider!=='codex'){if(data.key)secrets[data.provider==='custom'?'MY_API_KEY':data.provider==='deepseek'?'DEEPSEEK_API_KEY':'OPENAI_API_KEY']=data.key;else settings.no_api_key=true;}
    await rpc(data.scope==='个人'?'personal_configure':'configure',{settings,secrets});if(data.provider==='codex')await rpc(data.scope==='个人'?'personal_login':'login',{request_id:crypto.randomUUID()});
  });for(const name of ['model','base_url'])$('dialog-body').querySelector(`[name="${name}"]`).required=false;
});
action('sessions-button',async()=>{const snap=await rpc('snapshot');dialog('切换本地会话',[field('session_id','会话','select','新建',['新建',...snap.sessions.map(s=>s.id)])],data=>rpc('session',{session_id:data.session_id==='新建'?null:data.session_id}));});
initialize().catch(e=>notice(e.message));setInterval(()=>refresh(),1000);
