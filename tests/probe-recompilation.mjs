// Tests genuine translated leaf code, without copying original game bytes here.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const bytes = await readFile(process.argv[2]);
const module = await WebAssembly.compile(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), [], 'Probe must be self-contained');
const { exports: wasm } = await WebAssembly.instantiate(module);
if (wasm._initialize) wasm._initialize();
const memory = new Uint8Array(wasm.memory.buffer);
let checks = 0;
function put(index, values) {
  assert(values.length < 65536);
  const start = wasm.probe_input(index);
  memory.fill(0xa5, start, start + 65536);
  memory.set(values, start);
  memory[start + values.length] = 0;
}
function terminated(values) {
  const zero = values.indexOf(0);
  return zero < 0 ? values : values.slice(0, zero);
}
function compare(a, b) {
  a = terminated(a); b = terminated(b);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = a[i] ?? 0, y = b[i] ?? 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}
const samples = [[], [0], [65], [65, 66], [65, 0, 90], [127], [128], [255],
  [255, 128, 127, 1], Array(4095).fill(97), Array(65534).fill(255)];
let seed = 0x19aa3456;
function random() { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed >>> 0; }
for (let i = 0; i < 256; i++) samples.push(Array.from({ length: random() % 257 }, () => random() & 255));
for (const a of samples) {
  put(0, a);
  assert.equal(wasm.probe_run(0), terminated(a).length); checks++;
  for (const b of [a, [], [128, 255], samples[random() % samples.length]]) {
    put(0, a); put(1, b);
    assert.equal(wasm.probe_run(1), compare(a, b)); checks++;
  }
}
console.log(JSON.stringify({ passed: checks, sampleCount: samples.length,
  coverage: ['NUL termination', 'unsigned bytes', 'empty and long inputs', 'equal/prefix/different strings', 'seeded random inputs', 'callee-saved registers and guest stack'],
  performanceMeasured: false }));
