let ticks=0;setInterval(()=>document.querySelector('#heartbeat').textContent=' UI heartbeat '+(++ticks),250);
document.querySelector('#start').onclick=()=>{
 const worker=new Worker('reader-fixture-worker.js');document.querySelector('#start').disabled=true;document.querySelector('#result').textContent='Opening and reading original ASF…';let finished=false;
 const deadline=setTimeout(()=>finish({ok:false,error:'Reader fixture deadline'}),120000);
 function finish(result){if(finished)return;finished=true;clearTimeout(deadline);globalThis.moviesMediaReaderProbeResult=result;document.querySelector('#result').textContent=JSON.stringify(result,null,2);worker.terminate();document.querySelector('#start').disabled=false;}
 worker.onmessage=e=>finish(e.data);worker.onerror=e=>finish({ok:false,error:e.message});worker.postMessage({});
};
