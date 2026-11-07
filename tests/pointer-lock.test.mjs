import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/window-input.js';
import '../public/directinput-host.js';
import {MoviesPointerLockHost} from '../public/pointer-lock.js';
class Target {
 constructor(){this.handlers=new Map();}
 addEventListener(type,fn,capture){const list=this.handlers.get(type)||[];list.push({fn,capture:!!capture});this.handlers.set(type,list);}
 removeEventListener(type,fn){this.handlers.set(type,(this.handlers.get(type)||[]).filter(x=>x.fn!==fn));}
 fire(type,data={}){for(const {fn} of [...(this.handlers.get(type)||[])].sort((a,b)=>Number(b.capture)-Number(a.capture)))fn({type,preventDefault(){},...data});}
}
function fixture(){
 const env=new Target(),document=new Target(),canvas=new Target(),snapshot=new MoviesPointerSnapshot(MoviesPointerSnapshot.create()),status=[];
 env.document=document;Object.assign(document,{activeElement:canvas,hidden:false,pointerLockElement:null,hasFocus:()=>true,exitPointerLock(){this.pointerLockElement=null;this.fire('pointerlockchange');}});
 Object.assign(canvas,{width:800,height:600,hidden:false,getBoundingClientRect:()=>({left:0,top:0,width:400,height:300}),focus(){document.activeElement=this;this.fire('focus');},requestPointerLock(){document.pointerLockElement=this;document.fire('pointerlockchange');}});
 const host=new MoviesPointerLockHost({snapshot,surface:{},env,onStatus:s=>status.push(s)});host.attach(canvas,42);
 return {env,document,canvas,snapshot,status,host};
}
test('capture requires real initial point; atomic warp changes cursor with no DI motion',()=>{
 const {env,canvas,snapshot,status,host,document}=fixture();assert.equal(host.request(),false);assert.equal(snapshot.warp(512,384),false);
 snapshot.write({x:40,y:50});const buffer=MoviesWindowInput.create(16,MoviesDirectInputFields),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes);
 const di=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);di.attach(canvas,42);canvas.fire('focus');
 const queue=new MoviesWindowInput(buffer,MoviesDirectInputFields);assert.equal(host.request(),true);assert.equal(status.at(-1).locked,true);
 assert.equal(snapshot.warp(512,384),true);assert.deepEqual(snapshot.read(),{x:512,y:384});assert.deepEqual(queue.drain(16),[]);
 canvas.fire('pointermove',{pointerType:'mouse',movementX:3,movementY:-2,clientX:0,clientY:0});assert.deepEqual(snapshot.read(),{x:518,y:380});
 assert.deepEqual(queue.drain(16).map(e=>[e.offset,e.data|0]),[[0,6],[4,-4]]);
 snapshot.write({x:999,y:999});assert.deepEqual(snapshot.read(),{x:518,y:380},'absolute pointer cannot override capture');
 document.exitPointerLock();assert.equal(snapshot.warp(0,0),false);assert.deepEqual(snapshot.read(),{x:518,y:380},'retains virtual position, not OS pointer');assert.equal(status.at(-1).lost,true);
 snapshot.write({x:-10,y:25});assert.deepEqual(snapshot.read(),{x:-10,y:25},'new physical sample supersedes retained virtual point');
 snapshot.write({x:40,y:50});host.request();env.fire('blur');assert.equal(snapshot.isLocked(),false);assert.deepEqual(snapshot.read(),{x:40,y:50});
 host.close();di.dispose();assert.equal(document.pointerLockElement,null);
});
test('legacy snapshot remains physical-only and invalid input is rejected',()=>{
 const old=new MoviesPointerSnapshot(new SharedArrayBuffer(16));old.write({x:-1,y:2});assert.deepEqual(old.read(),{x:-1,y:2});assert.equal(old.warp(1,2),false);
 const s=new MoviesPointerSnapshot(MoviesPointerSnapshot.create());assert.throws(()=>s.setLocked({x:-1,y:2}));s.setLocked({x:1023,y:767});assert.deepEqual(s.move(10,20,1024,768),{x:1023,y:767});assert.deepEqual(s.move(-2000,-2000,1024,768),{x:0,y:0});assert.equal(s.warp(-1,2),false);
});
test('concurrent capture-generation change defeats stale worker warp CAS',async()=>{
 const {Worker}=await import('node:worker_threads'),{once}=await import('node:events');
 const buffer=MoviesPointerSnapshot.create(),snapshot=new MoviesPointerSnapshot(buffer);snapshot.setLocked({x:10,y:20});
 const gate=new SharedArrayBuffer(8),state=new Int32Array(gate);
 const worker=new Worker(`const {parentPort,workerData}=require('node:worker_threads');
 (async()=>{await import(workerData.module);const s=new MoviesPointerSnapshot(workerData.buffer),gate=new Int32Array(workerData.gate),original=Atomics.compareExchange;let paused=false;
 Atomics.compareExchange=(array,...args)=>{if(array instanceof BigUint64Array&&!paused){paused=true;parentPort.postMessage('loaded-old-generation');Atomics.wait(gate,0,0,2000);}return original(array,...args);};
 parentPort.postMessage({warped:s.warp(500,600)});})();`,{eval:true,workerData:{module:new URL('../public/window-input.js',import.meta.url).href,buffer,gate}});
 try{await once(worker,'message');snapshot.setLocked(null);snapshot.setLocked({x:30,y:40});const result=once(worker,'message');Atomics.store(state,0,1);Atomics.notify(state,0);assert.deepEqual((await result)[0],{warped:false});assert.deepEqual(snapshot.read(),{x:30,y:40});}finally{await worker.terminate();}
});
test('capture Promise rejection retains exception detail across generic pointerlockerror',async()=>{
 const {host,canvas,document,snapshot,status}=fixture();snapshot.write({x:40,y:50});
 canvas.requestPointerLock=()=>Promise.reject(Object.assign(new Error('embedding policy refused capture'),{name:'NotAllowedError'}));
 assert.equal(host.request(),true);await new Promise(resolve=>setImmediate(resolve));
 document.fire('pointerlockerror');assert.match(status.at(-1).message,/NotAllowedError: embedding policy refused capture/);assert.equal(status.at(-1).diagnostic.promise,true);
 assert.equal(status.at(-1).lost,false);assert.deepEqual(snapshot.read(),{x:40,y:50});host.close();
});
test('DI fractional motion resets on capture transition without dropping held keys',()=>{
 const {env,canvas,document,host,snapshot}=fixture();canvas.getBoundingClientRect=()=>({width:300,height:300});snapshot.write({x:40,y:50});
 const buffer=MoviesWindowInput.create(16,MoviesDirectInputFields),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes),queue=new MoviesWindowInput(buffer,MoviesDirectInputFields);
 const di=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);di.attach(canvas,42);canvas.fire('focus');canvas.fire('keydown',{code:'KeyW'});
 canvas.fire('pointermove',{pointerType:'mouse',movementX:1,movementY:0});queue.drain(16);host.request();
 canvas.fire('pointermove',{pointerType:'mouse',movementX:1,movementY:0});assert.equal(queue.drain(16)[0].data,2);assert.deepEqual(snapshot.read(),{x:42,y:50});
 canvas.fire('keyup',{code:'KeyW'});assert.equal(queue.drain(16)[0].data,0);document.exitPointerLock();host.close();di.dispose();
});
