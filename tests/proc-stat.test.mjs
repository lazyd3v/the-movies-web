import { test } from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../public/proc-stat.js', import.meta.url), 'utf8');
const context = vm.createContext({ TextEncoder });
vm.runInContext(source, context);
const { install } = context.MoviesProcStat;
function filesystem() {
  const FS = {
    ErrnoError: class extends Error { constructor(errno) { super(String(errno)); this.errno = errno; } },
    mkdirTree(path) { assert.equal(path, '/proc'); },
    writeFile(path, data, options) {
      assert.equal(path, '/proc/stat');
      assert.equal(options.mode, 0o444);
      this.initial = data;
      this.node = { mode: 0o100666, node_ops: { getattr: node => ({ mode: node.mode, size: 0 }) }, stream_ops: {} };
    },
    lookupPath(path) { assert.equal(path, '/proc/stat'); return { node: this.node }; }
  };
  return FS;
}
function opened(node) {
  const stream = { node, position: 0 };
  node.stream_ops.open(stream);
  return stream;
}
function contents(stream, start = 0) {
  const buffer = new Uint8Array(10000);
  const count = stream.node.stream_ops.read(stream, buffer, 0, buffer.length, start);
  return new TextDecoder().decode(buffer.subarray(0, count));
}
test('reports synthetic idle CPU ticks with correct aggregate and monotonic counters', () => {
  let clock = 1000;
  const { node, cpuCount } = install(filesystem(), { cpuCount: 3, now: () => clock });
  assert.equal(cpuCount, 3);
  clock += 250;
  const text = contents(opened(node));
  assert.match(text, /^cpu 0 0 0 75 0 0 0 0 0 0\ncpu0 0 0 0 25 /);
  assert.match(text, /cpu2 0 0 0 25 /);
  assert.equal(text.split('\n').filter(line => /^cpu\d/.test(line)).length, 3);
  assert.match(text, /btime 0\n/);
  clock -= 100;
  assert.equal(contents(opened(node)), text);
  clock += 200;
  assert.match(contents(opened(node)), /^cpu 0 0 0 105 /);
});
test('each open retains a consistent snapshot even when time advances between reads', () => {
  let clock = 0;
  const { node } = install(filesystem(), { cpuCount: 2, now: () => clock });
  const first = opened(node);
  const before = contents(first);
  clock = 100000;
  assert.equal(contents(first), before);
  assert.notEqual(contents(opened(node)), before);
  const prefix = new Uint8Array(8);
  assert.equal(node.stream_ops.read(first, prefix, 0, 8, 0), 8);
  assert.equal(new TextDecoder().decode(prefix) + contents(first, 8), before);
});
test('read-only metadata, CPU bounds, EOF and seek behavior', () => {
  for (const [input, expected] of [[0, 1], [-5, 1], [1000, 64], [3.9, 3]]) {
    assert.equal(install(filesystem(), { cpuCount: input, now: () => 0 }).cpuCount, expected);
  }
  const { node } = install(filesystem(), { cpuCount: 1, now: () => 0 });
  const stream = opened(node);
  const text = contents(stream);
  assert.equal(node.mode & 0o777, 0o444);
  assert.equal(node.node_ops.getattr(node).size, text.length);
  assert.equal(node.stream_ops.read(stream, new Uint8Array(2), 0, 2, text.length), 0);
  assert.equal(node.stream_ops.llseek(stream, -1, 2), text.length - 1);
  stream.position = 10;
  assert.equal(node.stream_ops.llseek(stream, 4, 1), 14);
  assert.equal(node.stream_ops.llseek(stream, 3, 0), 3);
  assert.throws(() => node.stream_ops.llseek(stream, -1, 0), error => error.errno === 28);
  assert.throws(() => node.stream_ops.llseek(stream, 0, 7), error => error.errno === 28);
  assert.throws(() => node.stream_ops.write(), error => error.errno === 69);
});
