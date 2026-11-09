import test from 'node:test';
import assert from 'node:assert/strict';
import {once} from 'node:events';
import {Worker, MessageChannel} from 'node:worker_threads';
import {MoviesGraphicsClient} from '../public/graphics-client.js';
import {GRAPHICS_LIMITS, HEADER, validateCommand, responseViews, publishResponse, consumeResponse} from '../public/graphics-protocol.js';
globalThis.Worker = class extends Worker { constructor(url, options) { super(url, {...options, workerData: {mode: new URL(url).searchParams.get('mode')}}); } };
function clientOptions(mode = '') {
  const channel = new MessageChannel(); channel.port1.unref();
  const url = new URL('./fixtures/graphics-transport-worker.mjs', import.meta.url); url.searchParams.set('mode', mode);
  return {width: 1, height: 1, responseBytes: 1024, timeoutMs: mode === 'timeout' ? 150 : 2000,
    presentationPort: channel.port2, presentationAck: new SharedArrayBuffer(8), workerURL: url};
}
test('real separate-thread SAB response while caller Atomics.waits; output copy survives next command', () => {
  const c = new MoviesGraphicsClient(clientOptions());
  assert.equal(c.capabilities.backend, 'transport-test-only');
  const input = new Uint8Array(48); input[0] = 77;
  assert.equal(c.uploadVertices(input), 1); assert.equal(input.byteLength, 48); assert.equal(input[0], 77);
  const first = c.readPixels(); assert.deepEqual(Array.from(first.rgba), [77, 20, 30, 255]);
  input[0] = 12; c.uploadVertices(input); assert.equal(c.readPixels().rgba[0], 12); assert.equal(first.rgba[0], 77);
  assert.throws(() => c.batch([{command: 'clear'}, {command: 'inventedSuccess'}]), /Unsupported/);
  assert.equal(c.readPixels().rgba[0], 12);
  assert.throws(() => c.present(), /intentional transport error/);
  assert.throws(() => c.readPixels(), /closed/);
});
test('dead worker times out and bad reply serial is rejected', () => {
  const started = performance.now();
  assert.throws(() => new MoviesGraphicsClient(clientOptions('timeout')), /timeout/);
  assert.ok(performance.now() - started < 2000);
  assert.throws(() => new MoviesGraphicsClient(clientOptions('serial')), /serial/);
});
test('protocol validates complete batches and total upload size before dispatch', () => {
  assert.throws(() => validateCommand('headlessPresent'), /Unsupported/);
  assert.throws(() => validateCommand('batch', {commands: [{command: 'batch', args: {commands: []}}]}), /Nested/);
  assert.throws(() => validateCommand('batch', {commands: new Array(GRAPHICS_LIMITS.maxBatchCommands + 1).fill({command: 'clear'})}), /batch size/);
  assert.throws(() => validateCommand('batch', {commands: [{command: 'readPixels'}]}), /not allowed/);
  assert.throws(() => validateCommand('clear', {imaginaryStencil: 7}), /fields/);
  assert.throws(() => validateCommand('uploadVertices', {bytes: new SharedArrayBuffer(48), format: 'XYZ_DIFFUSE'}), /owned/);
  assert.throws(() => validateCommand('drawIndexed', {vertices: 1, indices: 2, world: [1]}), /transform/);
  const bytes = new ArrayBuffer(GRAPHICS_LIMITS.maxUploadBytes / 2 + 16);
  assert.throws(() => validateCommand('batch', {commands: [0, 1].map(() => ({command: 'uploadVertices', args: {bytes, format: 'XYZ_DIFFUSE'}}))}), /budget/);
});
test('response payload ranges, pixel dimensions and serial are checked', () => {
  const v = responseViews(new SharedArrayBuffer(64 + 1024));
  publishResponse(v, 1, {width: 1, height: 1, rgba: new Uint8Array([1, 2, 3, 255])});
  assert.deepEqual(Array.from(consumeResponse(v, 1).rgba), [1, 2, 3, 255]);
  assert.throws(() => consumeResponse(v, 2), /serial/);
  Atomics.store(v.header, HEADER.length, 1025); assert.throws(() => consumeResponse(v, 1), /length/);
  Atomics.store(v.header, HEADER.length, 4); Atomics.store(v.header, HEADER.width, 2); assert.throws(() => consumeResponse(v, 1), /size/);
  assert.throws(() => responseViews(new SharedArrayBuffer(64)), /capacity/);
});

test('page-prepared MessagePort path completes synchronous readback without owner callbacks', async () => {
  const worker = new Worker(new URL('./fixtures/graphics-transport-worker.mjs', import.meta.url));
  const channel = new MessageChannel(); const connected = once(worker, 'message');
  worker.postMessage({type: 'connect', port: channel.port2}, [channel.port2]); await connected;
  const options = {...clientOptions(), commandPort: channel.port1, responseBuffer: new SharedArrayBuffer(64 + 1024)};
  const client = new MoviesGraphicsClient(options);
  try { assert.deepEqual(Array.from(client.readPixels().rgba), [0, 20, 30, 255]); client.dispose(); }
  finally { client.close(); await worker.terminate(); }
});

test('profiled client emits bounded windows and preserves synchronous result despite diagnostic callback errors',()=>{
 const reports=[];const c=new MoviesGraphicsClient({...clientOptions('profile'),profile:true,onMetrics:r=>{reports.push(r);throw Error('telemetry only');}});
 try{
  assert.equal(c.capabilities.profile,true);
  for(let i=0;i<121;i++){c.uploadVertices(new Uint8Array(48));const result=c.present();assert.equal(result.acknowledged,true);}
  assert.equal(reports.length,1);const r=reports[0];assert.equal(r.frames,120);assert.equal(r.values.bridgeMs.count,240);assert.equal(r.values.presentMs.count,120);assert.equal(r.values.uploadVerticesMs.count,120);assert.equal(r.values.uploadBytes.sum,120*48);assert.deepEqual(r.worker,{fixtureOnly:true});
 }finally{c.close();}
});
test('pipelined post returns immediately and its reply is checked at the next exchange', () => {
  const c = new MoviesGraphicsClient(clientOptions());
  const seen = [];
  c.post('batch', {commands: [{command: 'clear'}]}, result => seen.push(result));
  assert.deepEqual(seen, [], 'nothing waited for at post time');
  c.post('batch', {commands: [{command: 'clear'}]}, result => seen.push(['second', result]));
  assert.deepEqual(seen, [null], 'a second post settles the first');
  assert.equal(c.readPixels().width, 1); assert.deepEqual(seen, [null, ['second', null]]);
  c.post('batch', {commands: [{command: 'clear'}]}, () => { throw new Error('verification failed'); });
  assert.throws(() => c.readPixels(), /verification failed/);
  assert.throws(() => c.readPixels(), /closed/);
});
