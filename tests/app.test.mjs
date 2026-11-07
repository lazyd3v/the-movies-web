import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const source = await readFile(new URL('../public/app.js', import.meta.url), 'utf8');
async function app({ missing = [], failed = [] } = {}) {
  const elements = new Map();
  const listeners = new Map();
  const requests = [];
  const element = id => {
    if (!elements.has(id)) elements.set(id, { contentWindow: {}, value: id === 'engine' ? 'read-ahead' : '', options: id === 'engine' ? [{value: 'read-ahead'}, {value: 'mt-jit'}, {value: 'single-jit'}] : [] });
    return elements.get(id);
  };
  const context = {
    URL, URLSearchParams, SharedArrayBuffer, WebAssembly, crossOriginIsolated: true,
    location: { origin: 'http://localhost', search: '' },
    window: { addEventListener: (event, callback) => listeners.set(event, callback) },
    document: { getElementById: element, createElement: () => ({ getContext: () => ({}) }) },
    fetch: async (url, options) => {
      requests.push({url, method: options?.method || 'GET'});
      if (failed.includes(url)) throw new Error('Network failed');
      return { ok: !missing.includes(url), json: async () => ({ fileCount: 719, totalBytes: 100, components: ['qasf'] }) };
    }
  };
  await vm.runInNewContext(`(async () => {${source}\n})()`, context);
  return {
    element, requests,
    message(text, type = 'movies-log') {
      listeners.get('message')({ origin: context.location.origin, source: element('player').contentWindow, data: { type, text } });
    }
  };
}
test('batched milestones update status in line order without leaking trace text', async () => {
  const ui = await app();
  ui.message('MEDIA_SETUP_BEGIN\nMEDIA_REGISTER qasf.dll\ntrace:quartz unrelated');
  assert.equal(ui.element('session-status').textContent, 'Registering Windows Media: qasf.dll');
  ui.message('MEDIA_REGISTER wmvcore.dll\nThe Movies: starting\ntrace:quartz still unrelated');
  assert.equal(ui.element('session-status').textContent, 'Original game process started…');
  ui.message('The Movies: exited with code 1\nMEDIA_SETUP_FAILED');
  assert.equal(ui.element('session-status').textContent, 'Windows Media setup failed — see boot log');
});
test('immediate runtime status and errors supersede prior batch milestones', async () => {
  const ui = await app();
  ui.message('MEDIA_REGISTER qasf.dll');
  ui.message('Emulator running', 'movies-status');
  assert.equal(ui.element('session-status').textContent, 'Emulator running');
  ui.message('Launch failed: sample', 'movies-error');
  assert.equal(ui.element('session-status').textContent, 'Launch failed: sample');
});
test('launcher forwards strict ordering only when checked and preserves engine selection', async () => {
  const ui = await app();
  ui.element('launch').onclick();
  assert.equal(new URLSearchParams(ui.element('player').src.split('?')[1]).has('safe'), false);
  ui.element('strict-memory').checked = true;
  ui.element('engine').value = 'single-jit';
  ui.element('launch').onclick();
  const params = new URLSearchParams(ui.element('player').src.split('?')[1]);
  assert.equal(params.get('safe'), '1');
  assert.equal(params.get('engine'), 'single-jit');
});
test('optional read-ahead runtime requires both JS and WASM and falls back without disabling launch', async () => {
  for (const file of ['runtime/read-ahead/boxedwine.js', 'runtime/read-ahead/boxedwine.wasm']) {
    const ui = await app({missing: [file]});
    assert.equal(ui.element('engine').options[0].disabled, true);
    assert.equal(ui.element('engine').value, 'mt-jit');
    assert.equal(ui.element('launch').disabled, false);
    assert.ok(ui.requests.some(request => request.url === file && request.method === 'HEAD'));
    ui.element('launch').onclick();
    assert.equal(new URLSearchParams(ui.element('player').src.split('?')[1]).get('engine'), 'mt-jit');
  }
  const unavailable = await app({failed: ['runtime/read-ahead/boxedwine.wasm']});
  assert.equal(unavailable.element('engine').value, 'mt-jit');
  assert.equal(unavailable.element('launch').disabled, false);
});
test('complete read-ahead runtime retains the default experimental selection', async () => {
  const ui = await app();
  assert.equal(ui.element('engine').options[0].disabled, false);
  assert.equal(ui.element('engine').value, 'read-ahead');
  ui.element('launch').onclick();
  assert.equal(new URLSearchParams(ui.element('player').src.split('?')[1]).get('engine'), 'read-ahead');
});
