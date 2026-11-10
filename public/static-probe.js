'use strict';
// BEGIN THREAD FAILURE WATCHDOG
function createThreadFailureWatch(descriptor, onFailure, timers = globalThis) {
  const {sharedWasmBuffer, flagOffset, errorOffset, errorCapacity} = descriptor || {};
  const integer = value => Number.isSafeInteger(value) && value >= 0;
  if (!(sharedWasmBuffer instanceof SharedArrayBuffer) || !integer(flagOffset) || flagOffset % 4 ||
      !integer(errorOffset) || !integer(errorCapacity) || errorCapacity < 1 || errorCapacity > 4096 ||
      flagOffset + 4 > sharedWasmBuffer.byteLength || errorOffset + errorCapacity > sharedWasmBuffer.byteLength ||
      (flagOffset < errorOffset + errorCapacity && errorOffset < flagOffset + 4)) {
    throw new Error('Invalid shared thread failure descriptor');
  }
  // Views remain valid across shared Wasm growth: both globals are in initial memory.
  const flag = new Int32Array(sharedWasmBuffer, flagOffset, 1);
  const message = new Uint8Array(sharedWasmBuffer, errorOffset, errorCapacity);
  let closed = false, timer = null;
  function close() { if (closed) return; closed = true; if (timer !== null) timers.clearInterval(timer); }
  function poll() {
    if (closed) return;
    const ready = Atomics.load(flag, 0);
    if (!ready) return;
    close();
    if (ready !== 1) { onFailure('Invalid published thread failure state'); return; }
    // TextDecoder rejects shared ArrayBufferViews in supported browsers.
    const copy = message.slice(), nul = copy.indexOf(0);
    const text = new TextDecoder().decode(nul < 0 ? copy : copy.subarray(0, nul));
    onFailure(text || 'Guest child thread failed without a diagnostic');
  }
  timer = timers.setInterval(poll, 100);
  poll();
  return {close};
}
// END THREAD FAILURE WATCHDOG
let threadFailureWatch = null;
let ownedPthreads = null;
let frameMetrics = {count: 0, sampleCount: 0, sampleAt: 0, rate: null, firstAt: 0, lastAt: 0, reported: false};
let createAudioSink = null;
const audioModuleReady = import('./audio-sink.js').then(module => { createAudioSink = module.createMoviesAudioSink; }).catch(() => {});
let audioSink = null;
const start = document.getElementById('start');
const stop = document.getElementById('stop');
const status = document.getElementById('status');
const log = document.getElementById('log');
const optimization = document.getElementById('optimization');
/* play.html (the public page) sets data-mode="play": one fixed build, game files
 * from the player's chosen folder (play.js sets globalThis.moviesGameFiles), and
 * no diagnostic controls. static-probe.html is the local diagnostic page. */
const playMode = document.body?.dataset?.mode === 'play';
const buildValue = () => optimization?.value ?? '1-threads';
const checked = id => document.getElementById(id)?.checked === true;
optimization?.addEventListener('change', () => {
  document.getElementById('provenance').href = `runtime/static-probe${optimization.value === '0' ? '' : '-o' + optimization.value}/manifest.json`;
});
let worker = null;
let mediaService = null;
let readerService = null;
let graphicsTransport = null;
let graphicsPresenter = null;
let graphicsPort = null;
let input = null;
let cursorSnapshot = null, observedPointer = null;
let pointerLock = null;
const captureMouse = document.getElementById('capture-mouse');
const mouseStatus = document.getElementById('mouse-status');
captureMouse.addEventListener('click', async () => {
  const host = pointerLock;
  if (!host) return;
  try { await host.request(); }
  catch (error) { if (pointerLock === host) mouseStatus.textContent = `Mouse capture unavailable: ${error.message}`; }
});
/* Fullscreen covers the game surface only. Entering it also captures the mouse;
 * Escape releases the mouse (the browser's rule) and shows a bar with a way back
 * into the game or out of fullscreen. A second Escape leaves fullscreen. */
const gameSurface = document.getElementById('game-surface');
const fullscreenButton = document.getElementById('fullscreen');
const fullscreenBar = document.getElementById('fullscreen-bar');
const captureNow = () => { pointerLock?.request(); };
function syncFullscreenBar() {
  const locked = !!document.pointerLockElement && gameSurface.contains(document.pointerLockElement);
  fullscreenBar.hidden = locked || document.fullscreenElement !== gameSurface;
}
fullscreenButton.addEventListener('click', async () => {
  try { await gameSurface.requestFullscreen({navigationUI: 'hide'}); captureNow(); }
  catch (error) { mouseStatus.textContent = `Fullscreen unavailable: ${error.message}`; }
});
document.getElementById('fullscreen-capture').addEventListener('click', captureNow);
document.getElementById('fullscreen-exit').addEventListener('click', () => { document.exitFullscreen?.().catch(() => {}); });
/* Escape in fullscreen. Normally the browser takes Esc itself (releasing the mouse
 * or leaving fullscreen) and the game never sees it. With the Keyboard Lock API
 * (Chrome/Edge, fullscreen only) Esc reaches the page, so:
 *   1st Esc  -> delivered to the game (opens its pause/options menu);
 *   2nd Esc  -> swallowed (the game stays paused); leaves fullscreen and releases the mouse.
 * Any other key or a click in the game re-arms the first step. Holding Esc still
 * exits fullscreen, as Chrome always allows. Browsers without Keyboard Lock keep
 * their default Esc behaviour. */
let escapeTaken = false, swallowEscapeUp = false, escapeHintTimer = null;
// Hint shown inside the fullscreen element (anything outside it is not visible).
const escapeHint = document.createElement?.('div');
if (escapeHint) {
  escapeHint.id = 'escape-hint'; escapeHint.hidden = true; escapeHint.setAttribute('role', 'status');
  escapeHint.textContent = 'Press Esc again to exit fullscreen';
  Object.assign(escapeHint.style, {position: 'fixed', top: '16px', left: '50%', transform: 'translateX(-50%)', zIndex: '2', background: '#000d',
    color: '#fff', border: '1px solid #ffffff33', borderRadius: '10px', padding: '8px 16px', font: '600 15px system-ui, sans-serif', pointerEvents: 'none'});
  gameSurface.append?.(escapeHint);
}
function showEscapeHint(visible) {
  if (!escapeHint) return;
  clearTimeout(escapeHintTimer); escapeHint.hidden = !visible;
  if (visible) escapeHintTimer = setTimeout(() => { escapeHint.hidden = true; }, 3000);
}
function syncKeyboardLock() {
  escapeTaken = false; showEscapeHint(false);
  if (document.fullscreenElement === gameSurface) navigator.keyboard?.lock?.(['Escape']).catch(() => {});
  else navigator.keyboard?.unlock?.();
}
globalThis.addEventListener?.('keydown', event => {
  if (document.fullscreenElement !== gameSurface) return;
  if (event.code !== 'Escape') { escapeTaken = false; showEscapeHint(false); return; }
  if (event.repeat) { event.stopImmediatePropagation(); event.preventDefault(); return; }
  if (!escapeTaken) { escapeTaken = true; if (navigator.keyboard?.lock) showEscapeHint(true); return; } // first press: let the game have it
  event.stopImmediatePropagation(); event.preventDefault(); swallowEscapeUp = true;
  document.exitPointerLock?.(); document.exitFullscreen?.().catch(() => {});
}, true);
globalThis.addEventListener?.('keyup', event => {
  if (event.code === 'Escape' && swallowEscapeUp) { swallowEscapeUp = false; event.stopImmediatePropagation(); event.preventDefault(); }
}, true);
globalThis.addEventListener?.('pointerdown', event => { if (gameSurface.contains?.(event.target) && event.target?.tagName === 'CANVAS') { escapeTaken = false; showEscapeHint(false); } }, true);
document.addEventListener?.('fullscreenchange', syncFullscreenBar);
document.addEventListener?.('fullscreenchange', syncKeyboardLock);
document.addEventListener?.('pointerlockchange', syncFullscreenBar);
let directInput = null, droppedInput = 0;
/* Saved settings/registry/saves: OPFS store, loaded before each run and updated
 * from the game worker's /userdata changes (public/userdata-store.js). */
let userdataStorePromise = null;
const userdataStore = () => userdataStorePromise ??= import('./userdata-store.js').then(({MoviesUserdataStore}) => new MoviesUserdataStore({
  onError(error, change) { pendingLog += `[userdata] could not save ${change?.op} ${change?.path ?? change?.to ?? ''}: ${error?.message ?? error}\n`; if (logTimer === null) logTimer = setTimeout(flushLog, 50); }}));
let pendingLog = '';
let logTimer = null;
const dialog = document.getElementById('game-dialog');
let answerDialog = null;
const surface = new MoviesWindowSurface(document.getElementById('game-surface'), document, event => {
  // A busy game (e.g. loading) stops reading messages; Windows then drops or
  // coalesces input rather than terminating it. Motion keeps a quarter of the
  // queue free for buttons and keys.
  if (input && !input.offer(event, event.message === 0x200 ? input.capacity >> 2 : 0) && droppedInput++ % 500 === 0)
    pendingLog += `[input] window message queue full; dropped ${droppedInput} input message(s) while the game was busy\n`;
}, () => cursorSnapshot?.isLocked() ? cursorSnapshot.read() : null);
// Observe genuine page input before Run creates the guest window. Re-map on
// geometry updates while page input is active. Blur retains the last virtual
// coordinate; it does not synthesize movement from stale page coordinates.
function publishPointer() {
  if (!cursorSnapshot) return;
  if (!observedPointer) return; // Keyboard-only launch has no invented initial point.
  if (observedPointer.active === false) {
    const retained=cursorSnapshot.read() || observedPointer.virtual;
    if (retained) { cursorSnapshot.write(retained); return; }
    // A genuine pre-window click may be mapped once when geometry first exists.
  }
  const entry=surface.windows.size===1 ? surface.windows.entries().next().value : null;
  const geometry=entry && surface.geometry?.get(entry[0]), rect=entry && entry[1].getBoundingClientRect();
  // A minimized virtual window retains the last genuinely observed cursor;
  // its zero-size DOM rect must not block GetCursorPos and the restore queue.
  if (geometry?.minimized && cursorSnapshot.read()) return;
  if (!geometry || !rect?.width || !rect?.height) return;
  const point={x:Math.round(geometry.clientX+(observedPointer.x-rect.left)*entry[1].width/rect.width),
    y:Math.round(geometry.clientY+(observedPointer.y-rect.top)*entry[1].height/rect.height)};
  cursorSnapshot.write(point);observedPointer.virtual=point;
}
function observePointer(event) {
  if(event.pointerType && event.pointerType!=='mouse')return;
  if(!Number.isFinite(event.clientX)||!Number.isFinite(event.clientY))return;
  observedPointer={x:event.clientX,y:event.clientY,active:true};publishPointer();
}
function losePointer() {
  if (!observedPointer) return;
  observedPointer.active=false;
  const point=cursorSnapshot?.read();if(point)observedPointer.virtual=point;
  // DirectInput/pointer-lock listeners independently release focus/buttons.
}
for(const name of ['pointermove','pointerdown','pointerup'])document.addEventListener?.(name,observePointer,true);
document.addEventListener?.('pointerleave',losePointer);
document.addEventListener?.('visibilitychange',()=>{if(document.hidden)losePointer();});
globalThis.addEventListener?.('blur',losePointer);
globalThis.addEventListener?.('resize',publishPointer);
document.addEventListener?.('scroll',publishPointer,true);
function showDialog(data) {
  const policy = MoviesDialogPolicy(data.flags);
  const response = new Int32Array(data.response);
  document.getElementById('dialog-title').textContent = data.title;
  document.getElementById('dialog-text').textContent = data.text;
  const icon = document.getElementById('dialog-icon');
  icon.textContent = ({16:'Error',32:'Question',48:'Warning',64:'Information'})[data.flags & 0xf0] || '';
  icon.hidden = !icon.textContent;
  const buttons = document.getElementById('dialog-buttons');
  buttons.replaceChildren();
  answerDialog = id => {
    if (!policy.buttons.some(button => button.id === id)) return;
    answerDialog = null;
    dialog.close();
    Atomics.store(response, 0, id);
    Atomics.notify(response, 0);
  };
  for (const item of policy.buttons) {
    const button = document.createElement('button');
    button.textContent = item.label;
    button.addEventListener('click', () => answerDialog?.(item.id));
    buttons.append(button);
  }
  dialog.oncancel = event => {
    event.preventDefault();
    if (policy.escapeId !== null) answerDialog?.(policy.escapeId);
  };
  status.textContent = 'The game is waiting for your response.';
  dialog.showModal();
  buttons.children[policy.defaultIndex].focus();
}
function flushLog() {
  if (logTimer !== null) clearTimeout(logTimer);
  logTimer = null;
  if (!pendingLog) return;
  // Full API traces need far more than the 1 MiB normally kept on the page.
  log.textContent = (log.textContent + pendingLog).slice(-(document.getElementById('trace-imports')?.checked ? 32 : 1) * 1024 * 1024);
  pendingLog = '';
  log.scrollTop = log.scrollHeight;
}
const began = performance.now();
function updateFrameMetrics() {
  const element = document.getElementById('frame-stats');
  if (!element) return;
  const now = performance.now();
  if (worker && frameMetrics.count && now - frameMetrics.sampleAt >= 1000) {
    frameMetrics.rate = (frameMetrics.count - frameMetrics.sampleCount) * 1000 / (now - frameMetrics.sampleAt);
    frameMetrics.sampleCount = frameMetrics.count; frameMetrics.sampleAt = now;
  }
  const rate = !worker ? 'stopped' : frameMetrics.rate === null ? 'measuring' : `${frameMetrics.rate.toFixed(1)} presented frames/s`;
  element.textContent = `Game frames: ${frameMetrics.count}${frameMetrics.count ? ` (${rate})` : ' — no presentation yet'}`;
  const inputStats = document.getElementById('input-stats');
  if (inputStats) {
    const hwnd = directInput?.control ? Atomics.load(directInput.control, 1) : 0;
    const focused = typeof document.hasFocus === 'function' && document.hasFocus();
    const cursor = cursorSnapshot?.isLocked() ? 'captured' : cursorSnapshot?.read() ? 'position available' : 'position unavailable';
    const minimized = [...surface.geometry.values()].some(state => state.minimized);
    inputStats.textContent = `Input: page ${document.hidden ? 'hidden' : 'visible'}, ${focused ? 'focused' : 'unfocused'}; game focus ${hwnd ? '0x' + hwnd.toString(16) : 'none'}; cursor ${cursor}.${minimized ? ' Window minimized — use Restore game window.' : ''}`;
  }
}
/* Focus messages. The original (WndProc 00542380 -> 00553f50) keeps an "own window
 * has focus" flag at [00e52c9c]; 005550b0 clears it each frame while another window
 * is foreground, and in the menus only WM_SETFOCUS sets it again. Without that,
 * leaving the page froze the menu cursor for the rest of the run, so every gain of
 * page focus by the game canvas posts WM_SETFOCUS.
 * WM_KILLFOCUS is deliberately NOT posted: the original's handler (00542580 ->
 * 009a5d60) treats it as fullscreen device loss, minimizes the window and loops
 * until TestCooperativeLevel reports D3DERR_DEVICENOTRESET, then Resets the device.
 * The browser device is never lost and Reset is not modelled, so the game behaves
 * like a windowed application that keeps running in the background. */
let gameFocusHwnd = 0;
function syncGameFocus() {
  const target = directInput?.sync() ?? 0;
  if (!input || target === gameFocusHwnd) return;
  const previous = gameFocusHwnd;
  if (target) input.offer({hwnd: target, message: 7, wParam: previous && surface.windows.has(previous) ? previous : 0, lParam: 0,
    time: Math.floor(performance.timeOrigin + performance.now()) >>> 0, x: 0, y: 0});
  gameFocusHwnd = target;
}
for (const [target, name] of [[globalThis, 'focus'], [globalThis, 'blur'], [document, 'visibilitychange'], [document, 'focusin'], [document, 'focusout']])
  target.addEventListener?.(name, () => queueMicrotask(syncGameFocus), true);
setInterval(() => {
  const heartbeat = document.getElementById('heartbeat');
  if (heartbeat) heartbeat.textContent = `UI heartbeat: ${Math.floor((performance.now() - began) / 1000)} s`;
  syncGameFocus();
  updateFrameMetrics();
}, 1000);
function disposeMediaService() {
  const service = mediaService;
  mediaService = null;
  if (!service) return;
  // Wake a blocked guest even if the service crashed before replying. The
  // fatal flag is independent of potentially in-flight JSON payload writes.
  Atomics.store(service.control, 1, 1);
  Atomics.store(service.control, 0, 2);
  Atomics.notify(service.control, 0);
  service.worker.terminate();
  service.port.close();
}
function disposeReaderService() {
  const service = readerService;
  readerService = null;
  if (!service) return;
  Atomics.store(service.control, 1, 1);
  Atomics.store(service.control, 0, 2);
  Atomics.notify(service.control, 0);
  service.worker.terminate();
  service.port.close();
}
function finish(text) {
  threadFailureWatch?.close(); threadFailureWatch = null;
  answerDialog = null;
  if (dialog.open) dialog.close();
  if (frameMetrics.count && !frameMetrics.reported) {
    frameMetrics.reported = true;
    const span = frameMetrics.lastAt - frameMetrics.firstAt;
    const average = span > 0 ? ((frameMetrics.count - 1) * 1000 / span).toFixed(2) : 'n/a';
    pendingLog += `[graphics] Presentation sample: frames=${frameMetrics.count} first-to-last-ms=${span.toFixed(1)} mean-presented-fps=${average}; not a controlled gameplay benchmark.\n`;
  }
  flushLog();
  status.textContent = text;
  disposeReaderService();
  pointerLock?.close(); pointerLock = null;
  captureMouse.disabled = true; fullscreenButton.disabled = true;
  if (document.fullscreenElement === gameSurface) document.exitFullscreen?.().catch(() => {});
  disposeMediaService();
  graphicsPresenter?.close(); graphicsPresenter = null;
  graphicsPort?.close(); graphicsPort = null;
  graphicsTransport?.close(); graphicsTransport = null;
  ownedPthreads?.close(); ownedPthreads = null;
  worker?.terminate(); worker = null;
  const closingAudio = audioSink; audioSink = null;
  closingAudio?.close().catch(() => {});
  input = null; gameFocusHwnd = 0;
  directInput?.dispose(); directInput = null;
  cursorSnapshot?.write(null); cursorSnapshot = null;
  start.disabled = false; stop.disabled = true;
  if (optimization) optimization.disabled = false;
  globalThis.dispatchEvent?.(new CustomEvent('movies-game-stopped', {detail: {text}}));
  updateFrameMetrics();
}
start.addEventListener('click', async () => {
  if (playMode && !globalThis.moviesGameFiles?.length) { status.textContent = 'Choose your game folder first.'; return; }
  surface.clear();
  frameMetrics = {count: 0, sampleCount: 0, sampleAt: 0, rate: null, firstAt: 0, lastAt: 0, reported: false};
  updateFrameMetrics();
  if (logTimer !== null) clearTimeout(logTimer);
  logTimer = null; pendingLog = '';
  log.textContent = ''; start.disabled = true; stop.disabled = false;
  status.textContent = 'Starting worker…';
  if (optimization) optimization.disabled = true;
  worker = new Worker('static-probe-worker.js');
  const cursorBuffer = MoviesPointerSnapshot.create();
  cursorSnapshot = new MoviesPointerSnapshot(cursorBuffer);
  const inputBuffer = MoviesWindowInput.create(1024);
  input = new MoviesWindowInput(inputBuffer);
  const directInputBuffer = MoviesWindowInput.create(1024, MoviesDirectInputFields);
  const directInputControl = new SharedArrayBuffer(MoviesAsyncKeyControlBytes);
  directInput = new MoviesDirectInputHost(directInputBuffer, directInputControl, text => finish(text));
  const activeWorker = worker;
  const profileGraphics = checked('profile-graphics');
  const reportGraphicsMetrics = metrics => {
    if (worker !== activeWorker) return;
    pendingLog += `[graphics-profile] ${JSON.stringify(metrics)}\n`;
    if (logTimer === null) logTimer = setTimeout(flushLog, 50);
  };
  worker.onerror = (event) => { if (worker === activeWorker) finish(`Worker error: ${event.message}`); };
  let audio;
  try {
    if (!createAudioSink) throw new Error('Audio module is still loading; click Run again.');
    audio = await createAudioSink({onError(error) { if (worker === activeWorker) finish(`Audio renderer error: ${error.message}`); }});
    if (worker !== activeWorker) { await audio.close(); return; }
    audioSink = audio;
    audio.context.onstatechange = () => {
      if (worker === activeWorker && audio.context.state !== 'running') finish('Audio context stopped or was suspended.');
    };
  } catch (error) { if (worker === activeWorker) finish(`Audio setup error: ${error.message}`); return; }
  let graphics;
  try {
    const {prepareGraphicsTransport} = await import('./graphics-host.js');
    const {createGraphicsPresenter} = await import('./graphics-presentation.js');
    const {MoviesPointerLockHost} = await import('./pointer-lock.js');
    if (worker !== activeWorker) return;
    pointerLock = new MoviesPointerLockHost({snapshot: cursorSnapshot, surface, onStatus({locked, lost, message}) {
      if (worker !== activeWorker) return;
      if (lost) {
        // Releasing capture clears held buttons/motion but, as on Windows, the
        // game window keeps focus; moving focus to the page button silenced all input.
        directInput?.blur();
        losePointer();
        directInput?.sync();
      }
      mouseStatus.textContent = message || (locked ? 'Mouse captured — press Escape to release.' : 'Click Capture mouse to control the game; Escape releases it.');
    }});
    const transport = await prepareGraphicsTransport({width:1024, height:768});
    if (worker !== activeWorker) { transport.close(); return; }
    graphicsTransport = transport;
    const channel = new MessageChannel();
    const ack = new Int32Array(new SharedArrayBuffer(12)); // serial, success, page-hidden flag
    graphicsPort = channel.port1;
    let firstPresentLogged = false;
    graphicsPresenter = createGraphicsPresenter({ack, profile: profileGraphics, onMetrics: reportGraphicsMetrics, consume(bitmap, serial) {
      if (worker !== activeWorker || surface.windows.size !== 1) throw new Error('Presentation has no unique game window');
      const canvas = surface.windows.values().next().value;
      if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) throw new Error('GPU frame differs from game window dimensions');
      const context = canvas.getContext('2d');
      if (!context) throw new Error('Game presentation context unavailable');
      context.drawImage(bitmap, 0, 0);
      ++frameMetrics.count;
      frameMetrics.lastAt = performance.now();
      if (!firstPresentLogged) {
        firstPresentLogged = true;
        frameMetrics.firstAt = frameMetrics.lastAt;
        frameMetrics.sampleAt = frameMetrics.lastAt; frameMetrics.sampleCount = frameMetrics.count;
        updateFrameMetrics();
        pendingLog += `[graphics] First original Present consumed: serial=${serial} size=${bitmap.width}x${bitmap.height}\n`;
        if (logTimer === null) logTimer = setTimeout(flushLog, 50);
      }
    }, onError(error) { if (worker === activeWorker) pendingLog += `GPU presentation: ${error.message}\n`; }});
    graphicsPort.onmessage = ({data}) => {
      if (worker !== activeWorker) { data.bitmap?.close?.(); return; }
      try {
        if (data.type === 'graphics-hidden-frame') {
          ++frameMetrics.count; frameMetrics.lastAt = performance.now();
          // Occasional unacknowledged snapshot keeps a hidden canvas roughly current.
          if (data.bitmap) { try { const canvas = surface.windows.size === 1 ? surface.windows.values().next().value : null; if (canvas && canvas.width === data.bitmap.width && canvas.height === data.bitmap.height) canvas.getContext('2d')?.drawImage(data.bitmap, 0, 0); } finally { data.bitmap.close(); } }
          return;
        }
        if (data.type === 'graphics-slow-present') {
          const ms = k => Number.isFinite(data[k]) ? data[k].toFixed(1) : '?';
          if (data.dropped) pendingLog += `[graphics] page did not acknowledge frames within 1 s (display asleep or tab throttled); ${data.dropped} frame(s) dropped, game continues\n`; else
          pendingLog += `[graphics] slow Present serial=${data.serial} preValidate=${ms('preValidateMs')}ms bitmap=${ms('bitmapMs')}ms pageAck=${ms('ackMs')}ms postValidate=${ms('postValidateMs')}ms visibility=${document.visibilityState}\n`;
          if (logTimer === null) logTimer = setTimeout(flushLog, 50); return;
        }
        if (data.type !== 'graphics-frame') throw new Error('Unknown GPU presentation message');
        graphicsPresenter.submit(data);
      } catch (error) { finish(`GPU presentation error: ${error.message}`); }
    };
    graphics = {profile: profileGraphics, width:1024, height:768, presentationPort:channel.port2, presentationAck:ack.buffer,
      commandPort:transport.commandPort, responseBuffer:transport.responseBuffer};
  } catch (error) { if (worker === activeWorker) finish(`GPU setup error: ${error.message}`); return; }
  if (buildValue().endsWith('-threads')) {
    try {
      const {createOwnedWorkerHost} = await import('./owned-worker-host.js');
      if (worker !== activeWorker) return;
      const level = Number(buildValue().split('-')[0]);
      ownedPthreads = createOwnedWorkerHost({
        allowedURL: new URL(`runtime/static-probe${level ? '-o' + level : ''}-threads/probe.js`, document.baseURI ?? location.href).href,
        onError(error) { if (worker === activeWorker) finish(`Pthread worker error: ${error.message}`); }
      });
    } catch (error) { if (worker === activeWorker) finish(`Pthread ownership setup error: ${error.message}`); return; }
  }
  let media;
  try {
    const channel = new MessageChannel();
    media = {
      worker: new Worker('media-service-worker.js'),
      control: new Int32Array(new SharedArrayBuffer(20)),
      payload: new SharedArrayBuffer(1048576),
      port: channel.port2,
    };
    mediaService = media;
    const failMedia = event => {
      if (worker !== activeWorker || mediaService !== media) return;
      pendingLog += `Media service failed: ${event.message || 'message transport error'}\n`;
      finish(`Media service error: ${event.message || 'message transport error'}`);
    };
    media.worker.onerror = failMedia;
    media.worker.onmessageerror = failMedia;
    media.worker.postMessage({op: 'attachPort', port: channel.port1}, [channel.port1]);
  } catch (error) { finish(`Media service setup error: ${error.message}`); return; }
  let reader;
  try {
    const channel = new MessageChannel();
    reader = {
      worker: new Worker('media-reader-service.js'),
      control: new Int32Array(new SharedArrayBuffer(20)),
      payload: new SharedArrayBuffer(3 * 1048576), /* holds one 720x576 BGR24 movie frame */
      port: channel.port2,
    };
    readerService = reader;
    const failReader = event => {
      if (worker !== activeWorker || readerService !== reader) return;
      pendingLog += `Media reader failed: ${event.message || 'message transport error'}\n`;
      finish(`Media reader error: ${event.message || 'message transport error'}`);
    };
    reader.worker.onerror = failReader;
    reader.worker.onmessageerror = failReader;
    reader.worker.postMessage({op: 'attachPort', port: channel.port1}, [channel.port1]);
  } catch (error) { finish(`Media reader setup error: ${error.message}`); return; }
  worker.onmessage = ({data}) => {
    if (worker !== activeWorker) return;
    if (data.type === 'log') {
      pendingLog = (pendingLog + data.text + '\n').slice(-1024 * 1024);
      if (logTimer === null) logTimer = setTimeout(flushLog, 50);
    } else if (data.type === 'thread-failure-watch') {
      try {
        if (threadFailureWatch) throw new Error('Duplicate thread failure descriptor');
        const watch = createThreadFailureWatch(data.descriptor, message => {
          if (worker === activeWorker) { pendingLog += `[thread] ${message}\n`; finish(`Guest thread failed: ${message}`); }
        });
        if (worker === activeWorker) threadFailureWatch = watch; else watch.close();
      } catch (error) { finish(`Thread watchdog error: ${error.message}`); }
    } else if (data.type === 'persist') { userdataStore().then(store => store.apply(data.change)); }
    else if (data.type === 'graphics-trace') { globalThis.__graphicsTrace = data.entries; pendingLog += `[graphics] frame trace captured: ${data.entries.length} command(s)\n`; }
    else if (data.type === 'dsound') { audioSink?.attachDirectSound(data.memory, data.control, data.slots); pendingLog += `[audio] DirectSound buffers attached to the AudioWorklet (${data.slots} slots)\n`; }
    else if (data.type === 'status') status.textContent = data.text;
    else if (data.type === 'dialog') {
      try { showDialog(data); }
      catch (error) { finish(`Dialog bridge error: ${error.message}`); }
    }
    else if (data.type === 'window') {
      const response = new Int32Array(data.response);
      let result = -1;
      try {
        result = surface.apply(data.event);
        publishPointer();
        const event = data.event;
        const canvas = surface.windows.get(event.hwnd);
        if (event.type === 'create') {
          pointerLock.attach(canvas, event.hwnd);
          directInput.attach(canvas, event.hwnd);
          captureMouse.disabled = false; fullscreenButton.disabled = false;
        }
        if (event.type === 'show') {
          if (event.visible && !event.minimized) canvas.focus();
          else directInput.blur();
        }
        if (event.type === 'destroy') directInput.detach(event.hwnd);
      }
      catch (error) { pendingLog += `Window bridge: ${error.message}\n`; }
      Atomics.store(response, 0, result);
      Atomics.notify(response, 0);
    }
    else if (data.type === 'terminal') finish(playMode && data.code === 0 ? 'The game closed.' : playMode ? `The game stopped (${data.code}). ${data.code === 4 ? 'It reached something this port does not support yet. ' : ''}Open "Technical log" below for details.` : `Diagnostic ended: code ${data.code}. ${data.text || ''}`);
  };
  let userdata = {files: [], directories: []};
  try { userdata = await (await userdataStore()).load(); } catch (error) { pendingLog += `[userdata] saved data unavailable: ${error.message}\n`; }
  const traceAt = typeof URLSearchParams === 'function' && globalThis.location ? Number(new URLSearchParams(location.search).get('traceAt')) || 0 : 0;
  try { worker.postMessage({type: 'start', traceAt, userdata, traceImports: checked('trace-imports'), traceFiles: checked('trace-files'), traceDsound: globalThis.__traceDsound === true || (typeof URLSearchParams === 'function' && globalThis.location ? new URLSearchParams(location.search).get('dsound') === '1' : false),
    optimization: Number(buildValue().split('-')[0]), threads: buildValue().endsWith('-threads'), ...(playMode ? {gameFiles: globalThis.moviesGameFiles} : {}), inputBuffer, cursorBuffer, directInputBuffer, directInputControl,
    pthreadPort: ownedPthreads?.port, audio: audio.descriptor, graphics, reader: {port: reader.port, controlBuffer: reader.control.buffer, payloadBuffer: reader.payload},
    media: {port: media.port, controlBuffer: media.control.buffer, payloadBuffer: media.payload}}, [...userdata.files.map(f => f.bytes), media.port, reader.port, graphics.presentationPort, graphics.commandPort, ...(ownedPthreads ? [ownedPthreads.port] : [])]); }
  catch (error) { finish(`Guest worker start error: ${error.message}`); }
});
stop.addEventListener('click', () => finish('Stopped by user.'));
