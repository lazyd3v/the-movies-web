const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const base=path.resolve('.cache/media-probe');global.self={location:{href:'file://'+base+'/core/package/dist/umd/ffmpeg-core.js'}};require('../../public/media-reader-core.js');const create=require(base+'/core/package/dist/umd/ffmpeg-core.js');
(async()=>{
 const source=fs.readFileSync(base+'/input.wmv'),reference=fs.readFileSync(base+'/stream-wasm.pcm'),prior=JSON.parse(fs.readFileSync(base+'/stream-wasm.json'));const runs=[];
 assert.throws(()=>MoviesMediaReaderCore.range(-1,null));assert.throws(()=>MoviesMediaReaderCore.range(0,Number.MAX_SAFE_INTEGER));
 for(const selection of [{start100ns:0,duration100ns:null},{start100ns:10000000,duration100ns:5000000},{start100ns:200000000,duration100ns:null},{start100ns:0,duration100ns:0}]){
  const core=await create({wasmBinary:fs.readFileSync(base+'/core/package/dist/umd/ffmpeg-core.wasm')});const metadata=MoviesMediaReaderCore.probe(core,source),hash=crypto.createHash('sha256');let bytes=0,first=null,last=null;
  assert.equal(metadata.audioStreamNumber,1);assert.equal(metadata.sourceStreamCount,2);
  const stats=MoviesMediaReaderCore.decode(core,selection,(data,timing)=>{assert(data.length<=8192&&data.length%4===0);assert.deepEqual(Buffer.from(data),reference.subarray(timing.decodedSampleOffset*4,timing.decodedSampleOffset*4+data.length));hash.update(data);bytes+=data.length;first??=timing;last=timing;});
  const expected=[];for(const packet of prior.audioPackets){let offset=packet.sampleOffset;for(const span of packet.spans){const lo=Math.max(0,Math.min(span.samples,Math.ceil(selection.start100ns*44100/10000000-span.pts))),hi=selection.duration100ns===null?span.samples:Math.max(0,Math.min(span.samples,Math.ceil((selection.start100ns+selection.duration100ns)*44100/10000000-span.pts)));if(hi>lo)expected.push(reference.subarray((offset+lo)*4,(offset+hi)*4));offset+=span.samples;}}
  const expectedBytes=Buffer.concat(expected),digest=hash.digest('hex');assert.equal(bytes,expectedBytes.length);assert.equal(digest,crypto.createHash('sha256').update(expectedBytes).digest('hex'));runs.push({selection,metadata,...stats,bytes,sha256:digest,first,last});
 }
 fs.writeFileSync(base+'/reader-result.json',JSON.stringify({runs},null,2));console.log(runs.map(r=>({selection:r.selection,bytes:r.bytes,packets:r.packets,sha256:r.sha256,first:r.first?.start100ns,lastEnd:r.last&&r.last.start100ns+r.last.duration100ns})));
})().catch(e=>{console.error(e);process.exitCode=1});
