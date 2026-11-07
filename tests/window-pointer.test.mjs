import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import '../public/window-input.js';
import '../public/directinput-host.js';
class Target {
 constructor(){this.listeners=new Map();}
 addEventListener(n,f){const a=this.listeners.get(n)||[];a.push(f);this.listeners.set(n,a);}
 removeEventListener(n,f){this.listeners.set(n,(this.listeners.get(n)||[]).filter(x=>x!==f));}
 fire(n,e={}){for(const f of this.listeners.get(n)||[])f({type:n,preventDefault(){},...e});}
}
function fixture(){
 const source=fs.readFileSync(new URL('../public/static-probe.js',import.meta.url),'utf8');
 const begin=source.indexOf('// Observe genuine page input'),end=source.indexOf('function showDialog(data)',begin);
 assert.ok(begin>=0&&end>begin);
 const document=new Target(),env=new Target();document.hidden=false;env.document=document;
 const surface={windows:new Map(),geometry:new Map()},snapshot=new MoviesPointerSnapshot(MoviesPointerSnapshot.create());
 const context=vm.createContext({document,surface,cursorSnapshot:snapshot,observedPointer:null,addEventListener:(...a)=>env.addEventListener(...a)});
 vm.runInContext(source.slice(begin,end),context);
 let rect={left:100,top:200,width:0,height:0};const canvas=new Target();
 Object.assign(canvas,{width:800,height:600,hidden:false,getBoundingClientRect:()=>rect});
 return {document,env,surface,snapshot,context,canvas,setRect:r=>rect=r};
}
test('genuine prewindow launch click survives blur/leave/hidden and maps once on show',()=>{
 const f=fixture();const {document,env,surface,snapshot,context}=f;
 document.fire('pointerdown',{pointerType:'mouse',clientX:50,clientY:80});
 env.fire('blur');document.fire('pointerleave');document.hidden=true;document.fire('visibilitychange');
 assert.equal(snapshot.read(),null);
 surface.windows.set(42,f.canvas);surface.geometry.set(42,{clientX:16,clientY:39});context.publishPointer();assert.equal(snapshot.read(),null);
 f.setRect({left:100,top:200,width:400,height:300});context.publishPointer();assert.deepEqual(snapshot.read(),{x:-84,y:-201});
 f.setRect({left:300,top:400,width:800,height:600});surface.geometry.set(42,{clientX:50,clientY:70});
 env.fire('resize');document.fire('scroll');assert.deepEqual(snapshot.read(),{x:-84,y:-201}); // no invented motion while unfocused
 document.hidden=false;document.fire('pointermove',{pointerType:'mouse',clientX:310,clientY:420});assert.deepEqual(snapshot.read(),{x:60,y:90});
 document.fire('pointermove',{pointerType:'touch',clientX:0,clientY:0});assert.deepEqual(snapshot.read(),{x:60,y:90});
 env.fire('blur');surface.geometry.set(42,{clientX:50,clientY:70,minimized:true});f.setRect({left:0,top:0,width:0,height:0});context.publishPointer();assert.deepEqual(snapshot.read(),{x:60,y:90});
 context.cursorSnapshot=new MoviesPointerSnapshot(MoviesPointerSnapshot.create());context.publishPointer();assert.deepEqual(context.cursorSnapshot.read(),{x:60,y:90}); // same-page virtual position retained, not stale physical remap
});
test('no initial genuine point remains unavailable; cursor retention does not retain focus or held input',()=>{
 const f=fixture();const {document,env,surface,snapshot,context,canvas}=f;
 surface.windows.set(42,canvas);surface.geometry.set(42,{clientX:16,clientY:39});f.setRect({left:100,top:200,width:400,height:300});
 env.fire('blur');document.fire('pointerleave');context.publishPointer();assert.equal(snapshot.read(),null);
 const buffer=MoviesWindowInput.create(32,MoviesDirectInputFields),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes);
 const host=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);document.activeElement=canvas;host.attach(canvas,42);canvas.fire('focus');
 document.fire('pointerdown',{pointerType:'mouse',clientX:110,clientY:220});canvas.fire('pointerdown',{pointerType:'mouse',button:0});canvas.fire('keydown',{code:'KeyW'});
 assert.equal(new Int32Array(control)[1],42);assert.ok(host.held.size);assert.deepEqual(snapshot.read(),{x:36,y:79});
 env.fire('blur');assert.equal(new Int32Array(control)[1],0);assert.equal(host.held.size,0);assert.deepEqual(snapshot.read(),{x:36,y:79});
 assert.equal(MoviesReadAsyncKeyState(new Int32Array(control),1),0);host.dispose();
});

test('worker cursor read waits for a real concurrent odd-to-even publication',async()=>{
 const {Worker}=await import('node:worker_threads');
 const {once}=await import('node:events');
 const buffer=MoviesPointerSnapshot.create(),snapshot=new MoviesPointerSnapshot(buffer);
 snapshot.write({x:12,y:34});
 const writer=new Worker(`const {workerData,parentPort}=require('node:worker_threads');
 const state=new Int32Array(workerData);Atomics.add(state,0,1);Atomics.store(state,2,-123);
 parentPort.postMessage('writing');setTimeout(()=>{Atomics.store(state,3,456);Atomics.store(state,1,1);Atomics.add(state,0,1);Atomics.notify(state,0);},25);`,{eval:true,workerData:buffer});
 try {
  await once(writer,'message');assert.equal(snapshot.read(),undefined);
  assert.deepEqual(snapshot.readBlocking(500),{x:-123,y:456});
 } finally {await writer.terminate();}
});

test('worker cursor read distinguishes unavailable from a stalled publisher with bounded wait',()=>{
 const buffer=MoviesPointerSnapshot.create(),snapshot=new MoviesPointerSnapshot(buffer);
 assert.equal(snapshot.readBlocking(),null);
 Atomics.add(new Int32Array(buffer),0,1);
 const start=performance.now();assert.throws(()=>snapshot.readBlocking(20),/publisher stalled/);
 assert.ok(performance.now()-start>=15);assert.ok(performance.now()-start<500);
 assert.throws(()=>snapshot.readBlocking(Infinity),/deadline/);
});
