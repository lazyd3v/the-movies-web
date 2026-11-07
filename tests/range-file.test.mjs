import { test } from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../public/range-file.js', import.meta.url), 'utf8');
const context = vm.createContext({ Uint8Array, ArrayBuffer });
vm.runInContext(source, context);
const { createRangeReader, mount } = context.MoviesRangeFile;
const bytes = (url, start, end) => Uint8Array.from({ length: end - start + 1 }, (_, i) => (start + i) % 251);
const reader = options => createRangeReader({ url: '/game.zip', size: 20, chunkSize: 4, maxChunks: 2, request: bytes, ...options });

test('reads across chunks, clamps EOF, and preserves target bounds', () => {
  const r = reader({ size: 11 });
  const buffer = new Uint8Array(12).fill(255);
  assert.equal(r.read(buffer, 2, 8, 2), 8);
  assert.deepEqual([...buffer], [255, 255, 2, 3, 4, 5, 6, 7, 8, 9, 255, 255]);
  assert.equal(r.read(buffer, 0, 6, 9), 2);
  assert.deepEqual([...buffer.slice(0, 2)], [9, 10]);
  assert.equal(r.read(buffer, 0, 1, 100), 0);
  assert.equal(reader({ size: 0 }).read(buffer, 0, 1, 0), 0);
  assert.throws(() => r.read(buffer, 10, 3, 0), /bounds/);
  assert.throws(() => r.read(buffer, 0, 1, -1), /bounds/);
  assert.throws(() => r.read(buffer, 0, 1.5, 0), /bounds/);
});
test('LRU cache is bounded and touch updates recency', () => {
  const r = reader();
  const out = new Uint8Array(1);
  for (const at of [0, 4, 0, 8, 0]) r.read(out, 0, 1, at);
  assert.equal(r.stats.requests, 3);
  assert.equal(r.stats.hits, 2);
  assert.equal(r.stats.cacheBytes, 8);
  r.read(out, 0, 1, 4);
  assert.equal(r.stats.requests, 4);
  assert.equal(r.stats.cacheBytes, 8);
  assert.equal(r.stats.bytes, 16);
});
test('high offsets do not wrap to signed 32-bit integers', () => {
  const at = 2 ** 31 + 123;
  const r = reader({ size: at + 9 });
  const out = new Uint8Array(9);
  assert.equal(r.read(out, 0, out.length, at), 9);
  assert.deepEqual(out, bytes('', at, at + 8));
});
test('failed requests never poison the cache', () => {
  let attempts = 0;
  const r = reader({ request: (...args) => { attempts++; if (attempts === 1) return new Uint8Array(1); return bytes(...args); } });
  const out = new Uint8Array(2);
  assert.throws(() => r.read(out, 0, 2, 0), /exact requested/);
  assert.equal(r.stats.cacheBytes, 0);
  assert.equal(r.read(out, 0, 2, 0), 2);
  assert.equal(r.stats.requests, 2);
  for (const options of [{ size: -1 }, { size: 2 ** 53 }, { maxChunks: 0 }, { chunkSize: 0 }]) assert.throws(() => reader(options), /configuration/);
});
test('default XHR requires exact 206 range response and decodes binary text', () => {
  let response = { status: 206, contentRange: 'bytes 0-3/4', text: '\x00\x7f\uf780\uf7ff' };
  let instance;
  class XHR {
    constructor() { instance = this; }
    open(...args) { this.openArgs = args; }
    setRequestHeader(...args) { this.header = args; }
    overrideMimeType(value) { this.mime = value; }
    send() { this.status = response.status; this.responseText = response.text; }
    getResponseHeader(name) { return name === 'Content-Range' ? response.contentRange : 'application/zip'; }
  }
  const isolated = vm.createContext({ XMLHttpRequest: XHR, Uint8Array, ArrayBuffer });
  vm.runInContext(source, isolated);
  const make = () => isolated.MoviesRangeFile.createRangeReader({ url: '/x', size: 4 });
  const out = new Uint8Array(4);
  make().read(out, 0, 4, 0);
  assert.deepEqual([...out], [0, 127, 128, 255]);
  assert.deepEqual(instance.openArgs, ['GET', '/x', false]);
  assert.deepEqual(instance.header, ['Range', 'bytes=0-3']);
  assert.match(instance.mime, /x-user-defined/);
  response.status = 200;
  assert.throws(() => make().read(out, 0, 4, 0), /expected 206/);
  response.status = 206; response.contentRange = 'bytes 0-2/4';
  assert.throws(() => make().read(out, 0, 4, 0), /Content-Range/);
  response.contentRange = 'bytes 0-3/4'; response.text = 'x';
  assert.throws(() => make().read(out, 0, 4, 0), /Truncated/);
});
test('retries binary chunks that trigger browser BOM decoding using a preceding byte', () => {
  const raw = Uint8Array.from([80, 75, 3, 4, 255, 254, 128, 0, 255, 17, 19, 20]);
  const requests = [];
  class XHR {
    open() {}
    overrideMimeType() {}
    setRequestHeader(name, value) {
      assert.equal(name, 'Range');
      [this.start, this.end] = value.slice(6).split('-').map(Number);
      requests.push([this.start, this.end]);
    }
    send() {
      this.status = 206;
      const bytes = raw.slice(this.start, this.end + 1);
      this.responseText = bytes[0] === 255 && bytes[1] === 254
        ? '\u0080' // BOM sniffing decodes the remaining bytes as UTF-16LE.
        : Array.from(bytes, byte => String.fromCharCode(byte < 128 ? byte : 0xf700 + byte)).join('');
    }
    getResponseHeader(name) { return name === 'Content-Range' ? `bytes ${this.start}-${this.end}/${raw.length}` : 'application/zip'; }
  }
  const isolated = vm.createContext({ XMLHttpRequest: XHR, Uint8Array, ArrayBuffer });
  vm.runInContext(source, isolated);
  const r = isolated.MoviesRangeFile.createRangeReader({ url: '/x', size: raw.length, chunkSize: 4 });
  const out = new Uint8Array(4);
  assert.equal(r.read(out, 0, 4, 4), 4);
  assert.deepEqual(out, raw.slice(4, 8));
  assert.deepEqual(requests, [[4, 7], [3, 7]]);
});
test('mount adds read-only file metadata and correct seek behavior', () => {
  const baseAttrs = { size: 0, blocks: 0, mode: 33060 };
  const preserved = () => {};
  let creation;
  const FS = {
    ErrnoError: class extends Error { constructor(errno) { super(String(errno)); this.errno = errno; } },
    createFile(...args) { creation = args; return { node_ops: { getattr: () => baseAttrs }, stream_ops: { close: preserved } }; }
  };
  const mounted = mount(FS, { path: '/movies.zip', url: '/game.zip', size: 2 ** 31 + 100, request: bytes });
  assert.equal(creation[0], '/'); assert.equal(creation[1], 'movies.zip');
  assert.equal(creation[3], true); assert.equal(creation[4], false);
  const attrs = mounted.node.node_ops.getattr(mounted.node);
  assert.equal(attrs.size, 2 ** 31 + 100);
  assert.equal(attrs.blocks, Math.ceil(attrs.size / 512));
  assert.equal(baseAttrs.size, 0);
  const ops = mounted.node.stream_ops;
  assert.equal(ops.close, preserved);
  assert.equal(ops.llseek({ position: 5 }, 3, 1), 8);
  assert.equal(ops.llseek({ position: 0 }, -1, 2), attrs.size - 1);
  assert.equal(ops.llseek({}, 7, 0), 7);
  for (const [offset, whence] of [[-1, 0], [0, 3], [0.5, 0]]) assert.throws(() => ops.llseek({ position: 0 }, offset, whence), error => error.errno === 28);
  const out = new Uint8Array(2);
  assert.equal(ops.read({}, out, 0, 2, 1), 2);
  assert.deepEqual([...out], [1, 2]);
});
