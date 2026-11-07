/* Call from a guest/fixture WORKER only; synchronous waiting never blocks the UI. */
(function(scope){
 class Client{
  constructor({workerURL='/media-service-worker.js',assetBase='/runtime/media-probe/',timeoutMilliseconds=60000,port=null,controlBuffer=null,payloadBuffer=null}={}){
   if(typeof document!=='undefined'||typeof SharedArrayBuffer==='undefined')throw Error('Media service client requires an isolated worker');
   this.control=new Int32Array(controlBuffer||new SharedArrayBuffer(20));this.payload=new Uint8Array(payloadBuffer||new SharedArrayBuffer(1048576));this.timeout=timeoutMilliseconds;this.closed=false;this.busy=false;this.worker=port?null:new Worker(workerURL);this.transport=port||this.worker;if(this.worker)this.worker.onerror=e=>{this.startError=Error(e.message||'Media worker failed');};
   try{this.transport.postMessage({op:'initialize',control:this.control.buffer,payload:this.payload.buffer,assetBase:new URL(assetBase,location.href).href});}catch(e){this.close();throw e;}
  }
  static async connect(options){const client=new Client(options);try{await client.ready();return client;}catch(e){client.close();throw e;}}
  async ready(){
   const started=performance.now();while(Atomics.load(this.control,0)!==2){if(Atomics.load(this.control,1))throw Error('Media service failed or closed by owner');if(this.closed||this.startError)throw this.startError||Error('Media client closed');if(performance.now()-started>this.timeout)throw Error('Media initialization timeout');await new Promise(resolve=>setTimeout(resolve,5));}
   this.initial=this.wait();return this;
  }
  wait(){
   const start=performance.now();for(;;){if(Atomics.load(this.control,1))throw Error('Media service failed or closed by owner');const state=Atomics.load(this.control,0);if(state===2)break;const left=this.timeout-(performance.now()-start);if(left<=0||Atomics.wait(this.control,0,state,left)==='timed-out'){this.close();throw Error('Media service timeout; worker terminated');}}
   const n=Atomics.load(this.control,3),b=Atomics.load(this.control,4);if(n<0||b<0||n+b>this.payload.length){this.close();throw Error('Invalid service response bounds');}
   const response=JSON.parse(new TextDecoder().decode(this.payload.slice(0,n)));if(Atomics.load(this.control,2)!==0){const e=Error(response.error);e.code=response.code;throw e;}
   if(b)response.bytes=this.payload.slice(n,n+b);return response;
  }
  request(command,bytes=new Uint8Array()){
   if(Atomics.load(this.control,1))throw Error('Media service failed or closed by owner');
   if(this.closed||this.busy||!this.initial)throw Error('Media service unready, closed or concurrent synchronous call');
   const json=new TextEncoder().encode(JSON.stringify(command));if(json.length+bytes.length>this.payload.length)throw Error('Request exceeds bounded SAB capacity');
   this.busy=true;try{this.payload.set(json);this.payload.set(bytes,json.length);Atomics.store(this.control,3,json.length);Atomics.store(this.control,4,bytes.length);Atomics.store(this.control,0,1);this.transport.postMessage({op:'request'});return this.wait();}finally{this.busy=false;}
  }
  createWriter(){return this.request({op:'createWriter'}).token;}
  releaseWriter(token){return this.request({op:'releaseWriter',token}).released;}
  provider(){return {moviesMediaCreateWriter:()=>{try{return this.createWriter();}catch(e){this.lastError=e;return 0;}},moviesMediaConfigureWriter:(token,profile)=>{try{return this.request({op:'configure',token,profile}).configured?1:0;}catch(e){this.lastError=e;return 0;}},moviesMediaReleaseWriter:token=>{try{return this.releaseWriter(token)?1:0;}catch(e){this.lastError=e;return 0;}}};}
  close(){if(this.closed)return;this.closed=true;if(this.worker)this.worker.terminate();else if(this.transport){try{this.transport.postMessage({op:'close'});}catch{}finally{this.transport.close();}}}
 }
 scope.MoviesMediaServiceClient=Client;
})(globalThis);
