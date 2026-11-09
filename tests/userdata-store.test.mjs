import test from 'node:test';
import assert from 'node:assert/strict';
import {MoviesUserdataStore, mirrorUserdata, restoreUserdata} from '../public/userdata-store.js';

/* Minimal in-memory OPFS: directory/file handles with the methods the store uses. */
function fakeStorage() {
  const error = name => Object.assign(new Error(name), {name});
  class File { constructor() { this.kind = 'file'; this.data = new Uint8Array(); }
    async getFile() { const data = this.data; return {arrayBuffer: async () => data.slice().buffer}; }
    async createWritable() { const file = this; let chunks = []; return {write: async b => { chunks.push(new Uint8Array(b instanceof ArrayBuffer ? b : b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength))); }, close: async () => { const n = chunks.reduce((s, c) => s + c.length, 0), out = new Uint8Array(n); let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; } file.data = out; }}; } }
  class Dir { constructor() { this.kind = 'directory'; this.children = new Map(); }
    async getDirectoryHandle(name, {create = false} = {}) { let h = this.children.get(name); if (!h) { if (!create) throw error('NotFoundError'); h = new Dir(); this.children.set(name, h); } if (h.kind !== 'directory') throw error('TypeMismatchError'); return h; }
    async getFileHandle(name, {create = false} = {}) { let h = this.children.get(name); if (!h) { if (!create) throw error('NotFoundError'); h = new File(); this.children.set(name, h); } if (h.kind !== 'file') throw error('TypeMismatchError'); return h; }
    async removeEntry(name, {recursive = false} = {}) { const h = this.children.get(name); if (!h) throw error('NotFoundError'); if (h.kind === 'directory' && h.children.size && !recursive) throw error('InvalidModificationError'); this.children.delete(name); }
    async *entries() { yield* this.children.entries(); } }
  const root = new Dir();
  return {root, storage: {getDirectory: async () => root, persist: async () => true}};
}
const text = s => new TextEncoder().encode(s);
const decode = b => new TextDecoder().decode(b);

test('user-data store applies ordered changes and reloads the tree', async () => {
  const {storage} = fakeStorage(), store = new MoviesUserdataStore({storage, onError: e => { throw e; }});
  store.apply({op: 'write', path: 'registry.v1', bytes: text('reg')});
  store.apply({op: 'mkdir', path: 'My Documents/The Movies/Saves'});
  store.apply({op: 'write', path: 'My Documents/The Movies/Saves/a.sav', bytes: text('one')});
  store.apply({op: 'write', path: 'game-overlay/Data/options.cfg', bytes: text('quality=1')});
  store.apply({op: 'rename', from: 'My Documents/The Movies/Saves/a.sav', to: 'My Documents/The Movies/Saves/b.sav'});
  store.apply({op: 'mkdir', path: 'empty'});
  store.apply({op: 'write', path: 'tmp.bin', bytes: text('x')}); store.apply({op: 'delete', path: 'tmp.bin'});
  await store.flush();
  const {files, directories} = await new MoviesUserdataStore({storage}).load();
  const byPath = Object.fromEntries(files.map(f => [f.path, decode(f.bytes)]));
  assert.deepEqual(byPath, {'registry.v1': 'reg', 'My Documents/The Movies/Saves/b.sav': 'one', 'game-overlay/Data/options.cfg': 'quality=1'});
  assert.deepEqual(directories, ['empty']);
});
test('user-data store renames directories recursively and rejects escaping paths', async () => {
  const {storage} = fakeStorage(), errors = [], store = new MoviesUserdataStore({storage, onError: e => errors.push(e.message)});
  store.apply({op: 'write', path: 'Saves/slot1/data', bytes: text('d')});
  store.apply({op: 'rename', from: 'Saves', to: 'Backup'});
  store.apply({op: 'write', path: '../outside', bytes: text('no')});
  await store.flush();
  const {files} = await store.load();
  assert.deepEqual(files.map(f => f.path), ['Backup/slot1/data']);
  assert.match(errors[0], /Invalid user-data path/);
});
test('worker mirror reports MEMFS changes under /userdata only, and restore rebuilds the tree', () => {
  const nodes = new Map([['/userdata/s.cfg', text('abc')]]), sent = [];
  const FS = {write(stream) { return 1; }, close() {}, rename() {}, unlink() {}, rmdir() {}, mkdir() {}, truncate() {}, ftruncate() {}, getStream: () => null, readFile: path => nodes.get(path)};
  const undo = mirrorUserdata(FS, change => sent.push(change));
  const stream = {path: '/userdata/s.cfg'}; FS.write(stream); FS.close(stream);
  const other = {path: '/game/x'}; FS.write(other); FS.close(other);
  FS.close({path: '/userdata/readonly'});
  FS.rename('/userdata/a', '/userdata/b'); FS.unlink('/userdata/b'); FS.mkdir('/userdata/d'); FS.rmdir('/userdata/d'); FS.mkdir('/tmp/x');
  assert.deepEqual(sent.map(c => c.op + ':' + (c.path ?? c.from + '>' + c.to)), ['write:s.cfg', 'rename:a>b', 'delete:b', 'mkdir:d', 'rmdir:d']);
  assert.equal(decode(sent[0].bytes), 'abc');
  undo();
  const made = [], written = new Map();
  const target = {mkdir(path) { if (made.includes(path)) throw Object.assign(new Error('exists'), {errno: 20}); made.push(path); }, analyzePath: path => ({exists: made.includes(path)}), writeFile: (path, bytes) => written.set(path, decode(bytes))};
  assert.equal(restoreUserdata(target, {files: [{path: 'a/b.txt', bytes: text('B').buffer}], directories: ['empty/dir']}), 1);
  assert.deepEqual(made, ['/userdata', '/userdata/empty', '/userdata/empty/dir', '/userdata/a']);
  assert.equal(written.get('/userdata/a/b.txt'), 'B');
});
