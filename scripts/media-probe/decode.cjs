// Local execution of the identical worker-targeted core; no media compatibility shims.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const base = path.resolve('.cache/media-probe');
global.self = { location: { href: 'file://' + base + '/core/package/dist/umd/ffmpeg-core.js' } };
const create = require(base + '/core/package/dist/umd/ffmpeg-core.js');
(async () => {
 const core = await create({ wasmBinary: fs.readFileSync(base + '/core/package/dist/umd/ffmpeg-core.wasm') });
 const logs=[];core.setLogger(({message})=>logs.push(message));
 core.FS.writeFile('input.wmv',fs.readFileSync(base+'/input.wmv'));
 const results={};
 for(const [name,args] of [['frame.yuv',['-map','0:v:0','-frames:v','1','-pix_fmt','yuv420p','-f','rawvideo']],['audio.pcm',['-map','0:a:0','-t','1','-acodec','pcm_s16le','-f','s16le']]]) {
  core.reset();core.setTimeout(30000);const start=performance.now();
  const status=core.exec('-i','input.wmv',...args,name);
  if(status!==0) throw Error('decode failed '+status+'\n'+logs.join('\n'));
  const data=core.FS.readFile(name);fs.writeFileSync(base+'/'+name,data);
  results[name]={bytes:data.length,sha256:crypto.createHash('sha256').update(data).digest('hex'),milliseconds:performance.now()-start};
 }
 fs.writeFileSync(base+'/wasm.log',logs.join('\n'));
 fs.writeFileSync(base+'/node-result.json',JSON.stringify(results,null,2));console.log(results);
})().catch(e=>{console.error(e);process.exitCode=1});
