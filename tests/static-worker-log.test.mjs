import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';

test('synchronous guest API traces use bounded batches and flush before terminal', () => {
  const messages = [];
  const scope = vm.createContext({self:{}, postMessage:value=>messages.push(value)});
  vm.runInContext(readFileSync(new URL('../public/static-probe-worker.js',import.meta.url),'utf8'), scope);
  vm.runInContext(`for(let i=0;i<10000;i++) appendRuntimeLog('[import] HeapAlloc '+i); terminal(1,'fixture');`,scope);
  const packets = messages.filter(m=>m.type==='log');
  assert.ok(packets.length < 10, 'hot calls must not enqueue one UI task per API');
  assert.ok(packets.every(m=>m.text.length < 33000));
  const lines = packets.flatMap(m=>m.text.split('\n'));
  assert.equal(lines.length,10000);
  assert.equal(lines[0],'[import] HeapAlloc 0');
  assert.equal(lines.at(-1),'[import] HeapAlloc 9999');
  assert.equal(messages.at(-1).type,'terminal');
});
