import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/window-input.js';
import '../public/directinput-host.js';
class Target {
  constructor() { this.listeners = new Map(); }
  addEventListener(type, fn) { const list=this.listeners.get(type)||[];list.push(fn);this.listeners.set(type,list); }
  removeEventListener(type, fn) { this.listeners.set(type,(this.listeners.get(type)||[]).filter(f=>f!==fn)); }
  fire(type, data={}) { for(const fn of this.listeners.get(type)||[]) fn({type,preventDefault(){},...data}); }
}
test('DirectInput producer uses physical keys, relative mouse deltas and actual focus', () => {
  const buffer=MoviesWindowInput.create(32,MoviesDirectInputFields),control=new SharedArrayBuffer(8);
  const reader=new MoviesWindowInput(buffer,MoviesDirectInputFields),state=new Int32Array(control);
  const env=new Target(),canvas=new Target();env.document={activeElement:canvas};
  Object.assign(canvas,{width:800,height:600,hidden:false,getBoundingClientRect:()=>({width:400,height:300})});
  const host=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);
  host.attach(canvas,42);assert.equal(state[0],1);assert.equal(state[1],0);
  canvas.fire('keydown',{code:'KeyW'});assert.deepEqual(reader.drain(10),[]);
  canvas.fire('focus');assert.equal(state[1],42);
  canvas.fire('keydown',{code:'KeyW'});canvas.fire('keydown',{code:'KeyW',repeat:true});
  canvas.fire('keyup',{code:'KeyW'});
  canvas.fire('pointermove',{pointerType:'mouse',movementX:3,movementY:-2,clientX:10,clientY:20});
  canvas.fire('pointerdown',{pointerType:'mouse',button:2});
  canvas.fire('pointerup',{pointerType:'mouse',button:2});
  canvas.fire('wheel',{deltaY:3,deltaMode:1});
  const events=reader.drain(32);
  assert.deepEqual(events.map(({time,...event})=>event),[
    {deviceKind:1,offset:17,data:128},{deviceKind:1,offset:17,data:0},
    {deviceKind:2,offset:0,data:6},{deviceKind:2,offset:4,data:4294967292},
    {deviceKind:2,offset:13,data:128},{deviceKind:2,offset:13,data:0},
    {deviceKind:2,offset:8,data:4294967176},
  ]);
  const now=Math.floor(performance.timeOrigin+performance.now())>>>0;
  for(const event of events)assert.ok(((now-event.time)>>>0)<1000);
  env.fire('blur');assert.equal(state[1],0);
  canvas.fire('keydown',{code:'KeyA'});assert.deepEqual(reader.drain(10),[]);
  env.fire('focus');assert.equal(state[1],42);
  host.dispose();assert.equal(state[0],0);assert.equal(state[1],0);
  canvas.fire('focus');assert.equal(state[1],0);
});
test('physical modifier and navigation scan codes remain distinct', () => {
  assert.equal(MoviesDirectInputKeys.ControlLeft,0x1d);
  assert.equal(MoviesDirectInputKeys.ControlRight,0x9d);
  assert.equal(MoviesDirectInputKeys.Enter,0x1c);
  assert.equal(MoviesDirectInputKeys.NumpadEnter,0x9c);
  assert.equal(MoviesDirectInputKeys.ArrowUp,0xc8);
  assert.equal(MoviesDirectInputKeys.Numpad8,0x48);
});

test('focus state excludes hidden/background documents and clears on visibility change',()=>{
 const env=new Target(),doc=new Target(),canvas=new Target();let browserFocused=true;
 Object.assign(doc,{activeElement:canvas,hidden:false,hasFocus:()=>browserFocused});env.document=doc;
 Object.assign(canvas,{hidden:false,width:800,height:600,getBoundingClientRect:()=>({width:800,height:600})});
 const control=new SharedArrayBuffer(8),state=new Int32Array(control),host=new MoviesDirectInputHost(MoviesWindowInput.create(16,MoviesDirectInputFields),control,()=>assert.fail('overflow'),env);
 host.attach(canvas,42);canvas.fire('focus');assert.equal(state[1],42);
 browserFocused=false;canvas.fire('focus');assert.equal(state[1],0);
 browserFocused=true;env.fire('focus');assert.equal(state[1],42);
 doc.hidden=true;doc.fire('visibilitychange');assert.equal(state[1],0);
 doc.hidden=false;doc.fire('visibilitychange');assert.equal(state[1],42);
 doc.activeElement={};env.fire('focus');assert.equal(state[1],0);host.dispose();
});

test('async VK queries consume only their edge bit, preserve DI events, and combine Enter keys',()=>{
 const env=new Target(),doc=new Target(),canvas=new Target();Object.assign(doc,{activeElement:canvas,hidden:false,hasFocus:()=>true});env.document=doc;
 Object.assign(canvas,{hidden:false,width:800,height:600,getBoundingClientRect:()=>({width:800,height:600})});
 const buffer=MoviesWindowInput.create(64,MoviesDirectInputFields),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes),state=new Int32Array(control),reader=new MoviesWindowInput(buffer,MoviesDirectInputFields);
 const host=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);host.attach(canvas,42);canvas.fire('focus');
 const query=vk=>MoviesReadAsyncKeyState(state,vk);
 for(const [code,vk] of [['Escape',0x1b],['Space',0x20],['Enter',0x0d],['PageUp',0x21],['PageDown',0x22],['ArrowLeft',0x25],['ArrowRight',0x27],['ArrowUp',0x26],['ArrowDown',0x28]]) {
  canvas.fire('keydown',{code});assert.equal(query(vk),0x8001);assert.equal(query(vk),0x8000);
  canvas.fire('keydown',{code,repeat:true});assert.equal(query(vk),0x8000);
  canvas.fire('keyup',{code});assert.equal(query(vk),0);
  canvas.fire('keydown',{code});canvas.fire('keyup',{code});assert.equal(query(vk),1);assert.equal(query(vk),0);
 }
 assert.equal(reader.drain(64).length,36,'USER32 queries must not consume DirectInput events');
 canvas.fire('keydown',{code:'Enter'});canvas.fire('keydown',{code:'NumpadEnter'});assert.equal(query(0x0d),0x8001);
 canvas.fire('keyup',{code:'Enter'});assert.equal(query(0x0d),0x8000);canvas.fire('keyup',{code:'NumpadEnter'});assert.equal(query(0x0d),0);
 canvas.fire('keydown',{code:'Escape'});env.fire('blur');assert.equal(query(0x1b),0);env.fire('focus');assert.equal(query(0x1b),0);
 canvas.fire('keydown',{code:'Space'});doc.hidden=true;doc.fire('visibilitychange');assert.equal(query(0x20),0);doc.hidden=false;doc.fire('visibilitychange');assert.equal(query(0x20),0);
 assert.throws(()=>query(0x41),/Unsupported/);assert.throws(()=>MoviesReadAsyncKeyState(new Int32Array(new SharedArrayBuffer(8)),0x1b),/unavailable/);host.dispose();assert.equal(query(0x0d),0);
});

test('mouse async states track physical buttons independently from DI queue',()=>{
 const env=new Target(),doc=new Target(),canvas=new Target();Object.assign(doc,{activeElement:canvas,hidden:false,hasFocus:()=>true});env.document=doc;
 Object.assign(canvas,{hidden:false,width:800,height:600,getBoundingClientRect:()=>({width:800,height:600})});
 const buffer=MoviesWindowInput.create(64,MoviesDirectInputFields),control=new SharedArrayBuffer(MoviesAsyncKeyControlBytes),state=new Int32Array(control);
 const host=new MoviesDirectInputHost(buffer,control,()=>assert.fail('overflow'),env);host.attach(canvas,42);canvas.fire('focus');
 for(const [button,vk] of [[0,1],[1,4],[2,2]]) {
  canvas.fire('pointerdown',{pointerType:'mouse',button});assert.equal(MoviesReadAsyncKeyState(state,vk),0x8001);
  assert.equal(MoviesReadAsyncKeyState(state,vk),0x8000);canvas.fire('pointerup',{pointerType:'mouse',button});assert.equal(MoviesReadAsyncKeyState(state,vk),0);
 }
 assert.equal(new MoviesWindowInput(buffer,MoviesDirectInputFields).drain(64).length,6);
 canvas.fire('pointerdown',{pointerType:'mouse',button:0});canvas.fire('pointercancel');assert.equal(MoviesReadAsyncKeyState(state,1),0);host.dispose();
});
test('pointer snapshot requires genuine input and publishes signed coherent POINTs',()=>{
 const buffer=MoviesPointerSnapshot.create(),snapshot=new MoviesPointerSnapshot(buffer);assert.equal(snapshot.read(),null);
 snapshot.write({x:-123,y:456});assert.deepEqual(snapshot.read(),{x:-123,y:456});
 const state=new Int32Array(buffer);Atomics.add(state,0,1);assert.equal(snapshot.read(),undefined);Atomics.add(state,0,1);
 snapshot.write(null);assert.equal(snapshot.read(),null);assert.throws(()=>snapshot.write({x:2147483648,y:0}),/Invalid/);
});

test('sync recovers focus missed by events and clears it when the page loses focus',()=>{
 const env=new Target(),doc=new Target(),canvas=new Target();let browserFocused=false;
 Object.assign(doc,{activeElement:canvas,hidden:false,hasFocus:()=>browserFocused});env.document=doc;
 Object.assign(canvas,{hidden:false,width:8,height:8,getBoundingClientRect:()=>({width:8,height:8})});
 const control=new SharedArrayBuffer(8),state=new Int32Array(control),host=new MoviesDirectInputHost(MoviesWindowInput.create(16,MoviesDirectInputFields),control,()=>assert.fail('overflow'),env);
 host.attach(canvas,42);canvas.fire('focus');assert.equal(state[1],0);
 browserFocused=true;assert.equal(host.sync(),42);assert.equal(state[1],42);
 browserFocused=false;assert.equal(host.sync(),0);assert.equal(state[1],0);
 browserFocused=true;doc.activeElement={};assert.equal(host.sync(),0);host.dispose();
});

test('full input queues drop events instead of poisoning the consumer, reserving room for buttons',()=>{
 const buffer=MoviesWindowInput.create(8,MoviesDirectInputFields),queue=new MoviesWindowInput(buffer,MoviesDirectInputFields),reader=new MoviesWindowInput(buffer,MoviesDirectInputFields);
 const e=(offset)=>({deviceKind:2,offset,data:1,time:1});
 let accepted=0;for(let i=0;i<20;i++)if(queue.offer(e(0),2))accepted++;
 assert.equal(accepted,6);
 assert.ok(queue.offer(e(12)));assert.ok(queue.offer(e(12)));assert.equal(queue.offer(e(12)),false);
 const drained=reader.drain(16);assert.equal(drained.length,8);assert.deepEqual(drained.slice(-2).map(x=>x.offset),[12,12]);
 assert.ok(queue.offer(e(0),2));assert.equal(reader.drain(16).length,1);
 assert.throws(()=>queue.offer(e(0),8),/reserve/);
});
