importScripts('media-reader-core.js');
let control,payload,assetBase,warm=null,initialized=false,totalInput=0,nextToken=1;const sessions=new Map();
const MAX_INPUT=32*1024*1024,MAX_TOTAL=64*1024*1024;
function signal(d){for(const wake of d.waiters)wake();}
function waitFor(d,predicate,timeout=60000){return new Promise((resolve,reject)=>{let timer;const check=()=>{if(d.error){cleanup();reject(Error(d.error));}else if(predicate()){cleanup();resolve();}};function cleanup(){clearTimeout(timer);d.waiters.delete(check);}d.waiters.add(check);timer=setTimeout(()=>{cleanup();reject(Error('Reader decoder deadline'));},timeout);check();});}
function spawn(){
 const d={worker:new Worker('media-reader-decoder.js'),waiters:new Set(),ready:false,metadata:null,pending:null,eos:false,error:null,control:new Int32Array(new SharedArrayBuffer(4)),slot:new Uint8Array(new SharedArrayBuffer(8192))};
 d.worker.onmessage=({data})=>{if(d.closed)return;if(data.type==='ready')d.ready=true;else if(data.type==='opened')d.metadata=data.metadata;else if(data.type==='sample'){if(d.pending)d.error='PCM queue overflow';else d.pending={length:data.length,timing:data.timing,offset:0};}else if(data.type==='eos'){d.eos=true;d.stats=data.stats;d.wasmHeapBytes=data.wasmHeapBytes;}else if(data.type==='error')d.error=data.error;signal(d);};
 d.worker.onerror=e=>{d.error=e.message||'Decoder worker error';signal(d);};d.worker.postMessage({op:'initialize',assetBase});return d;
}
function dispose(d){if(!d||d.closed)return;d.closed=true;d.error='Reader decoder closed';Atomics.store(d.control,0,-1);Atomics.notify(d.control,0);d.worker.terminate();signal(d);}
function session(token){const s=sessions.get(token);if(!s)throw Error('Unknown reader token');return s;}
async function openDecoder(s){
 const d=warm||spawn();warm=null;s.decoder=d;
 try{await waitFor(d,()=>d.ready);d.worker.postMessage({op:'open',input:s.input.buffer,control:d.control.buffer,slot:d.slot.buffer});await waitFor(d,()=>d.metadata!==null);s.state='opened';s.started=false;return d.metadata;}
 catch(e){dispose(d);s.state='failed';throw e;}
}
async function request(c,bytes){
 if(c.op==='capabilities')return {ready:initialized,codec:'wmav2',sampleRate:44100,channels:2,bitsPerSample:16,maxSampleBytes:8192,maxInputBytes:MAX_INPUT,maxReaders:2};
 if(c.op==='createReader'){
  if(!Number.isSafeInteger(c.size)||c.size<=0||c.size>MAX_INPUT||sessions.size>=2||c.size>MAX_TOTAL-totalInput)throw Error('Compressed input quota exceeded or invalid declared size');
  const token=nextToken++;if(token>0x7fffffff)throw Error('Reader token exhausted');const input=new Uint8Array(c.size);sessions.set(token,{input,offset:0,state:'uploading',decoder:null,range:{start100ns:0,duration100ns:null}});totalInput+=c.size;return {token};
 }
 const s=session(c.token);
 if(c.op==='releaseReader'){dispose(s.decoder);totalInput-=s.input.length;sessions.delete(c.token);return {released:true};}
 if(c.op==='appendInput'){
  if(s.state!=='uploading'||c.offset!==s.offset||!bytes.length||bytes.length>s.input.length-s.offset)throw Error('Input append must be consecutive and within declared size');
  s.input.set(bytes,s.offset);s.offset+=bytes.length;return {acceptedBytes:bytes.length,received:s.offset};
 }
 if(c.op==='openReader'){if(s.state!=='uploading'||s.offset!==s.input.length)throw Error('Reader input incomplete or already opened');return {metadata:await openDecoder(s)};}
 if(c.op==='metadata'){if(s.state!=='opened')throw Error('Reader not opened');return {metadata:s.decoder.metadata};}
 if(c.op==='setRange'){
  if(s.state!=='opened')throw Error('Reader not opened');MoviesMediaReaderCore.range(c.start100ns,c.duration100ns);dispose(s.decoder);s.range={start100ns:c.start100ns,duration100ns:c.duration100ns};await openDecoder(s);return {range:s.range,seeked:true};
 }
 if(c.op==='readSample'){
  if(s.state!=='opened'||!Number.isSafeInteger(c.maxBytes)||c.maxBytes<4||c.maxBytes>8192||c.maxBytes%4)throw Error('Reader unready or invalid aligned sample bound');
  const d=s.decoder;if(!s.started){s.started=true;d.worker.postMessage({op:'decode',range:s.range});}
  try{await waitFor(d,()=>d.pending!==null||d.eos,60000);}catch(e){s.state='failed';dispose(d);throw e;}
  if(!d.pending)return {eos:true,bytes:new Uint8Array(),stats:d.stats,wasmHeapBytes:d.wasmHeapBytes};
  const p=d.pending,length=Math.min(c.maxBytes,p.length-p.offset),start=p.timing.sourcePts+p.offset/4,end=start+length/4;
  const result={eos:false,bytes:d.slot.slice(p.offset,p.offset+length),start100ns:Math.round(start*10000000/44100),duration100ns:Math.round(end*10000000/44100)-Math.round(start*10000000/44100),sampleRate:44100,channels:2,bitsPerSample:16,sourcePts:start,timeBase:[1,44100],decodedSampleOffset:p.timing.decodedSampleOffset+p.offset/4};
  p.offset+=length;if(p.offset===p.length){d.pending=null;Atomics.store(d.control,0,0);Atomics.notify(d.control,0);}return result;
 }
 throw Error('Unsupported reader command');
}
function reply(ok,result){const bytes=result.bytes||new Uint8Array(),json=new TextEncoder().encode(JSON.stringify({...result,bytes:undefined}));if(json.length+bytes.length>payload.length){reply(false,{error:'Reader response too large'});return;}payload.set(json);payload.set(bytes,json.length);Atomics.store(control,2,ok?0:1);Atomics.store(control,3,json.length);Atomics.store(control,4,bytes.length);Atomics.store(control,0,2);Atomics.notify(control,0);}
onmessage=async({data})=>{try{
 if(data.op==='attachPort'){data.port.onmessage=onmessage;data.port.start();return;}
 if(data.op==='close'){dispose(warm);for(const s of sessions.values())dispose(s.decoder);close();return;}
 if(data.op==='initialize'){
  if(control)throw Error('Reader service already initialized');control=new Int32Array(data.control);payload=new Uint8Array(data.payload);assetBase=data.assetBase;warm=spawn();try{await waitFor(warm,()=>warm.ready);}catch(e){dispose(warm);throw e;}initialized=true;reply(true,{ready:true,maxInputBytes:MAX_INPUT,maxReaders:2,queueBytesPerReader:8192});return;
 }
 if(!initialized||Atomics.load(control,0)!==1)throw Error('Reader service unready or invalid state');
 const n=Atomics.load(control,3),b=Atomics.load(control,4);if(n<0||b<0||n+b>payload.length)throw Error('Reader request bounds');
 const command=JSON.parse(new TextDecoder().decode(payload.slice(0,n)));reply(true,await request(command,payload.subarray(n,n+b)));
}catch(e){if(control)reply(false,{error:String(e.message||e),code:'READER'});else throw e;}};
