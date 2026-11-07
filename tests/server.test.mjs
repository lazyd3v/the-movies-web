import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { mkdtemp, writeFile, mkdir, symlink, open, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createServer } from '../scripts/serve.mjs';

let temp, root, server, port;
const offset = 2 ** 31 + 123;
before(async () => {
  temp = await mkdtemp(path.join(os.tmpdir(), 'movies-server-'));
  root = path.join(temp, 'public');
  await mkdir(root);
  await writeFile(path.join(root, 'index.html'), 'hello');
  await writeFile(path.join(root, 'module.wasm'), Buffer.from([0, 97, 115, 109]));
  await writeFile(path.join(root, 'empty'), '');
  await writeFile(path.join(temp, 'private.txt'), 'secret');
  await symlink(path.join(temp, 'private.txt'), path.join(root, 'escape'));
  const big = await open(path.join(root, 'large.zip'), 'w');
  await big.write(Buffer.from('LARGE'), 0, 5, offset);
  await big.close();
  server = createServer({ root });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => { server.off('error', reject); resolve(); });
  });
  port = server.address().port;
});
after(async () => {
  if (server?.listening) await new Promise(resolve => server.close(resolve));
  if (temp) await rm(temp, { recursive: true, force: true });
});
function request(target, { method = 'GET', headers = {} } = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({ hostname: '127.0.0.1', port, path: target, method, headers }, res => {
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
    });
    req.on('error', reject);
    req.end();
  });
}
test('serves index, WASM MIME, HEAD and browser isolation headers', async () => {
  const page = await request('/');
  assert.equal(page.body.toString(), 'hello');
  assert.equal(page.headers['cross-origin-opener-policy'], 'same-origin');
  assert.equal(page.headers['cross-origin-embedder-policy'], 'require-corp');
  assert.equal(page.headers['cross-origin-resource-policy'], 'same-origin');
  const wasm = await request('/module.wasm', { method: 'HEAD' });
  assert.equal(wasm.headers['content-type'], 'application/wasm');
  assert.equal(wasm.headers['content-length'], '4');
  assert.equal(wasm.body.length, 0);
});
test('streams ranges including offsets above signed 32-bit boundary', async () => {
  const large = await request('/large.zip', { headers: { Range: `bytes=${offset}-${offset + 4}` } });
  assert.equal(large.status, 206);
  assert.equal(large.body.toString(), 'LARGE');
  assert.equal(large.headers['content-range'], `bytes ${offset}-${offset + 4}/${offset + 5}`);
  for (const [range, expected] of [['bytes=-2', 'lo'], ['bytes=2-', 'llo'], ['bytes=1-99', 'ello']]) {
    const response = await request('/', { headers: { Range: range } });
    assert.equal(response.status, 206);
    assert.equal(response.body.toString(), expected);
  }
  const head = await request('/large.zip', { method: 'HEAD', headers: { Range: `bytes=${offset}-` } });
  assert.equal(head.status, 206);
  assert.equal(head.headers['content-length'], '5');
  assert.equal(head.body.length, 0);
});
test('rejects invalid, multiple and unsatisfiable ranges', async () => {
  for (const range of ['bytes=5-', 'bytes=3-1', 'bytes=-0', 'bytes=0-1,3-4', 'bytes=-', 'bytes=9007199254740992-']) {
    const res = await request('/', { headers: { Range: range } });
    assert.equal(res.status, 416, range);
    assert.equal(res.headers['content-range'], 'bytes */5');
  }
  assert.equal((await request('/empty', { headers: { Range: 'bytes=0-' } })).status, 416);
});
test('rejects traversal, malformed URL, escaped symlinks and writes', async () => {
  for (const target of ['/../private.txt', '/%2e%2e/private.txt', '/%2e%2e%2fprivate.txt', '/escape', '/foo%5c..%5cprivate.txt', '/%00']) {
    assert.equal((await request(target)).status, 403, target);
  }
  assert.equal((await request('/%ZZ')).status, 400);
  const missing = await request('/missing');
  assert.equal(missing.status, 404);
  assert.equal(missing.headers['cross-origin-embedder-policy'], 'require-corp');
  const post = await request('/', { method: 'POST' });
  assert.equal(post.status, 405);
  assert.equal(post.headers.allow, 'GET, HEAD');
});
