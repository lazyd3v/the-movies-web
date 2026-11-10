/* End-to-end check of the public play page as built in dist/.
 *
 * Serves --root (default dist) on a private port, opens it in a separate headless
 * Chrome, picks the game folder through the directory <input> fallback, presses
 * Play and waits for the main menu (frames presented). Fails on any HTTP error
 * response, and on any request for game files or MoviesSE.exe from the server.
 *
 * node scripts/bench/play-page-check.mjs [--root dist] [--game "<Game Files>"] [--seconds 120]
 */
import {spawn} from 'node:child_process';
import {rmSync, writeFileSync, mkdirSync} from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {createServer} from '../serve.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, i, all) => i % 2 ? pairs : [...pairs, [value.replace(/^--/, ''), all[i + 1]]], []));
const root = path.resolve(args.root ?? 'dist');
const game = args.game ?? path.resolve('.cache/game-files');
const seconds = Number(args.seconds ?? 120), port = 4199, devtools = 9399;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const began = performance.now();
const log = (...parts) => console.log(`[${((performance.now() - began) / 1000).toFixed(1)}s]`, ...parts);

const server = createServer({root});
await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
const profile = `/tmp/movies-play-check-${devtools}`;
rmSync(profile, {recursive: true, force: true});
const chrome = spawn(args.chrome ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', ['--headless=new', `--remote-debugging-port=${devtools}`,
  `--user-data-dir=${profile}`, '--no-first-run', '--enable-unsafe-webgpu', '--window-size=1280,1000', 'about:blank'], {stdio: 'ignore'});
const cleanup = () => { try { chrome.kill('SIGKILL'); } catch {} server.close(); };
process.on('exit', cleanup);

let version;
for (let i = 0; i < 100 && !version; i++) { try { version = await (await fetch(`http://127.0.0.1:${devtools}/json/version`)).json(); } catch { await sleep(100); } }
const socket = new WebSocket(version.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 1; const pending = new Map(), handlers = new Map();
socket.onmessage = ({data}) => {
  const message = JSON.parse(data);
  if (message.id && pending.has(message.id)) { const {resolve, reject} = pending.get(message.id); pending.delete(message.id); message.error ? reject(new Error(message.error.message)) : resolve(message.result); }
  else for (const fn of handlers.get(message.method) ?? []) fn(message.params, message.sessionId);
};
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => { const id = nextId++; pending.set(id, {resolve, reject}); socket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); });
const on = (method, fn) => handlers.set(method, [...(handlers.get(method) ?? []), fn]);

const failures = [], requested = new Set();
on('Network.responseReceived', ({response}) => {
  requested.add(new URL(response.url).pathname);
  if (response.status >= 400) failures.push(`HTTP ${response.status} ${response.url}`);
});
on('Target.attachedToTarget', async ({sessionId, waitingForDebugger}) => {
  try { await send('Network.enable', {}, sessionId); await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true}, sessionId); } catch {}
  if (waitingForDebugger) await send('Runtime.runIfWaitingForDebugger', {}, sessionId).catch(() => {});
});
const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
const {sessionId: page} = await send('Target.attachToTarget', {targetId, flatten: true});
for (const domain of ['Page', 'Runtime', 'Network', 'DOM']) await send(`${domain}.enable`, {}, page);
await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true}, page);
await send('Emulation.setDeviceMetricsOverride', {width: 1280, height: 1000, deviceScaleFactor: 1, mobile: false}, page);
// Headless Chrome cannot answer a native folder picker: use the <input> fallback.
await send('Page.addScriptToEvaluateOnNewDocument', {source: 'delete window.showDirectoryPicker;'}, page);
// --url checks a deployed site instead of serving --root locally.
await send('Page.navigate', {url: args.url ?? `http://127.0.0.1:${port}/`}, page);
const evaluate = async expression => { const r = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, page); if (r.exceptionDetails) throw new Error(r.exceptionDetails.text); return r.result.value; };
const text = id => evaluate(`document.getElementById(${JSON.stringify(id)})?.textContent ?? ''`);
// The root page loads the current release under /v/<hash>/ through <base>.
for (let i = 0; i < 100 && await evaluate("document.readyState === 'complete' && !!document.getElementById('folder-input')").catch(() => false) !== true; i++) await sleep(100);
await sleep(500);

const {root: documentNode} = await send('DOM.getDocument', {}, page);
const {nodeId} = await send('DOM.querySelector', {nodeId: documentNode.nodeId, selector: '#folder-input'}, page);
await send('DOM.setFileInputFiles', {nodeId, files: [game]}, page);
let folder = '';
for (let i = 0; i < 300 && !/^Ready|different|no MoviesSE|no Data|Could not/.test(folder); i++) { await sleep(200); folder = await text('folder-status'); }
log('folder:', folder);
if (!folder.startsWith('Ready')) { console.error('Folder was not accepted'); process.exit(1); }

const box = await evaluate(`(()=>{const b=document.getElementById('start');b.scrollIntoView({block:'center',behavior:'instant'});const r=b.getBoundingClientRect();return {x:r.left+r.width/2,y:r.top+r.height/2}})()`);
for (const type of ['mouseMoved', 'mousePressed', 'mouseReleased'])
  await send('Input.dispatchMouseEvent', {type, x: box.x, y: box.y, button: type === 'mouseMoved' ? 'none' : 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1}, page);
log('pressed Play');

let frames = 0, status = '';
const deadline = performance.now() + seconds * 1000;
while (performance.now() < deadline) {
  await sleep(2000);
  // Answer the game's first-run message box if it appears.
  await evaluate(`document.querySelector('#game-dialog[open] #dialog-buttons button')?.click()`);
  status = await text('status');
  frames = await evaluate(`(()=>{const c=document.querySelector('#game-surface canvas');return c&&!c.hidden?1:0})()`);
  if (/stopped|error/i.test(status)) break;
  if (frames && performance.now() - began > 60000) break;
}
// --esc-test 1: two Escape presses in fullscreen. The first must reach the game
// canvas and keep fullscreen; the second must not reach it and must leave fullscreen.
if (args['esc-test'] && frames) {
  await evaluate(`(()=>{const c=document.querySelector('#game-surface canvas');window.__esc=0;c.addEventListener('keydown',e=>{if(e.code==='Escape')window.__esc++;});c.focus();return true})()`);
  const esc = async () => { for (const type of ['keyDown', 'keyUp']) await send('Input.dispatchKeyEvent', {type, code: 'Escape', key: 'Escape', windowsVirtualKeyCode: 27}, page); await sleep(700); };
  const look = () => evaluate(`JSON.stringify({fullscreen: document.fullscreenElement?.id ?? null, delivered: window.__esc, keyboardLock: !!navigator.keyboard?.lock, hint: document.getElementById('escape-hint')?.hidden === false ? document.getElementById('escape-hint').textContent : null})`);
  log('esc before:', await look()); await esc(); log('esc after 1st:', await look()); await esc(); log('esc after 2nd:', await look());
}
const shot = await send('Page.captureScreenshot', {format: 'png'}, page);
mkdirSync('.cache/bench', {recursive: true});
writeFileSync('.cache/bench/play-page.png', Buffer.from(shot.data, 'base64'));
const leaked = [...requested].filter(p => /\/game\/|MoviesSE\.exe$/i.test(p));
log('status:', status);
log('game window shown:', !!frames, '| requests:', requested.size, '| HTTP errors:', failures.length, '| game-file requests:', leaked.length);
for (const failure of failures) console.log('  ', failure);
for (const leak of leaked) console.log('   leaked', leak);
const ok = frames && !failures.length && !leaked.length && !/stopped|error/i.test(status);
console.log(ok ? 'PASS' : 'FAIL'); cleanup(); process.exit(ok ? 0 : 1);
