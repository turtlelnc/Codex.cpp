// Native ABI / CLI integration, no network or paid models.
const {spawnSync}=require('node:child_process'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),assert=require('node:assert/strict');
const binary=path.resolve(process.argv[2]),manifest=path.resolve(process.argv[3]);
const root=fs.mkdtempSync(path.join(os.tmpdir(),'plugin-integration-'));
function run(args){const p=spawnSync(binary,args,{encoding:'utf8',timeout:30000,env:{...process.env,CODEX_CPP_HOME:path.join(root,'home')}});if(p.error)throw p.error;return p;}
try{
 let p=run(['plugin','inspect']);assert.equal(p.status,0,p.stderr);assert(JSON.parse(p.stdout).every(service=>service.owner.startsWith('builtin.')));
 p=run(['--plugin',manifest,'plugin','inspect']);assert.equal(p.status,0,p.stderr);const graph=JSON.parse(p.stdout);assert.equal(graph.find(s=>s.service==='model.request').owner,'demo-plugin');assert.deepEqual(graph.find(s=>s.service==='agent.turn').layers,['builtin.agent','demo-plugin']);
 const options=['--root',path.join(root,'project'),'--provider','custom','--api','chat','--model','plugin-demo','--base-url','http://127.0.0.1:9','--no-api-key','--json'];fs.mkdirSync(path.join(root,'project'));
 p=run(['--plugin',manifest,...options,'--','hello']);assert.equal(p.status,0,p.stderr);assert(p.stdout.includes('This reply came from a native module.'));assert(p.stdout.includes('model.delta'));
 p=run(['--plugin',manifest,...options,'--','run_plugin_tool']);assert.equal(p.status,0,p.stderr);assert(p.stdout.includes('tool.completed'));assert(p.stdout.includes('plugin_status'));
 p=run(['--plugin',manifest,...options,'--list-models']);assert.equal(p.status,0,p.stderr);assert.equal(JSON.parse(p.stdout)[0].id,'plugin-demo');
 // No arbitrary implicit overrides, wrong ABI, or out-of-bundle library paths.
 const original=JSON.parse(fs.readFileSync(manifest,'utf8'));
 const bundle=path.join(root,'bad');fs.mkdirSync(bundle);
 for(const file of fs.readdirSync(path.dirname(manifest)))if(fs.statSync(path.join(path.dirname(manifest),file)).isFile())fs.copyFileSync(path.join(path.dirname(manifest),file),path.join(bundle,file));
 const bad=path.join(bundle,'plugin.json');
 fs.writeFileSync(bad,JSON.stringify({...original,api:2}));p=run(['--plugin',bad,'plugin','inspect']);assert.equal(p.status,2);assert(p.stderr.includes('API 1'));
 const escaped=structuredClone(original);for(const key in escaped.libraries)escaped.libraries[key]='../'+escaped.libraries[key];fs.writeFileSync(bad,JSON.stringify(escaped));p=run(['--plugin',bad,'plugin','inspect']);assert.equal(p.status,2);assert(p.stderr.includes('escapes bundle'));
 const mutation=structuredClone(original);mutation.tools[0].mutates=true;fs.writeFileSync(bad,JSON.stringify(mutation));p=run(['--plugin',bad,...options,'--read-only','--auto','--max-steps','1','--','run_plugin_tool']);assert.equal(p.status,4,p.stderr);const journalDir=path.join(root,'project','.codex_cpp','sessions');const journals=fs.readdirSync(journalDir).filter(name=>name.endsWith('.items.jsonl')).map(name=>fs.readFileSync(path.join(journalDir,name),'utf8')).join('');assert(journals.includes('DENIED: mutation disabled by read-only policy'));assert(!p.stdout.includes('"type":"tool.started"'));
 p=run(['--plugin',manifest,'--plugin',bad,'plugin','inspect']);assert.equal(p.status,2);assert(p.stderr.includes('duplicate Plugin module'));
 console.log('PASS native Plugin model/catalog/Agent middleware/tool, streamed events, journal, explicit ABI/path/id conflicts, readonly policy and default builtin fallback');
}finally{fs.rmSync(root,{recursive:true,force:true});}
