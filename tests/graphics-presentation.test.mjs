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
test('consumer errors are acknowledged as failures and overlapping frames rejected', () => {
  const f=fixture(()=>{throw new Error('draw failed');});
  f.presenter.submit({bitmap:f.bitmap,serial:7});
  assert.throws(()=>f.presenter.submit({bitmap:{close(){}},serial:8}),/overlapping/);
  f.frames.values().next().value();
  assert.equal(Atomics.load(f.ack,0),7);assert.equal(Atomics.load(f.ack,1),0);assert.equal(f.errors[0].message,'draw failed');assert.equal(f.closed(),1);f.presenter.close();
});
test('hidden/suspended animation callbacks fail after bounded deadline', async () => {
  const f=fixture(()=>assert.fail('must not draw'),10);f.presenter.submit({bitmap:f.bitmap,serial:1});
  await new Promise(resolve=>setTimeout(resolve,30));
  assert.equal(Atomics.load(f.ack,0),1);assert.equal(Atomics.load(f.ack,1),0);assert.match(f.errors[0].message,/timeout/);assert.equal(f.closed(),1);assert.equal(f.frames.size,0);f.presenter.close();
});
