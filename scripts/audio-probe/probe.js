import {createMoviesAudioSink} from '/audio-sink.js';
const start=document.querySelector('#start'),stop=document.querySelector('#stop'),output=document.querySelector('#result');let active=null,cancelled=false;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function assert(value,message){if(!value)throw Error(message);}
stop.onclick=async()=>{cancelled=true;await active?.close();};
start.onclick=async()=>{
 start.disabled=true;stop.disabled=false;cancelled=false;const events=[];let sink;
 try{
  // This call executes directly within the button's transient user activation.
  const opening=createMoviesAudioSink({channels:2,capacityFrames:32768,gain:0.1});
  output.textContent='Opening audio after your click…';sink=await opening;active=sink;if(cancelled)throw Error('Stopped');
  const p=sink.producer;
  const pcm=new Int16Array(128*2);let phase=0,id=0;
  function enqueue(count){for(let block=0;block<count;block++){for(let frame=0;frame<128;frame++){const sample=Math.round(8192*Math.sin(phase));phase+=2*Math.PI*440/44100;pcm[frame*2]=pcm[frame*2+1]=sample;}p.write(++id,pcm);}}
  async function until(check,label){const deadline=performance.now()+4000;while(true){if(cancelled)throw Error('Stopped');if(sink.error)throw sink.error;const state=p.poll();events.push(...state.buffers);if(check(state))return state;if(performance.now()>deadline)throw Error(`${label} timed out`);await sleep(10);}}
  enqueue(128);await sleep(100);let state=p.poll();assert(state.consumedFrames===0&&state.buffers.length===0,'Paused buffers were consumed');
  p.resume();state=await until(s=>s.queuedBuffers===0,'Playback');assert(state.consumedFrames===16384,'Incorrect actual consumption');assert(events.length===128&&events.every(b=>b.status==='played'),'Completion did not match consumed buffers');
  const before=state;await sleep(80);state=p.poll();assert(state.consumedFrames===before.consumedFrames&&state.underrunFrames>before.underrunFrames,'Underrun accounting failed');
  p.pause();await sleep(30);enqueue(8);p.requestReset();state=await until(s=>s.resetDone,'Reset');assert(state.consumedFrames===16384,'Reset claimed discarded audio was played');assert(events.filter(b=>b.status==='reset').length===8,'Reset buffers were not returned');
  enqueue(64);p.resume();state=await until(s=>s.queuedBuffers===0,'Restart');assert(state.consumedFrames===24576,'Restart consumption failed');
  p.pause();await sleep(30);enqueue(2);const closed=await sink.close();assert(closed.length===2&&closed.every(b=>b.status==='closed'),'Close cancellation failed');
  const report={passed:true,sampleRate:sink.context.sampleRate,channels:2,bufferFrames:128,ringBytes:sink.descriptor.sampleBuffer.byteLength,controlBytes:sink.descriptor.controlBuffer.byteLength,playedBuffers:events.filter(b=>b.status==='played').length,resetBuffers:8,closedBuffers:2,consumedFrames:state.consumedFrames,renderedFrames:state.renderedFrames,underrunFrames:state.underrunFrames,pausedConsumption:0,contextState:sink.context.state,completionMeaning:'Consumed into AudioWorklet output; not physical DAC completion',hidden:document.hidden};
  globalThis.audioProbeResult=report;output.textContent=JSON.stringify(report,null,2);
 }catch(error){globalThis.audioProbeResult={passed:false,error:String(error)};output.textContent=JSON.stringify(globalThis.audioProbeResult,null,2);}
 finally{await sink?.close();active=null;start.disabled=false;stop.disabled=true;}
};
