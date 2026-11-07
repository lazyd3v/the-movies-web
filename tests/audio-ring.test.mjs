import test from 'node:test';import assert from 'node:assert/strict';
import {AUDIO,createAudioRing,audioViews,MoviesAudioProducer,consumeAudioQuantum} from '../public/audio-ring.js';
function setup(channels=2,capacityFrames=256){const d=createAudioRing({channels,capacityFrames}),p=new MoviesAudioProducer(d),r=audioViews(d),out=[new Float32Array(128),new Float32Array(128)];return {p,r,out,c:r.control};}
test('audio completion follows actual rendering consumption, never enqueue',()=>{
 const {p,r,out}=setup();const pcm=new Int16Array(256);pcm[0]=-32768;pcm[1]=32767;p.write(7,pcm);assert.deepEqual(p.poll().buffers,[]);consumeAudioQuantum(r,out);assert.equal(p.poll().consumedFrames,0);p.resume();consumeAudioQuantum(r,out);assert.equal(out[0][0],-1);assert.equal(out[1][0],32767/32768);const result=p.poll();assert.equal(result.consumedFrames,128);assert.equal(result.buffers[0].status,'played');assert.equal(result.buffers[0].id,7);assert.deepEqual(p.poll().buffers,[]);
});
test('pause preserves queued samples; underrun emits silence without phantom consumption',()=>{
 const {p,r,out}=setup(1);p.write(1,new Int16Array(256).fill(16384));p.resume();consumeAudioQuantum(r,out);p.pause();const before=p.poll();consumeAudioQuantum(r,out);assert.equal(p.poll().consumedFrames,before.consumedFrames);assert(out[0].every(x=>x===0));p.resume();consumeAudioQuantum(r,out);assert.equal(p.poll().buffers[0].status,'played');consumeAudioQuantum(r,out);assert.equal(p.poll().consumedFrames,256);assert.equal(p.poll().underrunFrames,128);assert(out[0].every(x=>x===0));
});
test('reset acknowledgement distinguishes consumed and cancelled headers, including publication race',()=>{
 const {p,r,out,c}=setup();p.write(1,new Int16Array(256));p.write(2,new Int16Array(256));p.resume();consumeAudioQuantum(r,out);p.requestReset();assert.throws(()=>p.write(3,new Int16Array(2)),/reset/);
 // Model render thread between READ flush and RESET_ACK publication.
 Atomics.store(c,AUDIO.RESET_READ,128);Atomics.store(c,AUDIO.READ,256);assert.deepEqual(p.poll().buffers,[]);Atomics.store(c,AUDIO.READ,128);consumeAudioQuantum(r,out);
 const result=p.poll();assert(result.resetDone);assert.deepEqual(result.buffers.map(x=>[x.id,x.status]),[[1,'played'],[2,'reset']]);assert.equal(result.consumedFrames,128);p.write(3,new Int16Array(256));consumeAudioQuantum(r,out);assert.equal(p.poll().buffers[0].id,3);
});
test('queue rejects overflow and duplicate ids without mutation; counters wrap safely',()=>{
 const {p,r,out,c}=setup();Atomics.store(c,AUDIO.READ,-128);Atomics.store(c,AUDIO.WRITE,-128);p.write(1,new Int16Array(256));const before=Atomics.load(c,AUDIO.WRITE);assert.throws(()=>p.write(1,new Int16Array(2)),/duplicate/);assert.throws(()=>p.write(2,new Int16Array(514)),/full/);assert.equal(Atomics.load(c,AUDIO.WRITE),before);p.resume();consumeAudioQuantum(r,out);assert.equal(Atomics.load(c,AUDIO.READ),0);assert.equal(p.poll().buffers[0].status,'played');
});
test('close reports pending buffers cancelled and format bounds fail explicitly',()=>{
 const {p,r,out}=setup();p.write(10,new Int16Array(256));assert.equal(p.close()[0].status,'closed');assert.equal(consumeAudioQuantum(r,out),false);assert.throws(()=>p.write(11,new Int16Array(2)),/closed/);assert.throws(()=>createAudioRing({channels:3}));assert.throws(()=>createAudioRing({capacityFrames:129}));
});
