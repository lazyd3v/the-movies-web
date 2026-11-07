'use strict';
document.querySelector('#start').onclick = () => {
  const button = document.querySelector('#start'), result = document.querySelector('#result');
  button.disabled = true;
  const worker = new Worker('native-abi-worker.js'), service = new Worker('/media-service-worker.js');
  const channel = new MessageChannel(), control = new Int32Array(new SharedArrayBuffer(20)), payload = new SharedArrayBuffer(1048576);
  service.postMessage({op:'attachPort', port:channel.port1}, [channel.port1]);
  result.textContent = 'Running native COM calls and real codec service…';
  let finished = false;
  const deadline = setTimeout(() => finish({ok:false,error:'Native ABI fixture timeout'}), 60000);
  function finish(value) {
    if (finished) return; finished = true; clearTimeout(deadline);
    result.textContent = JSON.stringify(value, null, 2);
    Atomics.store(control, 1, 1); Atomics.store(control, 0, 2); Atomics.notify(control, 0);
    worker.terminate(); service.terminate(); button.disabled = false;
  }
  worker.onmessage = e => finish(e.data);
  worker.onerror = e => finish({ok:false,error:e.message});
  service.onerror = e => finish({ok:false,error:'Service: '+e.message});
  worker.postMessage({service:{port:channel.port2,controlBuffer:control.buffer,payloadBuffer:payload}},[channel.port2]);
};
