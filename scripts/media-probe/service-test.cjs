const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),crypto=require('node:crypto');
const base=path.resolve('.cache/media-probe');global.self={location:{href:'file://'+base+'/core/package/dist/umd/ffmpeg-core.js'}};require('../../public/media-service-engine.js');
const create=require(base+'/core/package/dist/umd/ffmpeg-core.js');
(async()=>{
 const core=await create({wasmBinary:fs.readFileSync(base+'/core/package/dist/umd/ffmpeg-core.wasm')});const engine=MoviesMediaServiceEngine.create(core),profile={...MoviesMediaServiceEngine.PROFILE};const call=(op,token,args={},bytes)=>engine.request({op,token,...args},bytes);
 const token=call('createWriter').token;assert(token>0);assert.throws(()=>call('configure',token,{profile:{...profile,videoCodec:'wmv3'}}),/Only explicit/);assert.equal(call('status',token).state,'created');
 call('configure',token,{profile,byteBudget:100});const before=call('status',token);assert.throws(()=>call('appendVideo',token,{frameIndex:0,pts100ns:0},new Uint8Array(921600)),/budget/);assert.deepEqual(call('status',token),before);call('releaseWriter',token);assert.throws(()=>call('releaseWriter',token),/Unknown/);
 const real=call('createWriter').token;call('configure',real,{profile});const video=fs.readFileSync(base+'/service-frame.bgr'),audio=fs.readFileSync(base+'/stream-wasm.pcm').subarray(0,44100*4);
 assert.throws(()=>call('appendVideo',real,{frameIndex:1,pts100ns:400000},video),/consecutive/);assert.throws(()=>call('finish',real),/matching/);
 for(let i=0;i<25;i++)call('appendVideo',real,{frameIndex:i,pts100ns:i*400000},video);
 for(let offset=0;offset<audio.length;offset+=8192)call('appendAudio',real,{sampleOffset:offset/4},audio.subarray(offset,Math.min(offset+8192,audio.length)));
 const finished=call('finish',real);assert(finished.outputBytes>0);assert.throws(()=>call('appendAudio',real,{sampleOffset:44100},new Uint8Array(4)),/not accepting/);
 const chunks=[];for(let offset=0;offset<finished.outputBytes;offset+=4096)chunks.push(call('readOutput',real,{offset,length:4096}).bytes);const output=Buffer.concat(chunks);assert.equal(output.length,finished.outputBytes);assert.throws(()=>call('readOutput',real,{offset:-1,length:10}),/Invalid/);
 fs.writeFileSync(base+'/service-output.asf',output);const result={...finished,sha256:crypto.createHash('sha256').update(output).digest('hex'),inputVideoFrames:25,inputAudioSampleFrames:44100,tests:'unsupported profile, quota atomicity, invalid lifetime, timestamps, state transitions, bounded output reads'};
 call('releaseWriter',real);const fresh=call('createWriter').token;assert.equal(call('status',fresh).totalBytes,0);call('releaseWriter',fresh);fs.writeFileSync(base+'/service-result.json',JSON.stringify(result,null,2));console.log(result);
})().catch(e=>{console.error(e);process.exitCode=1});
