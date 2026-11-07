import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/window-input.js';
const event = n => ({hwnd: 0x0d000001, message: 0x200, wParam: 1, lParam: n, time: n, x: -2, y: 42});
test('shared input preserves FIFO across ring and unsigned counter wrap', () => {
  const buffer = MoviesWindowInput.create(4);
  const counters = new Int32Array(buffer);
  counters[0] = counters[1] = -2;
  const producer = new MoviesWindowInput(buffer), consumer = new MoviesWindowInput(buffer);
  for (let base = 0; base < 20; base += 3) {
    for (let n = base; n < base + 3; n++) assert.equal(producer.push(event(n)), true);
    assert.deepEqual(consumer.drain(1), [event(base)]);
    assert.deepEqual(consumer.drain(8), [event(base + 1), event(base + 2)]);
    assert.deepEqual(consumer.drain(1), []);
  }
});
test('input overflow and malformed buffers fail explicitly', () => {
  const queue = new MoviesWindowInput(MoviesWindowInput.create(2));
  assert.equal(queue.push(event(1)), true); assert.equal(queue.push(event(2)), true);
  assert.equal(queue.push(event(3)), false);
  assert.throws(() => queue.drain(2), /overflow/);
  assert.throws(() => MoviesWindowInput.create(3));
  assert.throws(() => new MoviesWindowInput(new ArrayBuffer(72)));
  assert.throws(() => queue.push({...event(0), x: NaN}));
  assert.throws(() => queue.drain(-1));
});
