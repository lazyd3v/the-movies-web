const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const base=path.resolve('.cache/media-probe');global.self={location:{href:'file://'+base+'/core/package/dist/umd/ffmpeg-core.js'}};
require('./stream-adapter.js');const create=require(base+'/core/package/dist/umd/ffmpeg-core.js');
(async()=>{const core=await create({wasmBinary:fs.readFileSync(base+'/core/package/dist/umd/ffmpeg-core.wasm')});const frames=[],audioPackets=[],packetOrder=[],audio=crypto.createHash('sha256');let audioFd=fs.openSync(base+'/stream-wasm.pcm','w');
 const stats=MoviesStreamDecoder.run(core,fs.readFileSync(base+'/input.wmv'),(kind,data,timing)=>{packetOrder.push({kind,...timing});if(kind==='video')frames.push({...timing,sha256:crypto.createHash('sha256').update(data).digest('hex')});else{audioPackets.push(timing);audio.update(data);fs.writeSync(audioFd,data);}});fs.closeSync(audioFd);
 const result={...stats,frames,audioPackets,packetOrder,audioSha256:audio.digest('hex')};fs.writeFileSync(base+'/stream-wasm.json',JSON.stringify(result,null,2));console.log({...stats,audioSha256:result.audioSha256});
})().catch(e=>{console.error(e);process.exitCode=1});
