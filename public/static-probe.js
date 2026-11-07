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
optimization.addEventListener('change', () => {
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
let directInput = null;
let pendingLog = '';
let logTimer = null;
const dialog = document.getElementById('game-dialog');
let answerDialog = null;
const surface = new MoviesWindowSurface(document.getElementById('game-surface'), document, event => {
  if (input && !input.push(event)) finish('Game input queue overflowed.');
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
  log.textContent = (log.textContent + pendingLog).slice(-1024 * 1024);
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
setInterval(() => {
  document.getElementById('heartbeat').textContent = `UI heartbeat: ${Math.floor((performance.now() - began) / 1000)} s`;
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
  captureMouse.disabled = true;
  disposeMediaService();
  graphicsPresenter?.close(); graphicsPresenter = null;
  graphicsPort?.close(); graphicsPort = null;
  graphicsTransport?.close(); graphicsTransport = null;
  ownedPthreads?.close(); ownedPthreads = null;
  worker?.terminate(); worker = null;
  const closingAudio = audioSink; audioSink = null;
  closingAudio?.close().catch(() => {});
  input = null;
  directInput?.dispose(); directInput = null;
  cursorSnapshot?.write(null); cursorSnapshot = null;
  start.disabled = false; stop.disabled = true;
  optimization.disabled = false;
  updateFrameMetrics();
}
start.addEventListener('click', async () => {
  surface.clear();
  frameMetrics = {count: 0, sampleCount: 0, sampleAt: 0, rate: null, firstAt: 0, lastAt: 0, reported: false};
  updateFrameMetrics();
  if (logTimer !== null) clearTimeout(logTimer);
  logTimer = null; pendingLog = '';
  log.textContent = ''; start.disabled = true; stop.disabled = false;
  status.textContent = 'Starting worker…';
  optimization.disabled = true;
  worker = new Worker('static-probe-worker.js');
  const cursorBuffer = MoviesPointerSnapshot.create();
  cursorSnapshot = new MoviesPointerSnapshot(cursorBuffer);
  const inputBuffer = MoviesWindowInput.create();
  input = new MoviesWindowInput(inputBuffer);
  const directInputBuffer = MoviesWindowInput.create(1024, MoviesDirectInputFields);
  const directInputControl = new SharedArrayBuffer(MoviesAsyncKeyControlBytes);
  directInput = new MoviesDirectInputHost(directInputBuffer, directInputControl, text => finish(text));
  const activeWorker = worker;
  const profileGraphics = document.getElementById('profile-graphics').checked;
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
        directInput?.blur();
        losePointer();
        if (!document.hidden) captureMouse.focus?.();
      }
      mouseStatus.textContent = message || (locked ? 'Mouse captured — press Escape to release.' : 'Click Capture mouse to control the game; Escape releases it.');
    }});
    const transport = await prepareGraphicsTransport({width:1024, height:768});
    if (worker !== activeWorker) { transport.close(); return; }
    graphicsTransport = transport;
    const channel = new MessageChannel();
    const ack = new Int32Array(new SharedArrayBuffer(8));
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
        if (data.type !== 'graphics-frame') throw new Error('Unknown GPU presentation message');
        graphicsPresenter.submit(data);
      } catch (error) { finish(`GPU presentation error: ${error.message}`); }
    };
    graphics = {profile: profileGraphics, width:1024, height:768, presentationPort:channel.port2, presentationAck:ack.buffer,
      commandPort:transport.commandPort, responseBuffer:transport.responseBuffer};
  } catch (error) { if (worker === activeWorker) finish(`GPU setup error: ${error.message}`); return; }
  if (optimization.value.endsWith('-threads')) {
    try {
      const {createOwnedWorkerHost} = await import('./owned-worker-host.js');
      if (worker !== activeWorker) return;
      const level = Number(optimization.value.split('-')[0]);
      ownedPthreads = createOwnedWorkerHost({
        allowedURL: new URL(`runtime/static-probe${level ? '-o' + level : ''}-threads/probe.js`, location.href).href,
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
      payload: new SharedArrayBuffer(1048576),
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
    } else if (data.type === 'status') status.textContent = data.text;
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
          captureMouse.disabled = false;
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
    else if (data.type === 'terminal') finish(`Diagnostic ended: code ${data.code}. ${data.text || ''}`);
  };
  try { worker.postMessage({type: 'start', traceImports: document.getElementById('trace-imports').checked, traceFiles: document.getElementById('trace-files').checked,
    optimization: Number(optimization.value.split('-')[0]), threads: optimization.value.endsWith('-threads'), inputBuffer, cursorBuffer, directInputBuffer, directInputControl,
    pthreadPort: ownedPthreads?.port, audio: audio.descriptor, graphics, reader: {port: reader.port, controlBuffer: reader.control.buffer, payloadBuffer: reader.payload},
    media: {port: media.port, controlBuffer: media.control.buffer, payloadBuffer: media.payload}}, [media.port, reader.port, graphics.presentationPort, graphics.commandPort, ...(ownedPthreads ? [ownedPthreads.port] : [])]); }
  catch (error) { finish(`Guest worker start error: ${error.message}`); }
});
stop.addEventListener('click', () => finish('Stopped by user.'));
