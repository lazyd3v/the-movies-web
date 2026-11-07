const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const base=path.resolve('.cache/media-probe');global.self={location:{href:'file://'+base+'/core/package/dist/umd/ffmpeg-core.js'}};
require('./stream-adapter.js');const create=require(base+'/core/package/dist/umd/ffmpeg-core.js');
(async()=>{const expected=JSON.parse(fs.readFileSync(base+'/stream-wasm.json'));const result={};
 for(const mode of ['video','audio']){
  const core=await create({wasmBinary:fs.readFileSync(base+'/core/package/dist/umd/ffmpeg-core.wasm')});let count=0;const audio=crypto.createHash('sha256');
  const stats=MoviesStreamDecoder.run(core,fs.readFileSync(base+'/input.wmv'),(kind,data,timing)=>{assert.equal(kind,mode);if(kind==='video'){const ref=expected.frames[count];assert.deepEqual(timing,{index:ref.index,pts:ref.pts,timeBase:ref.timeBase});assert.equal(crypto.createHash('sha256').update(data).digest('hex'),ref.sha256);}else{assert.deepEqual(timing,expected.audioPackets[count]);audio.update(data);}count++;},{mode});
  if(mode==='audio')assert.equal(audio.digest('hex'),expected.audioSha256);
  assert.equal(count,mode==='video'?expected.frames.length:expected.audioPackets.length);result[mode]={...stats,packets:count,verified:true};
 }fs.writeFileSync(base+'/independent-result.json',JSON.stringify(result,null,2));console.log(result);
})().catch(e=>{console.error(e);process.exitCode=1});
