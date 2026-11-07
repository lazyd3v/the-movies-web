import {createGraphicsPresenter} from './graphics-presentation.js';
import {prepareGraphicsTransport} from './graphics-host.js';
const status = document.querySelector('#status'), results = document.querySelector('#results');
const canvas = document.querySelector('canvas');
try {
  if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') throw new Error('Cross-origin isolation required for presentation acknowledgement');
  const ack = new Int32Array(new SharedArrayBuffer(8));
  status.textContent = 'Starting dedicated GPU worker module…';
  const transport = await prepareGraphicsTransport({width: 190, height: 128});
  status.textContent = 'GPU worker ready; starting synchronous guest probe…';
  const worker = new Worker(`./${document.documentElement.dataset.worker ?? 'worker.js'}`, {type: 'module'});
  const presentation = new MessageChannel();
  const presentationObservations=[];let presentationError=null;
  worker.onerror = event => { status.textContent = 'FAILED'; results.textContent = JSON.stringify({error:event.message,presentationError,presentationObservations},null,2); };
  const presenter = createGraphicsPresenter({ack, consume(bitmap, serial) {
    const ctx = canvas.getContext('2d'); canvas.width = bitmap.width; canvas.height = bitmap.height;
    ctx.drawImage(bitmap, 0, 0);
    const native=document.documentElement.dataset.worker==='native-abi-worker.js',blendFrame=native&&serial===2,sourceColorFrame=native&&serial===3;
    const center=ctx.getImageData((blendFrame||sourceColorFrame)?68:Math.floor(canvas.width/2),(blendFrame||sourceColorFrame)?44:Math.floor(canvas.height/2),1,1).data;
    let expected=blendFrame?(new URLSearchParams(location.search).get('format')==='25'?[197,0,58]:[192,0,63]):[0,255,0];
    if(sourceColorFrame){const is16=new URLSearchParams(location.search).get('format')==='25';const q=a=>is16?a.map((v,i)=>Math.round(Math.floor(v/255*(i===3?1:31)+.5)/(i===3?1:31)*255)):a;let value=q([17,34,51,96]);const src=[64,128,192,128];for(let i=0;i<2;i++)value=src.map((v,c)=>Math.round(v*v/255+value[c]*(1-v/255)));value=q(value);value=q(src.map((v,c)=>Math.round(v*v/255+value[c]*(1-v/255))));expected=[...value.slice(0,3),255]; /* Presentation canvas is explicitly opaque; readback separately verifies canonical alpha. */}
    presentationObservations.push({serial,width:bitmap.width,height:bitmap.height,rgba:Array.from(center),expected,sourceColorFrame,blendFrame});
    if(expected.some((v,i)=>Math.abs(center[i]-v)>2))throw new Error(`Presented canonical target serial=${serial} ${Array.from(center)} expected ${expected}`);
  }, onError(error) { presentationError=String(error);results.textContent = JSON.stringify({presentationError,presentationObservations},null,2); }});
  presentation.port1.onmessage = ({data}) => { if (data.type === 'graphics-frame') presenter.submit(data); };
  worker.onmessage = ({data}) => {
    if (data.type === 'result') {
      globalThis.graphicsProbeResult = data.result;
      status.textContent = 'PASSED — graphics diagnostic only'; results.textContent = JSON.stringify(data.result, null, 2);
      document.documentElement.dataset.result = 'passed'; worker.terminate(); presenter.close(); transport.close();
    } else if (data.type === 'error') {
      globalThis.graphicsProbeResult = {status: 'failed', error: data.message,presentationError,presentationObservations};
      status.textContent = 'FAILED'; results.textContent = JSON.stringify(globalThis.graphicsProbeResult,null,2);
      document.documentElement.dataset.result = 'failed'; worker.terminate(); presenter.close(); transport.close();
    }
  };
  worker.postMessage({type: 'run', targetFormat: new URLSearchParams(location.search).get('format') === '25' ? 25 : 22, ack: ack.buffer, presentationPort: presentation.port2, commandPort: transport.commandPort, responseBuffer: transport.responseBuffer}, [presentation.port2, transport.commandPort]);
} catch (error) { status.textContent = 'FAILED'; results.textContent = String(error); }
