importScripts('ffmpeg-core.js');
const sha = async data => [...new Uint8Array(await crypto.subtle.digest('SHA-256',data))].map(x=>x.toString(16).padStart(2,'0')).join('');
onmessage = async ({data:request}) => {
 try {
  const manifest=await (await fetch('manifest.json')).json();
  const source=new Uint8Array(await (await fetch('input.wmv')).arrayBuffer());
  if(source.length!==manifest.input.bytes || await sha(source)!==manifest.input.sha256) throw Error('Input integrity mismatch');
  const core=await createFFmpegCore();let log=[];
  core.setLogger(({message})=>{log.push(message);if(log.length>150)log.shift();});
  core.FS.writeFile('input.wmv',source);const results={};
  for(const [name,args] of [['frame.yuv',['-map','0:v:0','-frames:v','1','-pix_fmt','yuv420p','-f','rawvideo']],['audio.pcm',['-map','0:a:0','-t','1','-acodec','pcm_s16le','-f','s16le']]]) {
   core.reset();core.setTimeout(30000);const start=performance.now();
   const status=core.exec('-i','input.wmv',...args,name);
   if(status!==0) throw Error('Decoder exit '+status+'\n'+log.join('\n'));
   const data=core.FS.readFile(name);
   const digest=await sha(data);
   results[name]={bytes:data.length,sha256:digest,milliseconds:performance.now()-start,matchesLocalWasm:digest===manifest.wasm[name].sha256};
   if(!results[name].matchesLocalWasm) throw Error('Browser/local Wasm output mismatch: '+name);
   if(!request.encode) core.FS.unlink(name);
  }
  let writer;
  if(request.encode) {
   if(!manifest.writer) throw Error('Writer evidence not staged');
   core.reset();core.setTimeout(30000);const start=performance.now();
   const status=core.exec(...manifest.writer.args);if(status!==0)throw Error('Writer failed '+status+'\n'+log.join('\n'));
   const encoded=core.FS.readFile('roundtrip.asf');
   writer={encoder:'WMV2 + WMAv2; no WMV3 encoder',bytes:encoded.length,sha256:await sha(encoded),milliseconds:performance.now()-start};
   writer.matchesLocalWasm=writer.sha256===manifest.writer.output.sha256;
   if(!writer.matchesLocalWasm)throw Error('ASF browser/local hash mismatch');
   core.reset();core.setTimeout(30000);
   const readStatus=core.exec('-i','roundtrip.asf','-map','0:v:0','-frames:v','1','-pix_fmt','yuv420p','-f','rawvideo','roundtrip.yuv','-map','0:a:0','-t','1','-c:a','pcm_s16le','-f','s16le','roundtrip.pcm');
   if(readStatus!==0)throw Error('ASF roundtrip decode failed '+readStatus);
   writer.decoded={};
   for(const name of ['roundtrip.yuv','roundtrip.pcm']) {
    const bytes=core.FS.readFile(name),hash=await sha(bytes);writer.decoded[name]={bytes:bytes.length,sha256:hash};
    if(hash!==manifest.writer.decoded[name].sha256)throw Error('Roundtrip browser/local mismatch '+name);
   }
  }
  postMessage({ok:true,results,writer,reference:manifest.comparison,decoder:log.find(x=>x.startsWith('ffmpeg version')),note:'Bounded decode diagnostic; no Windows Media COM integration or game playback.'});
 }catch(error){postMessage({ok:false,error:String(error)});}
};
