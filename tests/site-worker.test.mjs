import test from 'node:test';
import assert from 'node:assert/strict';

globalThis.caches ??= {default: {store: new Map(), async match(r) { return this.store.get(r.url)?.clone(); }, async put(r, res) { this.store.set(r.url, res); }}};
const {default: site} = await import('../workers/site/src/index.js');
const objects = new Map([['index.html', 'root page'], ['v/0123456789ab/app.js', 'plain js'], ['v/0123456789ab/app.js.gz', 'gz js']]);
const env = {SITE: {async get(key) { if (!objects.has(key)) return null; const text = objects.get(key); return {body: new Response(text).body, size: text.length, httpEtag: `"${key}"`}; }}};
const ctx = {waitUntil() {}};
const get = (path, headers = {}) => site.fetch(new Request(`https://themovies.romanf.cc${path}`, {headers}), env, ctx);

test('root is revalidated, releases are immutable and gzip-aware, everything is isolated', async () => {
  const root = await get('/');
  assert.equal(await root.text(), 'root page'); assert.equal(root.headers.get('Cache-Control'), 'no-cache');
  assert.equal(root.headers.get('Cross-Origin-Embedder-Policy'), 'require-corp'); assert.equal(root.headers.get('Content-Type'), 'text/html; charset=utf-8');
  const plain = await get('/v/0123456789ab/app.js');
  assert.equal(await plain.text(), 'plain js'); assert.match(plain.headers.get('Cache-Control'), /immutable/); assert.equal(plain.headers.get('Content-Encoding'), null);
  const packed = await get('/v/0123456789ab/app.js', {'Accept-Encoding': 'gzip, br'});
  assert.equal(packed.headers.get('Content-Encoding'), 'gzip'); assert.equal(await packed.text(), 'gz js');
  assert.equal((await get('/game/movies.zip')).status, 404);
  assert.equal((await get('/v/0123456789ab/missing.js')).status, 404);
  assert.equal((await get('/', {'If-None-Match': '"index.html"'})).status, 304);
});
