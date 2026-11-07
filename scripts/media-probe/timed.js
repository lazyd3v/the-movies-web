let generation=0,workers=[],timer,ticks=0;
const out=document.querySelector('#result');
setInterval(()=>document.querySelector('#heartbeat').textContent='UI heartbeat '+(++ticks),250);
function stop(){generation++;for(const w of workers)w.terminate();workers=[];clearTimeout(timer);document.querySelector('#start').disabled=false;document.querySelector('#stop').disabled=true;}
document.querySelector('#stop').onclick=()=>{stop();out.textContent+='\nStopped; both workers and queues discarded. Restart begins at original start.';};
document.querySelector('#start').onclick=async()=>{
 if(!crossOriginIsolated){out.textContent='SAB requires cross-origin isolation.';return;}
 const id=++generation;document.querySelector('#start').disabled=true;document.querySelector('#stop').disabled=false;out.textContent='Preparing two independent decoders…';
 try{
 const expected=await(await fetch('stream-wasm.json')).json(),reference=new Int16Array(await(await fetch('stream-reference.pcm')).arrayBuffer());if(id!==generation)return;
 const streams={};let clockStart=null,frames=0,audioBytes=0,audioPackets=0,pcmError=0,lastVideoPts=null,lastAudioPts=null,maxAVDeliveryLead=0,maxLate={video:0,audio:0},latePackets={video:0,audio:0},underruns={video:0,audio:0},underrunActive={video:false,audio:false},firstReady={},started=performance.now(),hidden=false;
 const counts={video:expected.frames.length,audio:expected.audioPackets.length};
 const expectedAt=(kind,index)=>kind==='video'?expected.frames[index]?.pts/1000:expected.audioPackets[index]?.spans[0].pts/44100;
 const audioEnd=Math.max(...expected.audioPackets.flatMap(p=>p.spans.map(s=>(s.pts+s.samples)/44100)));
 const duration=Math.max(audioEnd,(expected.frames.at(-1).pts+33)/1000);
 function fail(error){if(id!==generation)return;out.textContent='Failed: '+error;globalThis.moviesTimedProbeResult={ok:false,error:String(error)};stop();}
 function tick(){
  if(id!==generation)return;hidden ||=document.visibilityState!=='visible';
  if(clockStart===null){if(performance.now()-started>30000)throw Error('Decoder priming timeout');if(streams.video?.pending&&streams.audio?.pending)clockStart=performance.now();else{timer=setTimeout(tick,4);return;}}
  const now=(performance.now()-clockStart)/1000;
  for(const kind of ['video','audio']){
   const s=streams[kind],next=expectedAt(kind,s.delivered);
   if(s.pending&&s.pending.pts<=now){
    const p=s.pending,lateness=Math.max(0,(now-p.pts)*1000);maxLate[kind]=Math.max(maxLate[kind],lateness);if(lateness>100)latePackets[kind]++;
    if(kind==='video'){frames++;lastVideoPts=p.pts;}else{audioBytes+=p.length;audioPackets++;lastAudioPts=p.pts;}
    if(lastVideoPts!==null&&lastAudioPts!==null)maxAVDeliveryLead=Math.max(maxAVDeliveryLead,Math.abs(lastVideoPts-lastAudioPts));
    s.pending=null;s.delivered++;underrunActive[kind]=false;Atomics.store(s.control,0,0);Atomics.notify(s.control,0);
   }else if(!s.pending&&next!==undefined&&now-next>0.100&&!underrunActive[kind]){underruns[kind]++;underrunActive[kind]=true;}
  }
  out.textContent=`Clock ${now.toFixed(3)} / ${duration.toFixed(3)} s; video ${frames}/${counts.video}; audio ${audioBytes}/${reference.byteLength} bytes; maximum late video/audio ${maxLate.video.toFixed(1)}/${maxLate.audio.toFixed(1)} ms; queue slots 1+1`;
  if(streams.video.done&&streams.audio.done&&now>=duration){
   if(frames!==counts.video||audioBytes!==reference.byteLength||audioPackets!==counts.audio)throw Error('Final accounting mismatch');
   const result={ok:true,frames,audioSampleFrames:audioBytes/4,audioBytes,audioPackets,allFrameHashesAndPtsMatch:true,allAudioSpansMatchLocalWasm:true,pcmMaxDifference:pcmError,clockedSeconds:now,expectedEndSeconds:duration,totalMilliseconds:performance.now()-started,firstPacketReadyMilliseconds:firstReady,maxLatenessMilliseconds:maxLate,packetsOver100msLate:latePackets,underrunEpisodesOver100ms:underruns,maxDeliveredStartPtsDifferenceSeconds:maxAVDeliveryLead,hiddenDuringRun:hidden,timingPassed:!hidden&&!latePackets.video&&!latePackets.audio&&!underruns.video&&!underruns.audio,queueBytes:{video:921600,audio:8192},workers:2,decoderStats:{video:streams.video.stats,audio:streams.audio.stats},muted:true,note:'Audio decoder PTS differs up to10.07ms from native FFmpeg7.1; no audio device or graph.'};
   globalThis.moviesTimedProbeResult=result;out.textContent=JSON.stringify(result,null,2);stop();return;
  }
  if(now>duration+15)throw Error('Clocked decoder completion timeout');timer=setTimeout(()=>{try{tick();}catch(e){fail(e);}},4);
 }
 for(const kind of ['video','audio']){
  const s=streams[kind]={slot:new Uint8Array(new SharedArrayBuffer(kind==='video'?921600:8192)),control:new Int32Array(new SharedArrayBuffer(4)),pending:null,received:0,delivered:0,done:false};
  const worker=new Worker('stream-worker.js');workers.push(worker);
  worker.onerror=e=>fail(e.message);
  worker.onmessage=async({data})=>{
   try{
    if(id!==generation)return;if(data.type==='error')throw Error(data.error);
    if(data.type==='done'){s.done=true;s.stats=data.stats;return;}
    if(data.kind!==kind||s.pending)throw Error('Wrong stream or queue overflow');
    const bytes=s.slot.slice(0,data.length);let pts;
    if(kind==='video'){
     const ref=expected.frames[s.received];const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(x=>x.toString(16).padStart(2,'0')).join('');
     if(!ref||ref.sha256!==hash||JSON.stringify(data.timing)!==JSON.stringify({index:ref.index,pts:ref.pts,timeBase:ref.timeBase}))throw Error('Video mismatch '+s.received);pts=data.timing.pts/1000;
    }else{
     const ref=expected.audioPackets[s.received];if(JSON.stringify(ref)!==JSON.stringify(data.timing))throw Error('Audio timestamp mismatch '+s.received);
     const samples=new Int16Array(bytes.buffer),base=data.timing.sampleOffset*2;
     for(let i=0;i<samples.length;i++){if(base+i>=reference.length)throw Error('Extra audio');pcmError=Math.max(pcmError,Math.abs(samples[i]-reference[base+i]));}if(pcmError>1)throw Error('PCM mismatch');pts=data.timing.spans[0].pts/44100;
    }
    if(id!==generation)return;s.received++;s.pending={pts,length:data.length};firstReady[kind]??=performance.now()-started;
   }catch(e){fail(e);}
  };
  worker.postMessage({mode:kind,control:s.control.buffer,slot:s.slot.buffer});
 }
 timer=setTimeout(()=>{try{tick();}catch(e){fail(e);}},4);
 }catch(error){out.textContent='Failed: '+error;stop();}
};
