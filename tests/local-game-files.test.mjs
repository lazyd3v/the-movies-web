import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/local-game-files.js';

const {createIndex, createRequest} = globalThis.MoviesLocalGameFiles;
class FakeReader { readAsArrayBuffer(blob) { return blob.buffer; } }
const file = bytes => ({size: bytes.length, slice: (a, b) => ({buffer: Uint8Array.from(bytes.slice(a, b)).buffer})});

test('files are laid end to end in path order and ranges cross file boundaries', () => {
  const blobs = [{start: 0, end: 3, file: file([1, 2, 3])}, {start: 3, end: 3, file: file([])}, {start: 3, end: 6, file: file([4, 5, 6])}];
  const request = createRequest(blobs, 6, FakeReader);
  assert.deepEqual([...request('local', 1, 4, 6)], [2, 3, 4, 5]);
  assert.deepEqual([...request('local', 3, 5, 6)], [4, 5, 6]);
  assert.throws(() => request('local', 0, 6, 6), /Invalid/);
  assert.throws(() => request('local', 0, 1, 7), /Invalid/);
});
test('index lists every chosen file with offsets and the expected executable hash', () => {
  globalThis.FileReaderSync = FakeReader;
  const {index} = createIndex([{path: 'MoviesSE.exe', file: file([9, 9])}, {path: 'Data/a.pak', file: file([1])}], 'ab'.repeat(32));
  assert.deepEqual(index.files, [{path: 'Data/a.pak', size: 1, offset: 0}, {path: 'MoviesSE.exe', size: 2, offset: 1}]);
  assert.equal(index.totalBytes, 3); assert.equal(index.archiveBytes, 3); assert.equal(index.executable.sha256, 'ab'.repeat(32));
  assert.throws(() => createIndex([], 'ab'.repeat(32)), /No game files/);
  delete globalThis.FileReaderSync;
});
