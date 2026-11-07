let worker,control,ticks=0,runId=0;
setInterval(()=>document.querySelector('#heartbeat').textContent='UI heartbeat '+(++ticks),250);
const out=document.querySelector('#result');
function stop(){runId++;if(worker)worker.terminate();worker=null;document.querySelector('#start').disabled=false;document.querySelector('#stop').disabled=true;}
document.querySelector('#stop').onclick=()=>{stop();out.textContent+='\nStopped: worker terminated; no queued samples retained.';};
document.querySelector('#start').onclick=async()=>{
 if(!crossOriginIsolated){out.textContent='Cross-origin isolation required for SAB.';return;}
 const id=++runId;
 document.querySelector('#start').disabled=true;document.querySelector('#stop').disabled=false;
 try{
 const expected=await(await fetch('stream-wasm.json')).json(),reference=new Int16Array(await(await fetch('stream-reference.pcm')).arrayBuffer());
 if(id!==runId)return;
 const slot=new Uint8Array(new SharedArrayBuffer(921600));control=new Int32Array(new SharedArrayBuffer(4));
 let frames=0,audioBytes=0,audioPackets=0,maxDifference=0,packets=0;const start=performance.now();const delay=Math.min(100,Math.max(0,Number(document.querySelector('#delay').value)||0));
 worker=new Worker('stream-worker.js');
 worker.onmessage=async({data})=>{
  try{
   if(id!==runId)return;
   if(data.type==='error')throw Error(data.error);
   if(data.type==='done'){
    if(frames!==expected.frames.length||audioBytes!==reference.byteLength||audioPackets!==expected.audioPackets.length)throw Error('Final sample accounting mismatch');
    const result={ok:true,...data.stats,frames,audioBytes,packets,audioPackets,audioMaxAbsoluteS16Difference:maxDifference,allVideoHashesAndPtsMatch:true,allAudioTimestampSpansMatchLocalWasm:true,queueSlots:1,queueBytes:slot.length,consumerDelayMilliseconds:delay,elapsedMilliseconds:performance.now()-start};
    globalThis.moviesStreamProbeResult=result;out.textContent=JSON.stringify(result,null,2);stop();return;
   }
   const bytes=slot.slice(0,data.length);
   if(data.kind==='video'){
    const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(x=>x.toString(16).padStart(2,'0')).join('');const ref=expected.frames[frames];
    if(!ref||hash!==ref.sha256||JSON.stringify(data.timing)!==JSON.stringify({index:ref.index,pts:ref.pts,timeBase:ref.timeBase}))throw Error('Frame hash/PTS mismatch '+frames);frames++;
   }else{
    if(JSON.stringify(data.timing)!==JSON.stringify(expected.audioPackets[audioPackets]))throw Error('Audio timestamp mismatch');
    const pcm=new Int16Array(bytes.buffer);for(let i=0;i<pcm.length;i++){const index=audioBytes/2+i;if(index>=reference.length)throw Error('Extra PCM');maxDifference=Math.max(maxDifference,Math.abs(pcm[i]-reference[index]));}if(maxDifference>1)throw Error('PCM differs from native reference');audioBytes+=bytes.length;audioPackets++;
   }
   packets++;out.textContent=`Frames ${frames}/${expected.frames.length}; PCM ${audioBytes}/${reference.byteLength} bytes; packets ${packets}; maximum PCM error ${maxDifference}; queue capacity 1`;
   if(delay)await new Promise(resolve=>setTimeout(resolve,delay));
   if(id!==runId)return;
   Atomics.store(control,0,0);Atomics.notify(control,0);
  }catch(error){out.textContent='Failed: '+error;stop();}
 };
 worker.onerror=e=>{out.textContent='Worker failed: '+e.message;stop();};worker.postMessage({control:control.buffer,slot:slot.buffer});
 }catch(error){out.textContent='Failed: '+error;stop();}
};
