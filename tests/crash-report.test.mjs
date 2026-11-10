import test from 'node:test';
import assert from 'node:assert/strict';
import {isLocalHost, reportingAvailable, buildReport, sendReport} from '../public/crash-report.js';
import worker, {normalize, signature, LIMITS} from '../workers/crash-reports/src/index.js';

test('reports are only possible on a configured production host', () => {
  for (const host of ['localhost', '127.0.0.1', '[::1]', 'game.localhost', 'movies.test', '']) assert.equal(isLocalHost(host), true, host);
  assert.equal(isLocalHost('movies.example.com'), false);
  assert.equal(reportingAvailable('https://reports.example.workers.dev/report', 'movies.example.com'), true);
  assert.equal(reportingAvailable('https://reports.example.workers.dev/report', '127.0.0.1'), false);
  assert.equal(reportingAvailable('', 'movies.example.com'), false);
  assert.equal(reportingAvailable('http://insecure.example/report', 'movies.example.com'), false);
});
test('a report carries the log tail, bounded, and nothing else from the page', async () => {
  const log = Array.from({length: 1000}, (_, i) => `line ${i}`).join('\n') + '\nSTOP: 00afe24c: D3D9 StretchRect unsupported';
  const report = buildReport({reason: 'The game stopped (4).', logText: log, siteVersion: 'abc', build: 'f'.repeat(64), sessionSeconds: 12.7, frames: 900});
  assert.equal(report.schema, 1); assert.equal(report.sessionSeconds, 13);
  assert.equal(report.logTail.split('\n').length, 400); assert.match(report.logTail, /STOP: 00afe24c/);
  let sent;
  const id = await sendReport('https://r.example/report', report, async (url, init) => { sent = {url, init}; return new Response(JSON.stringify({id: 'x'.repeat(36)}), {status: 201}); });
  assert.equal(id, 'x'.repeat(36)); assert.equal(sent.init.credentials, 'omit'); assert.deepEqual(JSON.parse(sent.init.body), report);
  assert.equal(await sendReport('https://r.example/report', report, async () => { throw new Error('offline'); }), null);
});

function database() {
  const rows = [];
  return {rows, prepare(sql) {
    return {bind(...args) {
      return {
        async run() { assert.match(sql, /^INSERT INTO reports/); const [id, created, client, kind, signature, , description, contact] = args; rows.push({id, created, client, kind, signature, description, contact, log_tail: args[15]}); return {}; },
        async first() { if (/COUNT/.test(sql)) return {n: rows.filter(r => r.client === args[0] && r.created > args[1]).length}; return rows.find(r => r.id === args[0]) ?? null; },
        async all() { return {results: rows.map(({log_tail, ...rest}) => rest)}; },
      };
    }, async all() { return {results: [{signature: 'x', count: rows.length}]}; }};
  }};
}
const env = () => ({DB: database(), ALLOWED_ORIGINS: 'https://movies.example.com', REPORTS_TOKEN: 'secret-token', CLIENT_SALT: 'salt'});
const post = (body, origin = 'https://movies.example.com', ip = '1.2.3.4') => new Request('https://r.example/report', {method: 'POST', body: typeof body === 'string' ? body : JSON.stringify(body), headers: {Origin: origin, 'CF-Connecting-IP': ip, 'Content-Type': 'application/json'}});
const valid = {schema: 1, reason: 'The game stopped (4).', logTail: '[dinput] calls=1200\nSTOP: 00b0e839: D3D9 back/depth surface LockRect unsupported\nregs', userAgent: 'UA'};

test('collector stores valid reports from the site origin only', async () => {
  const e = env();
  const preflight = await worker.fetch(new Request('https://r.example/report', {method: 'OPTIONS', headers: {Origin: 'https://movies.example.com'}}), e);
  assert.equal(preflight.status, 204); assert.equal(preflight.headers.get('Access-Control-Allow-Origin'), 'https://movies.example.com');
  assert.equal((await worker.fetch(post(valid, 'https://evil.example'), e)).status, 403);
  assert.equal((await worker.fetch(post('{not json'), e)).status, 400);
  assert.equal((await worker.fetch(post({...valid, schema: 2}), e)).status, 400);
  assert.equal((await worker.fetch(post('x'.repeat(LIMITS.body + 1)), e)).status, 413);
  const created = await worker.fetch(post(valid), e);
  assert.equal(created.status, 201); assert.equal(created.headers.get('Access-Control-Allow-Origin'), 'https://movies.example.com');
  const {id} = await created.json(); assert.match(id, /^[0-9a-f-]{36}$/);
  assert.equal(e.DB.rows[0].signature, 'STOP: 00b0e839: D3D9 back/depth surface LockRect unsupported');
  assert.doesNotMatch(e.DB.rows[0].client, /1\.2\.3\.4/);
});
test('collector throttles one client and protects the read endpoints', async () => {
  const e = env();
  for (let i = 0; i < LIMITS.perHour; i++) assert.equal((await worker.fetch(post(valid), e)).status, 201);
  assert.equal((await worker.fetch(post(valid), e)).status, 429);
  assert.equal((await worker.fetch(post(valid, undefined, '5.6.7.8'), e)).status, 201, 'other clients are unaffected');
  assert.equal((await worker.fetch(new Request('https://r.example/reports'), e)).status, 401);
  assert.equal((await worker.fetch(new Request('https://r.example/reports', {headers: {Authorization: 'Bearer wrong-token!'}}), e)).status, 401);
  const listing = await worker.fetch(new Request('https://r.example/reports', {headers: {Authorization: 'Bearer secret-token'}}), e);
  assert.equal(listing.status, 200); const rows = await listing.json(); assert.equal(rows.length, LIMITS.perHour + 1); assert.equal(rows[0].log_tail, undefined);
  const one = await worker.fetch(new Request(`https://r.example/reports/${e.DB.rows[0].id}`, {headers: {Authorization: 'Bearer secret-token'}}), e);
  assert.match((await one.json()).log_tail, /STOP: 00b0e839/);
});
test('signatures group the same bug across runs', () => {
  assert.equal(signature('r', 'x\n[D3D9] graphics bridge command 5 failed: RangeError calls=12 t=99.5 ms'), '[D3D9] graphics bridge command 5 failed: RangeError calls=# t=# ms');
  assert.equal(signature('Worker error: boom', 'nothing relevant'), 'Worker error: boom');
  assert.throws(() => normalize({schema: 1, reason: ''}), /reason/);
});
test('player-written reports need a description and keep the optional contact', async () => {
  const e = env();
  assert.equal((await worker.fetch(post({schema: 1, kind: 'user', description: '   '}), e)).status, 400);
  const report = buildReport({kind: 'user', description: 'Finances froze\nafter a click', contact: '@friend', siteVersion: 'v1'});
  assert.equal(report.logTail, '', 'no log unless the player includes it');
  assert.equal((await worker.fetch(post(report), e)).status, 201);
  assert.deepEqual([e.DB.rows[0].kind, e.DB.rows[0].signature, e.DB.rows[0].contact], ['user', 'user: Finances froze', '@friend']);
  assert.equal(normalize({schema: 1, reason: 'x', logTail: 'STOP: 1'}).kind, 'crash');
});
