importScripts('ffmpeg-core.js','stream-adapter.js');
onmessage=async({data})=>{
 const control=new Int32Array(data.control),slot=new Uint8Array(data.slot);
 try{
  const core=await createFFmpegCore(),input=new Uint8Array(await(await fetch('input.wmv')).arrayBuffer());
  const manifest=await(await fetch('manifest.json')).json();const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',input))].map(x=>x.toString(16).padStart(2,'0')).join('');
  if(input.length!==manifest.input.bytes||hash!==manifest.input.sha256)throw Error('Input integrity mismatch');
  const stats=MoviesStreamDecoder.run(core,input,(kind,bytes,timing)=>{
   if(bytes.length>slot.length)throw Error('Queue packet too large');
   if(Atomics.load(control,0)!==0)throw Error('Queue slot unexpectedly busy');
   slot.set(bytes);Atomics.store(control,0,1);postMessage({type:'packet',kind,length:bytes.length,timing});
   while(Atomics.load(control,0)===1){const result=Atomics.wait(control,0,1,10000);if(result==='timed-out')throw Error('Consumer backpressure deadline');}
   if(Atomics.load(control,0)!==0)throw Error('Consumer stopped');
  },{mode:data.mode||'both'});postMessage({type:'done',stats:{...stats,wasmHeapBytes:core.HEAPU8.buffer.byteLength,inputBytes:input.length}});
 }catch(error){postMessage({type:'error',error:String(error)});}
};
