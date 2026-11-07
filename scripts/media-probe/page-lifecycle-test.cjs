const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const elements=new Map();function el(id){if(!elements.has(id))elements.set(id,{value:'1',checked:false,disabled:false,textContent:'',open:false,events:{},addEventListener(type,cb){this.events[type]=cb;},close(){this.open=false;}});return elements.get(id);}
const workers=[],transports=[],presenters=[],audioSinks=[],ownedPools=[],pointerHosts=[];
class Port{close(){this.closed=true;}}
class Channel{constructor(){this.port1=new Port();this.port2=new Port();}}
class FakeWorker{constructor(url){this.url=url;this.messages=[];workers.push(this);}postMessage(data,transfer){this.messages.push({data,transfer});}terminate(){this.terminated=true;}}
class Input{static create(){return new SharedArrayBuffer(16);}push(){return true;}}
class Direct{dispose(){this.disposed=true;}}
class Surface{constructor(){this.geometry=new Map();}clear(){this.geometry.clear();} }
let holdPreparation=false,resolvePreparation,enteredPreparation;
let holdAudio=false,enteredAudio,resolveAudio;
async function testImport(name){
 if(name==='./owned-worker-host.js')return {createOwnedWorkerHost:()=>{const pool={port:new Port(),close(){this.closed=true;this.port.close();}};ownedPools.push(pool);return pool;}};
 if(name==='./audio-sink.js')return {createMoviesAudioSink:async()=>{const sink={descriptor:{test:true},context:{state:'running'},async close(){this.closed=true;}};audioSinks.push(sink);if(holdAudio){enteredAudio();await new Promise(resolve=>{resolveAudio=resolve;});}return sink;}};
 if(name==='./graphics-host.js')return {prepareGraphicsTransport:async()=>{
  const t={commandPort:new Port(),responseBuffer:new SharedArrayBuffer(64),close(){this.closed=true;this.commandPort.close();}};transports.push(t);
  if(holdPreparation){enteredPreparation();await new Promise(resolve=>{resolvePreparation=resolve;});}return t;
 }};
 if(name==='./pointer-lock.js')return {MoviesPointerLockHost:class {constructor(){pointerHosts.push(this);}attach(){}close(){this.closed=true;}async request(){}}};
 if(name==='./graphics-presentation.js')return {createGraphicsPresenter:()=>{const p={close(){this.closed=true;},submit(){}};presenters.push(p);return p;}};
 throw Error('Unexpected dynamic import '+name);
}
const ctx=vm.createContext({URL,location:{href:'http://localhost/static-probe.html'},document:{getElementById:el},MoviesWindowSurface:Surface,MoviesWindowInput:Input,MoviesPointerSnapshot:class {static create(){return new SharedArrayBuffer(16);}write(){}},MoviesAsyncKeyControlBytes:56,MoviesDirectInputFields:[],MoviesDirectInputHost:Direct,Worker:FakeWorker,MessageChannel:Channel,SharedArrayBuffer,Int32Array,Uint8Array,TextDecoder,Atomics,performance:{now:()=>0},setInterval(){return 9;},clearInterval(){},setTimeout(){return 1;},clearTimeout(){},testImport});
// Substitute only the two external dependency import expressions; production
// lifecycle code is evaluated unchanged and all awaits remain asynchronous.
let source=fs.readFileSync('public/static-probe.js','utf8');
for(const name of ['./audio-sink.js','./graphics-host.js','./graphics-presentation.js','./pointer-lock.js','./owned-worker-host.js']){const expression=`import('${name}')`;assert(source.includes(expression));source=source.replace(expression,`testImport('${name}')`);}
vm.runInContext(source,ctx);
async function begin(){await Promise.resolve();await el('start').events.click();const guest=workers.at(-3),media=workers.at(-2),reader=workers.at(-1);assert.equal(guest.url,'static-probe-worker.js');assert.equal(media.url,'media-service-worker.js');assert.equal(reader.url,'media-reader-service.js');const message=guest.messages[0];assert.equal(message.transfer[0],message.data.media.port);assert.equal(message.transfer[1],message.data.reader.port);return {guest,media,reader,readerControl:new Int32Array(message.data.reader.controlBuffer),transport:transports.at(-1),presenter:presenters.at(-1),control:new Int32Array(message.data.media.controlBuffer)};}
(async()=>{
 const first=await begin();first.media.onerror({message:'Actual service failure'});assert(first.reader.terminated&&first.media.terminated&&first.guest.terminated&&first.transport.closed&&first.presenter.closed);assert.equal(first.readerControl[1],1);assert.equal(first.control[1],1);assert.equal(first.control[0],2);assert.match(el('status').textContent,/Actual service failure/);
 const second=await begin();first.guest.onmessage({data:{type:'thread-failure-watch',descriptor:{}}});assert(!second.guest.terminated,'Stale watchdog descriptor stopped new run');first.media.onerror({message:'Stale failure'});first.reader.onerror({message:'Stale reader failure'});assert(!second.reader.terminated&&!second.media.terminated&&!second.guest.terminated);el('stop').events.click();assert(second.reader.terminated&&second.media.terminated&&second.guest.terminated&&second.transport.closed);assert.equal(second.readerControl[1],1);assert.equal(second.control[1],1);
 const third=await begin();third.guest.onmessage({data:{type:'terminal',code:4,text:'Stopped'}});assert(third.reader.terminated&&third.media.terminated&&third.guest.terminated&&third.transport.closed);assert.equal(third.readerControl[1],1);assert.equal(third.control[1],1);
 const fourth=await begin();fourth.reader.onerror({message:'Real reader failure'});assert(fourth.reader.terminated&&fourth.media.terminated&&fourth.guest.terminated&&fourth.transport.closed);assert.equal(fourth.readerControl[1],1);assert.equal(fourth.control[1],1);assert.match(el('status').textContent,/Real reader failure/);
 const fatal=await begin();const failure=new SharedArrayBuffer(512);new Uint8Array(failure,16).set(new TextEncoder().encode('actual shared child failure'));Atomics.store(new Int32Array(failure),0,1);fatal.guest.onmessage({data:{type:'thread-failure-watch',descriptor:{sharedWasmBuffer:failure,flagOffset:0,errorOffset:16,errorCapacity:256}}});assert(fatal.guest.terminated&&fatal.media.terminated&&fatal.reader.terminated&&fatal.transport.closed);assert(audioSinks.at(-1).closed);assert.match(el('status').textContent,/actual shared child failure/);
 el('optimization').value='1-threads';const threaded=await begin();assert(threaded.guest.messages[0].data.pthreadPort);assert.equal(threaded.guest.messages[0].transfer.at(-1),ownedPools.at(-1).port);el('stop').events.click();assert(ownedPools.at(-1).closed,'Page-owned pthreads leaked');assert(pointerHosts.at(-1).closed,'Pointer capture host leaked on stop');el('optimization').value='1';
 holdPreparation=true;const entered=new Promise(resolve=>{enteredPreparation=resolve;});const count=workers.length,pending=el('start').events.click();await entered;const guest=workers.at(-1),transport=transports.at(-1);el('stop').events.click();assert(guest.terminated);resolvePreparation();await pending;assert(transport.closed,'Late GPU preparation leaked transport');assert(pointerHosts.at(-1).closed,'Pointer capture host leaked during prewarm stop');assert(audioSinks.at(-1).closed,'Audio context leaked on stop');assert.equal(workers.length,count+1,'Media worker created after stop');
 const enteredError=new Promise(resolve=>{enteredPreparation=resolve;});const errorStart=el('start').events.click();await enteredError;const earlyGuest=workers.at(-1),lateTransport=transports.at(-1);earlyGuest.onerror({message:'Guest failed during prewarm'});assert(earlyGuest.terminated);resolvePreparation();await errorStart;assert(lateTransport.closed);assert.match(el('status').textContent,/Guest failed during prewarm/);
 holdPreparation=false;holdAudio=true;const audioEntered=new Promise(resolve=>{enteredAudio=resolve;});const audioStart=el('start').events.click();await audioEntered;const audioGuest=workers.at(-1),lateAudio=audioSinks.at(-1);el('stop').events.click();assert(audioGuest.terminated);resolveAudio();await audioStart;assert(lateAudio.closed,'Late audio prewarm leaked context');
 console.log('Page lifecycle passed: error→SAB, cleanup, stale guards, and stop during asynchronous GPU prewarm.');
})().catch(e=>{console.error(e);process.exitCode=1});
