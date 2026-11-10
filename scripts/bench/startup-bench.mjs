#!/usr/bin/env node
/* Automated startup benchmark for public/static-probe.html.
 *
 * Launches a separate headless Chrome over the DevTools protocol (no user windows
 * or cursor involved), clicks "Run original startup" and the first-run dialog with
 * trusted Input events (these satisfy the page's user-activation audio check), and
 * records a timeline until the main menu is visible or a deadline/stop is reached.
 * Optional: CPU-profile every game worker for a window and print the hottest
 * functions by self time.
 *
 * Usage:
 *   node scripts/bench/startup-bench.mjs [--build 1-threads] [--until menu|lot|seconds:N]
 *        [--profile-from S --profile-seconds N] [--out .cache/bench/run.json] [--headful]
 * The dev server must be running (npm start).
 */
import {spawn} from 'node:child_process';
import {mkdirSync, writeFileSync, rmSync, existsSync, readFileSync, unlinkSync} from 'node:fs';
import {dirname} from 'node:path';

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, value, i, all) => {
  if (value.startsWith('--')) pairs.push([value.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
  return pairs;
}, []));
const build = args.build ?? '1-threads';
const until = args.until ?? 'menu';
const origin = args.origin ?? 'http://127.0.0.1:4173';
const port = Number(args.port ?? 9333);
const out = args.out ?? `.cache/bench/startup-${build}-${Date.now()}.json`;
const deadlineSeconds = Number(args.deadline ?? 1500);
const profileFrom = args['profile-from'] !== undefined ? Number(args['profile-from']) : null;
const profileSeconds = Number(args['profile-seconds'] ?? 60);
const chromePath = args.chrome ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const profileDir = `/tmp/movies-bench-chrome-${port}`;

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const log = (...parts) => console.log(`[${((performance.now() - began) / 1000).toFixed(1)}s]`, ...parts);
const began = performance.now();

if (!args['keep-profile']) rmSync(profileDir, {recursive: true, force: true});
const chrome = spawn(chromePath, [
  ...(args.headful ? [] : ['--headless=new']), `--remote-debugging-port=${port}`, `--user-data-dir=${profileDir}`,
  '--no-first-run', '--no-default-browser-check', '--enable-unsafe-webgpu', '--window-size=1280,1100',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
  'about:blank'], {stdio: ['ignore', 'ignore', 'pipe']});
let chromeErr = '';
chrome.stderr.on('data', chunk => { chromeErr = (chromeErr + chunk).slice(-4000); });
process.on('exit', () => { try { chrome.kill('SIGKILL'); } catch {} });

let version;
for (let i = 0; i < 100 && !version; i++) {
  try { version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(100); }
}
if (!version) throw new Error(`Chrome DevTools endpoint unavailable\n${chromeErr}`);

const socket = new WebSocket(version.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
let nextId = 1;
const pending = new Map(), handlers = new Map();
socket.onmessage = event => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    const {resolve, reject} = pending.get(message.id); pending.delete(message.id);
    message.error ? reject(new Error(`${message.error.message} ${message.error.data ?? ''}`)) : resolve(message.result);
  } else if (message.method) for (const fn of handlers.get(message.method) ?? []) fn(message.params, message.sessionId);
};
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = nextId++; pending.set(id, {resolve, reject});
  socket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
});
const on = (method, fn) => handlers.set(method, [...(handlers.get(method) ?? []), fn]);

// Track every worker (game worker, pthreads, graphics/media workers).
const workers = new Map();
on('Target.attachedToTarget', async ({sessionId, targetInfo, waitingForDebugger}) => {
  if (targetInfo.type.includes('worker')) workers.set(sessionId, {url: targetInfo.url, title: targetInfo.title, type: targetInfo.type});
  try {
    await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true}, sessionId);
    if (waitingForDebugger) await send('Runtime.runIfWaitingForDebugger', {}, sessionId);
  } catch {}
});
on('Target.detachedFromTarget', ({sessionId}) => workers.delete(sessionId));

const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
const {sessionId: page} = await send('Target.attachToTarget', {targetId, flatten: true});
await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true}, page);
await send('Page.enable', {}, page);
await send('Runtime.enable', {}, page);
await send('Emulation.setDeviceMetricsOverride', {width: 1280, height: 1100, deviceScaleFactor: 1, mobile: false}, page);
// --init-js <base64>: page script installed before any page code runs (diagnostic hooks).
if (args['init-js']) await send('Page.addScriptToEvaluateOnNewDocument', {source: Buffer.from(String(args['init-js']), 'base64').toString()}, page);
await send('Page.navigate', {url: `${origin}/static-probe.html${args['trace-at'] ? '?traceAt=' + Number(args['trace-at']) : ''}`}, page);

const evaluate = async expression => {
  const result = await send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, page);
  if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
  return result.result.value;
};
const click = async (x, y) => {
  for (const type of ['mouseMoved', 'mousePressed', 'mouseReleased'])
    await send('Input.dispatchMouseEvent', {type, x, y, button: type === 'mouseMoved' ? 'none' : 'left', buttons: type === 'mousePressed' ? 1 : 0, clickCount: 1}, page);
};
const moveTo = async (x, y) => send('Input.dispatchMouseEvent', {type: 'mouseMoved', x, y, button: 'none', buttons: 0}, page);
const centerOf = async selector => evaluate(`(()=>{const e=document.querySelector(${JSON.stringify(selector)});if(!e)return null;e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect();return {x:r.left+r.width/2,y:r.top+r.height/2}})()`);
// The game's menu cursor is driven by relative DirectInput motion. Steer it like a
// physical mouse: synthetic pointer events with explicit movementX/Y on the canvas
// (the DirectInput host does not require trusted events), clamped at the corner.
const rel = async (dx, dy) => evaluate(`(async()=>{const c=document.querySelector('#game-surface canvas');c.focus();
  let x=${dx},y=${dy};while(x||y){const sx=Math.max(-40,Math.min(40,x)),sy=Math.max(-40,Math.min(40,y));x-=sx;y-=sy;
  c.dispatchEvent(new PointerEvent('pointermove',{bubbles:true,pointerType:'mouse',pointerId:1,isPrimary:true,button:-1,buttons:0,clientX:0,clientY:0,movementX:sx,movementY:sy}));
  await new Promise(r=>setTimeout(r,16));}return true})()`);
const gameClick = async (button = 0) => evaluate(`(async()=>{const c=document.querySelector('#game-surface canvas');c.focus();const o={bubbles:true,pointerType:'mouse',pointerId:1,isPrimary:true,button:${button},clientX:0,clientY:0,movementX:0,movementY:0};
  c.dispatchEvent(new PointerEvent('pointerdown',{...o,buttons:${button === 2 ? 2 : 1}}));await new Promise(r=>setTimeout(r,150));c.dispatchEvent(new PointerEvent('pointerup',{...o,buttons:0}));return true})()`);
const mouseScale = Number(args['mouse-scale'] ?? 1.6); // measured: game multiplies DI deltas by ~1.6
let cursor = null; // last steered game-space point, tracked so in-lot moves avoid edge scrolling
const steerTo = async (gx, gy) => { await rel(-2500, -2000); await sleep(400); await rel(Math.round(gx / mouseScale), Math.round(gy / mouseScale)); await sleep(800); cursor = {x: gx, y: gy}; };
const glide = async (gx, gy) => { if (!cursor) return steerTo(gx, gy); await rel(Math.round((gx - cursor.x) / mouseScale), Math.round((gy - cursor.y) / mouseScale)); await sleep(300); cursor = {x: gx, y: gy}; };
const shot = async suffix => writeFileSync(out.replace(/\.json$/, suffix), Buffer.from((await send('Page.captureScreenshot', {format: 'png'}, page)).data, 'base64'));
// Game-space (1024x768) point to viewport point.
const gamePoint = async (gx, gy) => evaluate(`(()=>{const c=document.querySelector('#game-surface canvas');const r=c.getBoundingClientRect();return {x:r.left+${gx}*r.width/c.width,y:r.top+${gy}*r.height/c.height}})()`);

for (let i = 0; i < 100; i++) { if (await evaluate(`document.readyState==='complete'&&!!document.getElementById('start')`).catch(() => false)) break; await sleep(100); }
await evaluate(`(()=>{const s=document.getElementById('optimization');s.value=${JSON.stringify(build)};s.dispatchEvent(new Event('change',{bubbles:true}));return s.value})()`);
if (args['trace-imports']) await evaluate(`document.getElementById('trace-imports').checked=true`);
const start = await centerOf('#start');
const t0 = performance.now();
await click(start.x, start.y);
log('clicked Run', build);

const timeline = [];
const mark = (event, extra = {}) => { const entry = {t: +((performance.now() - t0) / 1000).toFixed(2), event, ...extra}; timeline.push(entry); log(event, JSON.stringify(extra)); };
const state = () => evaluate(`(()=>{const g=id=>document.getElementById(id)?.textContent??'';const d=document.getElementById('game-dialog');
  const c=document.querySelector('#game-surface canvas');let menu=false;
  if(c&&c.width===1024){try{const o=new OffscreenCanvas(64,16);const x=o.getContext('2d');x.drawImage(c,150,316,64,16,0,0,64,16);const p=x.getImageData(0,0,64,16).data;let bright=0;for(let i=0;i<p.length;i+=4)if(p[i]>200&&p[i+1]>200&&p[i+2]>200)bright++;x.drawImage(c,600,500,64,16,0,0,64,16);const q=x.getImageData(0,0,64,16).data;let dark=0;for(let i=0;i<q.length;i+=4)if(q[i]<40&&q[i+1]<40&&q[i+2]<40)dark++;menu=bright>40&&(dark>900||${args["menu-min"]?1:0});}catch(e){}}
  const log=g('log');return {status:g('status'),frames:g('frame-stats'),dialog:d.open?{text:g('dialog-text').slice(0,80),buttons:[...document.querySelectorAll('#dialog-buttons button')].map(b=>b.textContent)}:null,menu,
  logLength:log.length,stop:(log.match(/STOP: [^\\n]*/)||[null])[0],tail:log.slice(-600)}})()`);

let profiling = null, lastFrames = 0, menuAt = null, firstFrameAt = null, done = false;
let lotPhase = null, menuStreak = 0, stalledSince = 0;
const frameCount = text => Number((text.match(/Game frames: (\d+)/) || [0, 0])[1]);

async function startProfiling() {
  const sessions = [...workers.keys()];
  for (const s of sessions) { try { await send('Profiler.enable', {}, s); await send('Profiler.setSamplingInterval', {interval: 1000}, s); await send('Profiler.start', {}, s); } catch {} }
  profiling = {sessions, startedAt: performance.now()};
  mark('profile-start', {workers: sessions.length});
}
async function stopProfiling() {
  const totals = new Map(); let samples = 0;
  const perWorker = [];
  for (const s of profiling.sessions) {
    let profile; try { profile = (await Promise.race([send('Profiler.stop', {}, s), sleep(5000).then(() => { throw new Error('timeout'); })])).profile; } catch { continue; }
    // --raw-prof: keep every worker's leaf stacks (top 5 by samples) for thread-state analysis.
    if (args['raw-prof']) {
      const nodes = new Map(profile.nodes.map(n => [n.id, n])), up = new Map(); for (const n of profile.nodes) for (const c of n.children ?? []) up.set(c, n.id);
      const tally = new Map(); for (const id of profile.samples ?? []) tally.set(id, (tally.get(id) ?? 0) + 1);
      const stacks = [...tally].sort((a, b) => b[1] - a[1]).slice(0, 5).map(([id, n]) => { const chain = []; for (let cur = id; cur !== undefined && chain.length < 40; cur = up.get(cur)) chain.push(nodes.get(cur).callFrame.functionName || '(anon)'); return {n, chain}; });
      (globalThis.rawProfiles ??= []).push({url: workers.get(s)?.url?.split('/').pop(), samples: profile.samples?.length ?? 0, stacks});
      writeFileSync(out.replace(/\.json$/, '-raw.json'), JSON.stringify(globalThis.rawProfiles, null, 1));
    }
    const byId = new Map(profile.nodes.map(n => [n.id, n]));
    const counts = new Map(); for (const id of profile.samples ?? []) counts.set(id, (counts.get(id) ?? 0) + 1);
    let workerSamples = 0, idle = 0;
    for (const [id, n] of counts) {
      const node = byId.get(id), name = node.callFrame.functionName || `(anonymous ${node.callFrame.url.split('/').pop()}:${node.callFrame.lineNumber})`;
      workerSamples += n; if (name === '(idle)' || name === '(program)') idle += n;
      const key = `${name}`; totals.set(key, (totals.get(key) ?? 0) + n); samples += n;
    }
    const local = new Map();
    for (const [id, n] of counts) { const node = byId.get(id); const name = node.callFrame.functionName || '(anonymous)'; local.set(name, (local.get(name) ?? 0) + n); }
    perWorker.push({url: workers.get(s)?.url?.split('/').pop(), samples: workerSamples, idle,
      top: [...local].sort((a, b) => b[1] - a[1]).slice(0, 25).map(([name, n]) => ({name, pct: +(100 * n / Math.max(1, workerSamples)).toFixed(2)}))});
    // Inclusive time per function (self + descendants) for the busiest names.
    const parent = new Map(); for (const node of profile.nodes) for (const child of node.children ?? []) parent.set(child, node.id);
    const inclusive = new Map();
    for (const [id, n] of counts) { const seen = new Set(); for (let cur = id; cur !== undefined; cur = parent.get(cur)) { const name = byId.get(cur).callFrame.functionName || '(anonymous)'; if (seen.has(name)) continue; seen.add(name); inclusive.set(name, (inclusive.get(name) ?? 0) + n); } }
    const wanted = String(args.callers ?? 'exchange,allocate').split(',');
    perWorker.at(-1).callers = {};
    for (const target of wanted) {
      const byCaller = new Map();
      for (const [id, n] of counts) {
        const chain = []; for (let cur = id; cur !== undefined && chain.length < 6; cur = parent.get(cur)) chain.push(byId.get(cur).callFrame.functionName || '(anonymous)');
        const at = chain.indexOf(target); if (at < 0) continue;
        const key = chain.slice(at + 1, at + 4).join(' < '); byCaller.set(key, (byCaller.get(key) ?? 0) + n);
      }
      perWorker.at(-1).callers[target] = [...byCaller].sort((a, b) => b[1] - a[1]).slice(0, 6).map(([chain, n]) => ({chain, pct: +(100 * n / Math.max(1, workerSamples)).toFixed(2)}));
    }
    perWorker.at(-1).inclusive = [...inclusive].sort((a, b) => b[1] - a[1]).slice(0, 40).map(([name, n]) => ({name, pct: +(100 * n / Math.max(1, workerSamples)).toFixed(2)}));
  }
  const top = [...totals].sort((a, b) => b[1] - a[1]).slice(0, 60).map(([name, n]) => ({name, samples: n, pct: +(100 * n / samples).toFixed(2)}));
  mark('profile-stop', {samples});
  const result = {perWorker, top};
  mkdirSync(dirname(out), {recursive: true}); writeFileSync(out.replace(/\.json$/, '-profile.json'), JSON.stringify(result, null, 2));
  for (const row of top.slice(0, 40)) console.log(`${row.pct.toFixed(2).padStart(6)}%  ${row.samples.toString().padStart(7)}  ${row.name}`);
  for (const w of perWorker) { if (w.samples - w.idle < w.samples * 0.2) continue; console.log(`\n== worker ${w.url}: samples=${w.samples} idle=${w.idle}`);
    console.log('  self: ' + w.top.slice(0, 15).map(r => `${r.name} ${r.pct}%`).join(', '));
    console.log('  inclusive: ' + w.inclusive.slice(0, 30).map(r => `${r.name} ${r.pct}%`).join(', '));
    for (const [name, rows] of Object.entries(w.callers ?? {})) if (rows.length) console.log(`  callers of ${name}: ` + rows.map(r => `[${r.chain}] ${r.pct}%`).join('; ')); }
  return result;
}

let profileResult = null;
while (!done) {
  await sleep(1000);
  const elapsed = (performance.now() - t0) / 1000;
  let s; try { s = await state(); } catch (error) { mark('state-error', {message: error.message}); continue; }
  if (s.dialog) { mark('dialog', s.dialog); const ok = await centerOf('#dialog-buttons button'); if (ok) await click(ok.x, ok.y); continue; }
  const frames = frameCount(s.frames);
  if (frames && !firstFrameAt) { firstFrameAt = elapsed; mark('first-frame', {frames}); }
  if (Math.floor(elapsed) % 30 === 0) mark('progress', {frames, fps: +((frames - lastFrames)).toFixed(0), status: s.status.slice(0, 60)});
  if (frames !== lastFrames || !frames) stalledSince = elapsed; else if (elapsed - stalledSince > Number(args['stall-seconds'] ?? 180)) { mark('stalled', {frames, tail: s.tail.slice(-400)}); break; }
  lastFrames = frames;
  if (s.stop) { mark('stop', {stop: s.stop, tail: s.tail}); done = true; break; }
  if (!/Continuing|Starting|Loading|Running|Mount|Download|original/i.test(s.status) && /ended|Stopped|error|failed/i.test(s.status)) { mark('ended', {status: s.status, tail: s.tail}); break; }
  if (profileFrom !== null && !profiling && elapsed >= profileFrom) await startProfiling();
  if (profiling && !profileResult && (performance.now() - profiling.startedAt) / 1000 >= profileSeconds) profileResult = await stopProfiling();
  menuStreak = s.menu && frames > 300 ? menuStreak + 1 : 0;
  if (until === 'menu' && menuAt && (!profiling || profileResult)) break;
  if (menuStreak >= 3 && !menuAt && elapsed >= Number(args['menu-min'] ?? 0)) {
    menuAt = elapsed; mark('menu', {frames});
    if (until === 'menu' && (!profiling || profileResult)) break;
    if ((until === 'lot' || args['new-game']) && !lotPhase) {
      // Hover Game, then click the first submenu entry ("New Game").
      mkdirSync(dirname(out), {recursive: true});
      await steerTo(Number(args['game-x'] ?? 150), Number(args['game-y'] ?? 318)); await sleep(800); await gameClick(); await sleep(3000); await shot('-submenu.png');
      if (args['calibrate']) {
        // --calibrate-wait <s>: stay on the submenu, then print --final-js <base64>.
        if (args['calibrate-wait']) { await sleep(Number(args['calibrate-wait']) * 1000); await shot('-submenu-late.png'); }
        if (args['final-js']) console.log('final-js', JSON.stringify(await evaluate(Buffer.from(String(args['final-js']), 'base64').toString())));
        mark('calibration-shot'); break;
      }
      await steerTo(Number(args['newgame-x'] ?? 260), Number(args['newgame-y'] ?? 364)); await sleep(1500); await shot('-newgame-hover.png');
      if (args['pre-js']) log('pre-js', await evaluate(Buffer.from(String(args['pre-js']), 'base64').toString()));
      await gameClick();
      lotPhase = {clickedAt: elapsed, framesAt: frames}; mark('new-game-clicked', {frames});
    }
  }
  if (lotPhase && !lotPhase.lotAt && frames - lotPhase.framesAt > 400 && elapsed - lotPhase.clickedAt > Number(args['lot-min'] ?? 30)) {
    lotPhase.lotAt = elapsed; mark('lot-drawing', {frames, loadSeconds: +(elapsed - lotPhase.clickedAt).toFixed(1)});
    writeFileSync(out.replace(/\.json$/, '-lot.png'), Buffer.from((await send('Page.captureScreenshot', {format: 'png'}, page)).data, 'base64'));
    if (args['lot-clicks']) {
      // "x,y;x,y": steer the game cursor to each game-space point, click, wait, screenshot.
      let i = 0;
      for (const point of String(args['lot-clicks']).split(';')) {
        const [x, y, button = 0] = point.split(',').map(Number); await steerTo(x, y); await sleep(600); await shot(`-click${i}-hover.png`); await gameClick(button); await sleep(Number(args['click-wait'] ?? 5000));
        await shot(`-click${i}.png`); mark('lot-click', {x, y, frames: frameCount((await state()).frames)}); i++;
      }
    }
    if (args.script) {
      // Steps separated by ';': key:Code | type:text | click:x,y[,button] | wait:ms | shot:name
      await evaluate(`document.querySelector('#game-surface canvas').focus()`);
      const press = async (code, text) => {
        const named = {ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Enter: 13, Escape: 27, Space: 32, Backspace: 8, Tab: 9};
        const keyCode = named[code] ?? (code.startsWith('Key') ? code.charCodeAt(3) : code.startsWith('Digit') ? 48 + Number(code.slice(5)) : 0);
        const key = text ?? (code === 'Space' ? ' ' : code);
        await send('Input.dispatchKeyEvent', {type: 'keyDown', code, key, windowsVirtualKeyCode: keyCode, ...(text ? {text} : {})}, page); await sleep(120);
        await send('Input.dispatchKeyEvent', {type: 'keyUp', code, key, windowsVirtualKeyCode: keyCode}, page); await sleep(120);
      };
      const runSteps = async script => { for (const step of String(script).split(';').map(x => x.trim()).filter(Boolean)) {
        const [kind, value = ''] = [step.slice(0, step.indexOf(':')), step.slice(step.indexOf(':') + 1)];
        if (kind === 'key') await press(value);
        else if (kind === 'type') for (const ch of value) await press(/[a-z]/i.test(ch) ? 'Key' + ch.toUpperCase() : /[0-9]/.test(ch) ? 'Digit' + ch : 'Space', ch === ' ' ? ' ' : ch);
        else if (kind === 'click') { const [x, y, button = 0] = value.split(',').map(Number); await glide(x, y); await sleep(500); await gameClick(button); }
        else if (kind === 'steer') { const [x, y] = value.split(',').map(Number); await steerTo(x, y); }
        else if (kind === 'glide') { const [x, y] = value.split(',').map(Number); await glide(x, y); }
        else if (kind === 'down' || kind === 'up') { const b = Number(value || 0); await evaluate(`(()=>{const c=document.querySelector('#game-surface canvas');c.focus();c.dispatchEvent(new PointerEvent('pointer${kind}',{bubbles:true,pointerType:'mouse',pointerId:1,isPrimary:true,button:${b},buttons:${kind === 'down' ? (b === 2 ? 2 : 1) : 0},clientX:0,clientY:0,movementX:0,movementY:0}));return true})()`); }
        else if (kind === 'wait') await sleep(Number(value));
        else if (kind === 'shot') await shot(`-${value}.png`);
        else if (kind === 'js') writeFileSync(out.replace(/\.json$/, '-js.txt'), String(await evaluate(Buffer.from(value, 'base64').toString())));
        else if (kind === 'prof') { await startProfiling(); await sleep(Number(value) * 1000); const r = await stopProfiling(); profiling = null; writeFileSync(out.replace(/\.json$/, '-prof.json'), JSON.stringify(r, null, 2)); }
        else if (kind === 'wjs') { const [where, code] = value.split('@'); const w = [...workers].find(([, x]) => (x.url ?? '').includes(where))?.[0];
          const r = w ? await send('Runtime.evaluate', {expression: Buffer.from(code, 'base64').toString(), returnByValue: true}, w) : null;
          writeFileSync(out.replace(/\.json$/, '-js.txt'), String(r?.result?.value ?? JSON.stringify(r?.exceptionDetails ?? 'no worker'))); }
        else if (kind === 'tail') writeFileSync(out.replace(/\.json$/, '-tail.txt'), (await state()).tail);
        mark('step', {step, frames: frameCount((await state()).frames)});
      } };
      await runSteps(args.script);
      if (args.interactive) {
        // Drive the running game from a command file: write steps to it, wait for <file>.done.
        const file = String(args.interactive);
        for (;;) {
          if (existsSync(file)) { const steps = readFileSync(file, 'utf8'); unlinkSync(file); if (steps.trim() === 'quit') break;
            try { await runSteps(steps); writeFileSync(file + '.done', 'ok ' + Date.now()); } catch (e) { writeFileSync(file + '.done', 'error ' + e.message); } }
          const s = await state(); if (s.stop) { writeFileSync(file + '.done', 'STOP ' + s.stop); mark('stop', {stop: s.stop, tail: s.tail}); break; }
          await sleep(300);
        }
      }
    }
    if (args.keys) {
      // Trusted key presses on the focused game canvas: "code,code,..." (KeyboardEvent.code).
      await evaluate(`document.querySelector('#game-surface canvas').focus()`);
      const vk = {ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Enter: 13, Escape: 27, Space: 32, Backspace: 8};
      for (const code of String(args.keys).split(',')) {
        const key = code.startsWith('Key') ? code.slice(3).toLowerCase() : code === 'Space' ? ' ' : code;
        const keyCode = vk[code] ?? (code.startsWith('Key') ? code.charCodeAt(3) : 0);
        await send('Input.dispatchKeyEvent', {type: 'keyDown', code, key, windowsVirtualKeyCode: keyCode, ...(key.length === 1 ? {text: key} : {})}, page); await sleep(300);
        await send('Input.dispatchKeyEvent', {type: 'keyUp', code, key, windowsVirtualKeyCode: keyCode}, page); await sleep(200);
      }
      await sleep(2000); await shot('-keys.png'); mark('keys', {frames: frameCount((await state()).frames)});
    }
    if (args['skip-draws']) {
      const gpu = [...workers].find(([, w]) => (w.url ?? '').includes('graphics-worker'))?.[0];
      for (const spec of String(args['skip-draws']).split(';')) {
        const list = spec.split(',').flatMap(r => { const [a, b] = r.split('-').map(Number); return b ? Array.from({length: b - a + 1}, (_, i) => a + i) : [a]; });
        await send('Runtime.evaluate', {expression: `globalThis.moviesSkipDraws=${JSON.stringify(list)}`}, gpu); await sleep(2500);
        await shot(`-skip-${spec.replace(/[^0-9-]/g, '_')}.png`);
      }
      await send('Runtime.evaluate', {expression: 'globalThis.moviesSkipDraws=undefined'}, gpu); mark('skip-draws');
    }
    if (args['draw-steps']) {
      const gpu = [...workers].find(([, w]) => (w.url ?? '').includes('graphics-worker'))?.[0];
      for (const n of String(args['draw-steps']).split(',').map(Number)) {
        await send('Runtime.evaluate', {expression: `globalThis.moviesDrawLimit=${n}`}, gpu); await sleep(2500);
        await shot(`-draws-${String(n).padStart(4, '0')}.png`);
      }
      await send('Runtime.evaluate', {expression: 'globalThis.moviesDrawLimit=undefined'}, gpu); mark('draw-steps');
    }
    if (args['trace-at']) {
      for (let i = 0; i < 120 && !(await evaluate('!!globalThis.__graphicsTrace')); i++) await sleep(500);
      const entries = await evaluate('JSON.stringify(globalThis.__graphicsTrace ?? [])');
      writeFileSync(out.replace(/\.json$/, '-trace.json'), entries); mark('trace', {entries: JSON.parse(entries).length});
    }
    if (until === 'lot' && (!profiling || profileResult)) break;
  }
  if (until.startsWith('seconds:') && elapsed >= Number(until.slice(8)) && (!profiling || profileResult)) break;
  if (elapsed > deadlineSeconds) { mark('deadline'); break; }
}
if (profiling && !profileResult) profileResult = await stopProfiling();
const final = await state().catch(() => null);
mkdirSync(dirname(out), {recursive: true});
writeFileSync(out.replace(/\.json$/, '-final.png'), Buffer.from((await send('Page.captureScreenshot', {format: 'png'}, page)).data, 'base64'));
const fullLog = await evaluate(`document.getElementById('log').textContent`).catch(() => '');
writeFileSync(out.replace(/\.json$/, '.log'), fullLog);
if (args['final-js'] && !args['calibrate']) { try { console.log('final-js', JSON.stringify(await evaluate(Buffer.from(String(args['final-js']), 'base64').toString()))); } catch (error) { console.log('final-js error', error.message); } }
writeFileSync(out, JSON.stringify({build, until, firstFrameAt, menuAt, lot: lotPhase, timeline, final, profile: profileResult, workers: [...workers.values()]}, null, 2));
log('wrote', out, 'firstFrame', firstFrameAt, 'menu', menuAt, 'lot', JSON.stringify(lotPhase));
if (profileResult) for (const row of profileResult.top.slice(0, 30)) console.log(`${row.pct.toFixed(2).padStart(6)}%  ${row.samples.toString().padStart(7)}  ${row.name}`);
await send('Browser.close').catch(() => {});
process.exit(0);
