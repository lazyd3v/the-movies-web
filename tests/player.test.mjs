import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const source = await readFile(new URL('../public/player.js', import.meta.url), 'utf8');
async function boot({search = '', media = null, missing = false, procFailure = false} = {}) {
  const elements = new Map();
  const scripts = [];
  const messages = [];
  const mounts = [];
  const timers = new Map();
  let nextTimer = 0, upstreamPrints = 0;
  const context = {
    URL, URLSearchParams, Uint8Array, Error, console,
    Config: {}, Module: {print() { upstreamPrints++; }, printErr() { upstreamPrints++; }},
    setTimeout(callback) { const id = ++nextTimer; timers.set(id, callback); return id; },
    clearTimeout(id) { timers.delete(id); },
    location: {search, href: `http://localhost/player.html${search}`, origin: 'http://localhost'},
    window: {addEventListener() {}}, parent: {postMessage: m => messages.push(m)},
    document: {getElementById(id) {if (!elements.has(id)) elements.set(id, {style: {}, value: ''}); return elements.get(id);}, createElement() {return {};}, body: {append: s => scripts.push(s)}},
    fetch: async url => ({ok: url.includes('media') ? !!media : !missing, json: async () => url.includes('media') ? media : {archiveBytes: 2628561057, fileCount: 719}}),
    buildAppFileSystem() {}, buildBrowserFileSystem() {}, loadFile() {}, getEnvProp() {}, getEmulatorParams() { return ['-root', '/root', '/bin/wine', 'launch.bat']; }, FS: {mkdirTree() {}, writeFile() {}},
    MoviesProcStat: {install() { mounts.push('proc-stat'); if (procFailure) throw new Error('Proc filesystem failed'); }},
    MoviesRangeFile: {mount: (_fs, options) => { mounts.push('archive'); return options; }}
  };
  await vm.runInNewContext(source, context);
  return {context, scripts, messages, mounts, elements, timers, upstreamPrints: () => upstreamPrints, flush() { for (const callback of [...timers.values()]) callback(); }};
}
test('upstream replacement parameters omit question mark and retain root filename', async () => {
  const {context, scripts} = await boot();
  assert.equal(context.Config.urlParams.startsWith('?'), false);
  const params = new URLSearchParams(context.Config.urlParams);
  assert.equal(params.get('root'), 'TinyCore15Wine11.0-web.zip');
  assert.equal(params.get('p'), 'launch.bat');
  assert.equal(scripts[0].src, 'runtime/boxedwine.js');
  context.buildAppFileSystem(() => {});
  assert.equal(context.moviesArchive.size, 2628561057);
});
test('media override bypasses upstream encoded-colon parser and sets overlay bootstrap', async () => {
  const {context} = await boot({media: {dllOverrides: 'qasf,wmvcore,wmasf=n,b'}});
  const params = new URLSearchParams(context.Config.urlParams);
  assert.equal(params.get('p'), 'c:/media-setup.bat');
  assert.equal(params.get('overlay'), 'media.zip');
  assert.equal(params.has('env'), false);
  assert.equal(context.getEnvProp(), 'WINEDLLOVERRIDES=qasf,wmvcore,wmasf=n,b');
});
test('fresh diagnostic uses memory storage and bypasses game media registration', async () => {
  const {context} = await boot({search: '?diagnostic=notepad&fresh=1', media: {dllOverrides: 'qasf=n,b'}});
  const params = new URLSearchParams(context.Config.urlParams);
  assert.equal(params.get('p'), 'notepad.exe');
  assert.deepEqual(params.getAll('storage'), ['memory']);
  assert.equal(params.get('rootfs'), 'memory');
  assert.equal(params.has('overlay'), false);
});
test('missing game manifest reports failure and never loads runtime', async () => {
  const {scripts, messages} = await boot({missing: true});
  assert.equal(scripts.length, 0);
  assert.match(messages.at(-1).text, /prepare:game/);
});
test('procfs is installed before archive mount and runtime startup callback', async () => {
  const {context, mounts} = await boot();
  context.Config.appZipFile = 'movies.zip'; // Set by the upstream configuration parser.
  context.buildAppFileSystem(() => mounts.push('start'));
  assert.deepEqual(mounts, ['proc-stat', 'archive', 'start']);
  assert.equal(context.moviesArchive.path, '/movies.zip');
  assert.equal(context.moviesArchive.url, 'http://localhost/game/movies.zip');
});
test('procfs installation failure reports an error and prevents emulator startup', async () => {
  const {context, mounts, messages} = await boot({procFailure: true});
  let started = false;
  context.buildAppFileSystem(() => { started = true; });
  assert.equal(started, false);
  assert.deepEqual(mounts, ['proc-stat']);
  assert.equal(messages.at(-1).type, 'movies-error');
  assert.match(messages.at(-1).text, /Proc filesystem failed/);
});
test('trace logs batch without upstream per-line DOM writes and flush on exit', async () => {
  const {context, messages, elements, timers, upstreamPrints, flush} = await boot();
  for (let i = 0; i < 2000; i++) context.Module.print('trace', i);
  context.Module.printErr('trace stderr');
  assert.equal(upstreamPrints(), 0);
  assert.equal(elements.get('output').value, '');
  assert.equal(messages.length, 0);
  assert.equal(timers.size, 1);
  flush();
  assert.equal(messages.length, 1);
  assert.equal(messages[0].text.split('\n').length, 2001);
  assert.match(elements.get('output').value, /trace 1999\ntrace stderr/);
  context.Module.print('last message');
  context.Module.onExit(0);
  assert.equal(timers.size, 0);
  assert.match(messages.at(-1).text, /last message/);
});
test('log buffers remain bounded and fatal errors are delivered immediately', async () => {
  const {context, messages, elements, flush} = await boot();
  const huge = 'x'.repeat(2 * 1024 * 1024);
  context.Module.print(huge);
  context.Module.print('recent tail');
  flush();
  assert.ok(elements.get('output').value.length <= 1024 * 1024);
  assert.match(elements.get('output').value, /recent tail/);
  context.Module.print('first JIT block executed');
  assert.equal(messages.at(-1).type, 'movies-status');
  context.Module.printErr('Unhandled promise rejection: stopped');
  assert.equal(messages.at(-1).type, 'movies-error');
  assert.match(elements.get('output').value, /Launch failed/);
});
test('media tracing stays quiet during bootstrap and enables deferred game tracing', async () => {
  const {context} = await boot({search: '?trace=media'});
  const args = context.getEmulatorParams();
  assert.deepEqual(args.slice(2, 6), ['-env', 'WINEDEBUG=-all', '-env', 'MOVIES_TRACE=1']);
  assert.equal(args[6], '/bin/wine');
});
test('strict memory ordering is off by default and enabled only with safe=1', async () => {
  for (const [search, expected] of [['', 'false'], ['?safe=0', 'false'], ['?safe=1&engine=single-jit', 'true']]) {
    const {context, scripts} = await boot({search});
    assert.equal(new URLSearchParams(context.Config.urlParams).get('safe-memory'), expected);
    if (search.includes('single-jit')) assert.equal(scripts[0].src, 'runtime/single-jit/boxedwine.js');
  }
});
test('read-ahead mode loads its own runtime while retaining the normal game boot configuration', async () => {
  const {context, scripts} = await boot({search: '?engine=read-ahead'});
  assert.equal(scripts[0].src, 'runtime/read-ahead/boxedwine.js');
  const params = new URLSearchParams(context.Config.urlParams);
  assert.equal(params.get('app'), 'movies.zip');
  assert.equal(params.get('safe-memory'), 'false');
});
