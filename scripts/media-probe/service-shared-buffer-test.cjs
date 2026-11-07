// Model browser WebIDL's non-shared BufferSource requirement explicitly: Node
// TextDecoder may accept SharedArrayBuffer even where browser implementations do not.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
class BrowserTextDecoder extends TextDecoder {
 decode(input,...args){assert(!(input?.buffer instanceof SharedArrayBuffer),'TextDecoder received shared memory');return super.decode(input,...args);}
}
const globals={TextEncoder,TextDecoder:BrowserTextDecoder,Uint8Array,Int32Array,SharedArrayBuffer,Atomics,performance,URL,setTimeout};
(async()=>{
 const constructed=[];class FakeWorker{constructor(){constructed.push(this);}terminate(){this.terminated=true;}postMessage(){}}
 const clientContext=vm.createContext({...globals,Worker:FakeWorker,location:{href:'http://localhost/fixture-worker.js'}});vm.runInContext(fs.readFileSync('public/media-service-client.js','utf8'),clientContext);
 const Client=clientContext.MoviesMediaServiceClient;
 await assert.rejects(Client.connect({assetBase:'http://['}),/Invalid URL/);assert.equal(constructed.length,1);assert(constructed[0].terminated,'Constructor failure leaked its worker');
 const client=Object.create(Client.prototype);client.control=new Int32Array(new SharedArrayBuffer(20));client.payload=new Uint8Array(new SharedArrayBuffer(1048576));client.initial={ready:true};client.timeout=1000;client.closed=false;client.busy=false;
 client.transport={postMessage(){const text=new TextEncoder().encode(JSON.stringify({token:17}));client.payload.set(text);client.payload.set([9,8,7],text.length);Atomics.store(client.control,2,0);Atomics.store(client.control,3,text.length);Atomics.store(client.control,4,3);Atomics.store(client.control,0,2);}};
 const response=client.request({op:'createWriter'});assert.equal(response.token,17);assert.deepEqual([...response.bytes],[9,8,7]);assert(!(response.bytes.buffer instanceof SharedArrayBuffer));
 const control=new Int32Array(new SharedArrayBuffer(20)),payload=new Uint8Array(new SharedArrayBuffer(1048576));
 const text=new TextEncoder().encode(JSON.stringify({op:'appendAudio',token:7,sampleOffset:0}));payload.set(text);payload.set([1,2,3,4],text.length);control[0]=1;control[3]=text.length;control[4]=4;
 let called=false;
 const workerContext=vm.createContext({...globals,importScripts(){},testControl:control,testPayload:payload,testEngine:{request(command,bytes){called=true;assert.equal(command.op,'appendAudio');assert.equal(command.token,7);assert.deepEqual([...bytes],[1,2,3,4]);return {acceptedBytes:4};}}});
 vm.runInContext(fs.readFileSync('public/media-service-worker.js','utf8'),workerContext);vm.runInContext('control=testControl;payload=testPayload;engine=testEngine;ready=true;',workerContext);
 await workerContext.onmessage({data:{op:'request'}});assert(called);assert.equal(control[0],2);assert.equal(control[2],0);assert.equal(JSON.parse(new TextDecoder().decode(payload.slice(0,control[3]))).acceptedBytes,4);
 Atomics.store(client.control,1,1);assert.throws(()=>client.request({op:'status'}),/failed or closed/);assert.throws(()=>client.wait(),/failed or closed/);
 console.log('Shared-buffer browser BufferSource regression tests passed (client + service requests + copied binary response).');
})().catch(e=>{console.error(e);process.exitCode=1});
