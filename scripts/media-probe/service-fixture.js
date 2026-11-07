let ticks=0;setInterval(()=>document.querySelector('#heartbeat').textContent='UI heartbeat '+(++ticks),250);
document.querySelector('#start').onclick=()=>{
 document.querySelector('#start').disabled=true;const worker=new Worker('service-fixture-worker.js'),service=new Worker('/media-service-worker.js'),channel=new MessageChannel(),control=new Int32Array(new SharedArrayBuffer(20)),payload=new SharedArrayBuffer(1048576);service.postMessage({op:'attachPort',port:channel.port1},[channel.port1]);document.querySelector('#result').textContent='Running synchronous client and real codec service…';
 const deadline=setTimeout(()=>finish({ok:false,error:'Fixture timeout'}),120000);
 function finish(result){clearTimeout(deadline);globalThis.moviesMediaServiceProbeResult=result;document.querySelector('#result').textContent=JSON.stringify(result,null,2);Atomics.store(control,1,1);Atomics.store(control,0,2);Atomics.notify(control,0);worker.terminate();service.terminate();document.querySelector('#start').disabled=false;}
 worker.onmessage=e=>finish(e.data);worker.onerror=e=>finish({ok:false,error:e.message});service.onerror=e=>finish({ok:false,error:'Service: '+e.message});worker.postMessage({service:{port:channel.port2,controlBuffer:control.buffer,payloadBuffer:payload}},[channel.port2]);
};
