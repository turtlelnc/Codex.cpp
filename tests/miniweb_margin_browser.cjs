const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const {spawnSync}=require('child_process');
module.exports=async(page,probe)=>{
  const file=path.resolve('miniweb/test/margins.html');
  for(const width of [640,390]){
    await page.setViewportSize({width,height:800});await page.setContent(fs.readFileSync(file,'utf8'));
    const browser=await page.evaluate(()=>Object.fromEntries(Array.from(document.querySelectorAll('[id]'),n=>{
      const r=n.getBoundingClientRect();return[n.id,[r.x,r.y,r.width,r.height]];
    })));
    const r=spawnSync(probe,['--geometry',file,String(width),Object.keys(browser).join(',')],{encoding:'utf8',timeout:10000});
    assert(!r.error,String(r.error));assert.equal(r.status,0,r.stderr);
    const native=JSON.parse(r.stdout);
    for(const id in browser)for(let i=0;i<4;i++)assert(Math.abs(native[id][i]-browser[id][i])<.1,
      `${width} ${id} axis ${i}: ${native[id]} != ${browser[id]}`);
  }
  // Deterministic screening found the nonbinding-min-height regression. Keep
  // nested combinations as black-box comparisons, with no paid model calls.
  const dir=fs.mkdtempSync(path.join(require('os').tmpdir(),'miniweb-margins-'));
  try{
    let seed=4127;const rand=n=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed%n;};
    const pick=a=>a[rand(a.length)];
    for(let sample=0;sample<35;sample++){
      let count=0;
      const tree=depth=>{
        const id='n'+count++;
        let css=`margin:${pick([-15,0,10,30])}px 0 ${pick([-15,0,10,30])}px;padding:${pick([0,0,0,1])}px;border:${pick([0,0,0,1])}px solid black;overflow:${pick(['visible','visible','visible','hidden'])};min-height:${pick([0,0,10])}px;`;
        if(rand(3)===0)css+=`height:${pick([0,20,40])}px;`;
        let children='';if(depth)for(let i=0,n=rand(3);i<n;i++)children+=tree(depth-1);else css+='height:20px;';
        return `<div id="${id}" style="${css}">${children}</div>`;
      };
      const html='<!doctype html><style>html,body{margin:0}.stage{width:300px;padding:8px;border:1px solid black}</style><div class="stage">'+tree(3)+tree(3)+'</div>';
      const file=path.join(dir,'case.html');fs.writeFileSync(file,html);await page.setContent(html);
      const expected=await page.evaluate(()=>Object.fromEntries(Array.from(document.querySelectorAll('[id]'),n=>{const r=n.getBoundingClientRect();return[n.id,[r.x,r.y,r.width,r.height]];})));
      const result=spawnSync(probe,['--geometry',file,'390',Object.keys(expected).join(',')],{encoding:'utf8',timeout:10000});
      assert(!result.error,String(result.error));assert.equal(result.status,0,result.stderr);
      const actual=JSON.parse(result.stdout);
      for(const id in expected)for(let axis=0;axis<4;axis++)assert(Math.abs(actual[id][axis]-expected[id][axis])<.1,
        `seed case ${sample} ${id} axis ${axis}: ${actual[id]} != ${expected[id]}; ${html}`);
    }
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
};
