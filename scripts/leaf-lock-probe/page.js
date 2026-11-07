import {createOwnedWorkerHost} from '/owned-worker-host.js';
const run=document.querySelector('#run'),out=document.querySelector('#output');
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
run.onclick=async()=>{
 run.disabled=true;let owned,parent,control,timer;let settling=false;const terminationCalls=[];const workerNames=[];
 class ObservedWorker extends Worker {constructor(url,options){super(url,options);this.probeName=options?.name;workerNames.push(this.probeName);}terminate(){terminationCalls.push(this.probeName);return super.terminate();}}
 const report=value=>{globalThis.leafLockProbeResult=value;out.textContent=JSON.stringify(value,null,2);};
 try{
 const result=await new Promise((resolve,reject)=>{
 const fail=error=>{if(!settling){settling=true;reject(error instanceof Error?error:Error(String(error)));}};
 timer=setTimeout(()=>fail(Error('Supervision timeout: no qualifying real child fault')),15000);
 parent=new Worker('./worker.js');
 owned=createOwnedWorkerHost({WorkerClass:ObservedWorker,allowedURL:new URL('./probe.js',location.href).href,onError:async error=>{
  if(settling)return;settling=true;
  try{
   if(!control)throw Error('Pthread error before control publication: '+error.message);
   const before=Array.from({length:8},(_,i)=>Atomics.load(control,i)),poolBefore=owned.size;
   owned.close();parent.terminate();
   const stopped=Atomics.load(control,0);const heartbeatSamples=[];for(const delay of [20,80,200,700]){await sleep(delay);heartbeatSamples.push(Atomics.load(control,0));}const after=heartbeatSamples.at(-1);
   const result={passed:before[0]>=5&&before[1]===1&&before[3]===1&&before[4]===1&&before[5]===0x5400105c&&before[7]===1&&poolBefore===2&&owned.size===0&&heartbeatSamples.every(v=>v===stopped)&&terminationCalls.length===2,error:error.message,heartbeatBeforeFault:before[0],parentBlocked:before[1]===1,lockDepthAtBoundsTrap:before[3],boundsTrapObserved:before[4]===1,faultAddress:'0x'+(before[5]>>>0).toString(16),ownedWorkersBeforeStop:poolBefore,ownedWorkersAfterStop:owned.size,heartbeatAtStop:stopped,heartbeatAfterStop:after,heartbeatSamplesAt20_100_300_1000ms:heartbeatSamples,workerNames,terminationCalls};
   resolve(result);
  }catch(e){reject(e);}
 }});
 parent.onerror=e=>fail(Error('Unexpected parent error: '+e.message));
 parent.onmessage=async({data})=>{
  if(data.type==='log')return;
  if(data.type!=='control'){fail(Error('Unexpected parent result: '+JSON.stringify(data)));return;}
  try{
   if(!(data.buffer instanceof SharedArrayBuffer)||data.words!==8||data.offset%4)throw Error('Invalid shared control descriptor');
   control=new Int32Array(data.buffer,data.offset,8);
   while(!settling&&(Atomics.load(control,0)<5||!Atomics.load(control,1)||!Atomics.load(control,7)))await sleep(5);
   if(!settling)Atomics.store(control,2,1);
  }catch(e){fail(e);}
 };
 parent.postMessage({port:owned.port},[owned.port]);
 });report(result);
 }catch(error){report({passed:false,error:String(error)});}finally{clearTimeout(timer);owned?.close();parent?.terminate();if(control){Atomics.store(control,6,1);Atomics.notify(control,6);}run.disabled=false;}
};
