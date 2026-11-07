let ticks=0;setInterval(()=>{document.querySelector('#heartbeat').textContent='UI heartbeat '+(++ticks);},250);
function run(encode){
 document.querySelector('#start').disabled=true;document.querySelector('#writer').disabled=true;document.querySelector('#result').textContent='Loading decoder and running bounded decode…';
 const worker=new Worker('worker.js');const started=performance.now();
 const timeout=setTimeout(()=>{worker.terminate();document.querySelector('#result').textContent='Failed: worker deadline 60 seconds';document.querySelector('#start').disabled=false;document.querySelector('#writer').disabled=false;},60000);
 function finish(result){clearTimeout(timeout);result.totalMilliseconds=performance.now()-started;document.querySelector('#result').textContent=JSON.stringify(result,null,2);globalThis.moviesMediaProbeResult=result;worker.terminate();document.querySelector('#start').disabled=false;document.querySelector('#writer').disabled=false;}
 worker.onmessage=e=>finish(e.data);worker.onerror=e=>finish({ok:false,error:e.message});worker.postMessage({encode});
};

document.querySelector('#start').onclick=()=>run(false);
document.querySelector('#writer').onclick=()=>run(true);
