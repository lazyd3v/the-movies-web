/* Service worker thread, separate from the guest worker that waits on SAB. */
importScripts('media-service-engine.js');
let control,payload,engine,ready=false;
const enc=new TextEncoder(),dec=new TextDecoder();
const PIN={js:'b266ab5b952555881dd6310663986994a182acb2b7ff25cf10a25f7a37ac2b21',wasm:'9f57947a5bd530d8f00c5b3f2cb2a3492faa7e5d823315342d6a8656d0a6b7b7'};
async function verified(url,hash,max){const r=await fetch(url);if(!r.ok)throw Error('Codec fetch failed '+r.status);const b=new Uint8Array(await r.arrayBuffer());if(b.length>max)throw Error('Codec artifact exceeds bound');const h=[...new Uint8Array(await crypto.subtle.digest('SHA-256',b))].map(v=>v.toString(16).padStart(2,'0')).join('');if(h!==hash)throw Error('Codec artifact hash mismatch');return b;}
function reply(ok,result){
 const bytes=result.bytes||new Uint8Array();const json=enc.encode(JSON.stringify({...result,bytes:undefined}));
 if(json.length+bytes.length>payload.length){ok=false;result={code:'BOUNDS',error:'Response exceeds SAB capacity'};return reply(ok,result);}
 payload.set(json);payload.set(bytes,json.length);Atomics.store(control,2,ok?0:1);Atomics.store(control,3,json.length);Atomics.store(control,4,bytes.length);Atomics.store(control,0,2);Atomics.notify(control,0);
}
onmessage=async({data})=>{
 try{
  if(data.op==='attachPort'){data.port.onmessage=onmessage;data.port.start();return;}
  if(data.op==='close'){close();return;}
  if(data.op==='initialize'){
   if(control)throw Error('Service already initialized');control=new Int32Array(data.control);payload=new Uint8Array(data.payload);
   const base=new URL(data.assetBase,location.href);if(base.origin!==location.origin)throw Error('Codec assets must be same-origin');
   const [js,wasm]=await Promise.all([verified(new URL('ffmpeg-core.js',base),PIN.js,120000),verified(new URL('ffmpeg-core.wasm',base),PIN.wasm,33000000)]);
   const blob=URL.createObjectURL(new Blob([js],{type:'text/javascript'}));try{importScripts(blob);}finally{URL.revokeObjectURL(blob);}
   const core=await createFFmpegCore({wasmBinary:wasm});let lines=[];core.setLogger(({message})=>lines.push(message));core.reset();if(core.exec('-encoders')!==0)throw Error('Encoder discovery failed');
   if(!lines.some(x=>/^\s*V\S*\s+wmv2\s/.test(x))||!lines.some(x=>/^\s*A\S*\s+wmav2\s/.test(x)))throw Error('Required WMV2/WMAv2 encoders unavailable');
   lines=[];core.reset();if(core.exec('-muxers')!==0||!lines.some(x=>/^\s*E\s+asf\s/.test(x)))throw Error('ASF muxer unavailable');
   engine=MoviesMediaServiceEngine.create(core);ready=true;reply(true,{ready:true,profile:MoviesMediaServiceEngine.PROFILE});return;
  }
  if(!ready)throw Error('Media service not ready');
  if(Atomics.load(control,0)!==1)throw Error('Invalid request state');
  const jsonLength=Atomics.load(control,3),binaryLength=Atomics.load(control,4);
  if(jsonLength<0||binaryLength<0||jsonLength+binaryLength>payload.length)throw Error('Invalid request bounds');
  const command=JSON.parse(dec.decode(payload.slice(0,jsonLength)));
  const result=engine.request(command,payload.subarray(jsonLength,jsonLength+binaryLength));reply(true,result);
 }catch(error){if(control)reply(false,{code:error.code||'SERVICE',error:String(error.message||error)});else throw error;}
};
