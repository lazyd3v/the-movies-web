import test from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto, createHash } from 'node:crypto';
import '../public/range-file.js';
import '../public/stored-archive-fs.js';
if (!globalThis.crypto) globalThis.crypto = webcrypto;
const CHUNK = 256*1024;
function fsMock() {
  const entries = new Map();
  class ErrnoError extends Error { constructor(errno) { super(`errno ${errno}`); this.errno=errno; } }
  return { entries, ErrnoError, analyzePath(path) { return {exists:entries.has(path)}; },
    mkdirTree(path) { if(!entries.has(path))entries.set(path,{mode:0o40777,node_ops:{getattr:()=>({})}}); },
    lookupPath(path) { return {node:entries.get(path)}; },
    createFile(dir,name) { const node={mode:0o100444,node_ops:{getattr:()=>({size:0,mode:0o100444})},stream_ops:{}};entries.set(`${dir}/${name}`,node);return node; }
  };
}
function makeIndex() {
  return {schemaVersion:1,archive:'movies.zip',archiveBytes:3*1024**3,archiveSha256:'a'.repeat(64),
    executable:{path:'MoviesSE.exe',sha256:'b'.repeat(64)},fileCount:3,totalBytes:32,
    files:[{path:'MoviesSE.exe',offset:CHUNK-4,size:12},{path:'Data/a',offset:CHUNK+8,size:8},
      {path:'Data/b',offset:2**31+100,size:12}]};
}
const request = (url,start,end) => Uint8Array.from({length:end-start+1},(_,i)=>(start+i)%251);
test('one cache shared across files, chunk edges, EOF and 64-bit archive offsets',async()=>{
  const FS=fsMock(), index=makeIndex();
  const mount=await MoviesStoredArchiveFS.mount(FS,{index,url:'/game/movies.zip',request});
  const a=mount.nodes.get('MoviesSE.exe'),b=mount.nodes.get('Data/a'),high=mount.nodes.get('Data/b');
  const buffer=new Uint8Array(30).fill(255);
  assert.equal(a.stream_ops.read({},buffer,3,20,0),12);
  assert.deepEqual([...buffer.slice(3,15)],Array.from({length:12},(_,i)=>(CHUNK-4+i)%251));
  assert.equal(mount.stats.requests,2);
  assert.equal(b.stream_ops.read({},buffer,0,8,0),8);assert.equal(mount.stats.requests,2);
  assert.equal(high.stream_ops.read({},buffer,0,30,9),3);
  assert.equal(buffer[0],(2**31+109)%251);
  assert.equal(high.stream_ops.read({},buffer,0,10,12),0);
  assert.equal(high.node_ops.getattr(high).size,12);
  assert.equal(high.stream_ops.llseek({position:3},-2,2),10);
  assert.equal(high.stream_ops.llseek({position:3},2**32,0),2**32);
  assert.throws(()=>high.stream_ops.llseek({},-1,0));
  assert.throws(()=>high.stream_ops.read({},buffer,0,31,0));
  for(const operation of ['write','allocate','msync','mmap'])assert.throws(()=>high.stream_ops[operation]());
  assert.throws(()=>high.node_ops.setattr(high,{size:0}));
  assert.throws(()=>FS.lookupPath('/game/Data').node.node_ops.unlink());
  assert.throws(()=>high.stream_ops.open({flags:2}));
  assert.ok(mount.stats.cacheBytes<=32*1024**2);
});
test('reject whole invalid index before mutation',async()=>{
  for(const mutate of [i=>i.files[1].path='../escape',i=>i.files[1].path='moviesse.EXE',
    i=>i.files[1].offset=i.archiveBytes,i=>i.files[1].offset=i.files[0].offset,
    i=>i.files[1].path='MoviesSE.exe/child']) {
    const FS=fsMock(),index=makeIndex();mutate(index);
    await assert.rejects(MoviesStoredArchiveFS.mount(FS,{index,url:'/x',request}));assert.equal(FS.entries.size,0);
  }
});
test('preload checked before mount, owned bytes avoid range request',async()=>{
  const FS=fsMock(),index=makeIndex(),bytes=new Uint8Array(12).fill(7);
  index.executable.sha256=createHash('sha256').update(bytes).digest('hex');
  const mounted=await MoviesStoredArchiveFS.mount(FS,{index,url:'/x',request,preloaded:new Map([['MoviesSE.exe',bytes]])});
  bytes.fill(99);const buffer=new Uint8Array(12);
  mounted.nodes.get('MoviesSE.exe').stream_ops.read({},buffer,0,12,0);
  assert.equal(buffer[0],7);assert.equal(mounted.stats.requests,0);
  const invalid=fsMock();
  await assert.rejects(MoviesStoredArchiveFS.mount(invalid,{index,url:'/x',request,preloaded:new Map([['MoviesSE.exe',bytes]])}));
  assert.equal(invalid.entries.size,0);
});
