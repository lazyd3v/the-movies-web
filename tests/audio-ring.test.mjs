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
test('DirectSound slots mix from shared guest memory and advance the play cursor with wrap', async () => {
  const {mixDirectSound, DSOUND_SLOT} = await import('../public/audio-ring.js');
  const memory = new SharedArrayBuffer(4096), control = 1024, samples = new Int16Array(memory), ctl = new Int32Array(memory, control, 16);
  const ring = 256; for (let f = 0; f < 4; f++) { samples[ring / 2 + f * 2] = 16384; samples[ring / 2 + f * 2 + 1] = -16384; }
  ctl[DSOUND_SLOT.STATE] = 1; ctl[DSOUND_SLOT.ADDRESS] = ring; ctl[DSOUND_SLOT.BYTES] = 16; ctl[DSOUND_SLOT.CHANNELS] = 2; ctl[DSOUND_SLOT.CURSOR] = 2; ctl[DSOUND_SLOT.VOLUME] = 10000;
  ctl[8 + DSOUND_SLOT.STATE] = 0;
  const outputs = [new Float32Array(6), new Float32Array(6)];
  mixDirectSound({samples, control: ctl, slots: 2}, outputs);
  assert.deepEqual([...outputs[0]], [0.5, 0.5, 0.5, 0.5, 0.5, 0.5]);
  assert.deepEqual([...outputs[1]], [-0.5, -0.5, -0.5, -0.5, -0.5, -0.5]);
  assert.equal(ctl[DSOUND_SLOT.CURSOR], 0); /* (2 + 6) % 4 */
  assert.equal(ctl[DSOUND_SLOT.HEARTBEAT], 1); assert.equal(ctl[8 + DSOUND_SLOT.HEARTBEAT], 1);
});
test('host streams stop at the written end: silence and a held cursor, never stale audio', async () => {
  const {mixDirectSound, DSOUND_SLOT} = await import('../public/audio-ring.js');
  const memory = new SharedArrayBuffer(4096), samples = new Int16Array(memory), ctl = new Int32Array(memory, 1024, 8);
  const ring = 256; for (let f = 0; f < 8; f++) { samples[ring / 2 + f * 2] = 16384; samples[ring / 2 + f * 2 + 1] = 16384; }
  Object.assign(ctl, {[DSOUND_SLOT.STATE]: 1, [DSOUND_SLOT.ADDRESS]: ring, [DSOUND_SLOT.BYTES]: 32, [DSOUND_SLOT.CHANNELS]: 2, [DSOUND_SLOT.CURSOR]: 6, [DSOUND_SLOT.VOLUME]: 10000});
  ctl[DSOUND_SLOT.LIMIT] = 0x80000000 | 1; // data written up to frame 1 (wrapping): frames 6, 7, 0 remain
  let outputs = [new Float32Array(5), new Float32Array(5)];
  mixDirectSound({samples, control: ctl, slots: 1}, outputs);
  assert.deepEqual([...outputs[0]], [0.5, 0.5, 0.5, 0, 0]);
  assert.equal(ctl[DSOUND_SLOT.CURSOR], 1, 'cursor waits at the end of written data');
  outputs = [new Float32Array(4), new Float32Array(4)];
  mixDirectSound({samples, control: ctl, slots: 1}, outputs);
  assert.deepEqual([...outputs[0]], [0, 0, 0, 0]); assert.equal(ctl[DSOUND_SLOT.CURSOR], 1);
  ctl[DSOUND_SLOT.LIMIT] = 0x80000000 | 0x40000000 | 1; // completely full ring
  outputs = [new Float32Array(3), new Float32Array(3)];
  mixDirectSound({samples, control: ctl, slots: 1}, outputs);
  assert.deepEqual([...outputs[0]], [0.5, 0.5, 0.5]); assert.equal(ctl[DSOUND_SLOT.CURSOR], 4);
});
