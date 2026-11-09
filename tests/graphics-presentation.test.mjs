import test from 'node:test';
import assert from 'node:assert/strict';
import {createGraphicsPresenter} from '../public/graphics-presentation.js';
function fixture(consume = () => {}, timeoutMs = 1000) {
  const ack = new Int32Array(new SharedArrayBuffer(8)), frames = new Map(), errors = [];
  let next = 1, closed = 0;
  const presenter = createGraphicsPresenter({ack, consume, timeoutMs, requestFrame: fn => {const id=next++;frames.set(id,fn);return id;}, cancelFrame: id => frames.delete(id), onError: e => errors.push(e)});
  return {ack, presenter, frames, errors, bitmap: {close(){closed++;}}, closed: () => closed};
}
test('ONE acknowledgement occurs only after animation callback consumes bitmap', () => {
  let consumed=0;const f=fixture(()=>{consumed++;assert.equal(Atomics.load(f.ack,0),0);});
  f.presenter.submit({bitmap:f.bitmap,serial:1});
  assert.equal(consumed,0);assert.equal(Atomics.load(f.ack,0),0);assert.equal(f.closed(),0);
  f.frames.values().next().value();
  assert.equal(consumed,1);assert.equal(Atomics.load(f.ack,0),1);assert.equal(Atomics.load(f.ack,1),1);assert.equal(f.closed(),1);assert.equal(f.frames.size,0);
  f.presenter.close();
});
test('consumer errors are acknowledged as failures; a newer frame supersedes a pending one; stale serials are rejected', () => {
  const f=fixture(()=>{throw new Error('draw failed');});
  let firstClosed=0;f.presenter.submit({bitmap:{close(){firstClosed++;}},serial:7});
  f.presenter.submit({bitmap:f.bitmap,serial:8});
  assert.equal(firstClosed,1,'superseded frame is released');assert.equal(Atomics.load(f.ack,0),0,'superseded frame is not acknowledged');
  assert.throws(()=>f.presenter.submit({bitmap:{close(){}},serial:8}),/stale/);
  [...f.frames.values()].at(-1)();
  assert.equal(Atomics.load(f.ack,0),8);assert.equal(Atomics.load(f.ack,1),0);assert.equal(f.errors[0].message,'draw failed');assert.equal(f.closed(),1);f.presenter.close();
});
test('a page without animation frames still presents after the bounded deadline', async () => {
  let consumed=0;const f=fixture(()=>{consumed++;},10);f.presenter.submit({bitmap:f.bitmap,serial:1});
  await new Promise(resolve=>setTimeout(resolve,30));
  assert.equal(consumed,1);assert.equal(Atomics.load(f.ack,0),1);assert.equal(Atomics.load(f.ack,1),1);assert.equal(f.errors.length,0);assert.equal(f.closed(),1);f.presenter.close();
});
test('hidden pages consume frames immediately instead of timing Present out', () => {
  let hidden = true, consumed = 0; const listeners = new Map();
  const target = {addEventListener: (type, fn) => listeners.set(type, fn), removeEventListener: type => listeners.delete(type)};
  const ack = new Int32Array(new SharedArrayBuffer(8)), frames = new Map(); let next = 1;
  const presenter = createGraphicsPresenter({ack, consume: () => { consumed++; }, timeoutMs: 1000, isHidden: () => hidden, visibilityTarget: target,
    requestFrame: fn => { const id = next++; frames.set(id, fn); return id; }, cancelFrame: id => frames.delete(id)});
  presenter.submit({bitmap: {close() {}}, serial: 1});
  assert.equal(consumed, 1); assert.equal(Atomics.load(ack, 0), 1); assert.equal(Atomics.load(ack, 1), 1); assert.equal(frames.size, 0);
  hidden = false; presenter.submit({bitmap: {close() {}}, serial: 2});
  assert.equal(consumed, 1); assert.equal(frames.size, 1);
  hidden = true; listeners.get('visibilitychange')();
  assert.equal(consumed, 2); assert.equal(Atomics.load(ack, 0), 2); assert.equal(Atomics.load(ack, 1), 1); assert.equal(frames.size, 0);
  presenter.close(); assert.equal(listeners.size, 0);
});
test('three-word acknowledgement publishes page visibility for worker-side completion', () => {
  let hidden = false; const listeners = new Map();
  const ack = new Int32Array(new SharedArrayBuffer(12));
  const presenter = createGraphicsPresenter({ack, consume: () => {}, isHidden: () => hidden,
    visibilityTarget: {addEventListener: (t, f) => listeners.set(t, f), removeEventListener: t => listeners.delete(t)},
    requestFrame: () => 1, cancelFrame: () => {}});
  assert.equal(Atomics.load(ack, 2), 0);
  hidden = true; listeners.get('visibilitychange')(); assert.equal(Atomics.load(ack, 2), 1);
  hidden = false; listeners.get('visibilitychange')(); assert.equal(Atomics.load(ack, 2), 0);
  presenter.close();
  assert.throws(() => createGraphicsPresenter({ack: new Int32Array(new SharedArrayBuffer(16)), consume: () => {}, requestFrame: () => 1, cancelFrame: () => {}}), /Invalid graphics presenter/);
});
