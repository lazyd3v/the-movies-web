'use strict';
let started = false;
let finished = false;
let entryStarted = null;
let archive = null;
let mediaClient = null;
let readerClient = null;
let audioClient = null;
let ownedPthreads = null;
let graphicsHost = null;
const gameWindows = new Map();
let runtimeLog = '', runtimeLogFlushed = Date.now();
function flushRuntimeLog() {
  runtimeLogFlushed = Date.now();
  if (!runtimeLog) return;
  postMessage({type:'log', text:runtimeLog.trimEnd()});
  runtimeLog = '';
}
function appendRuntimeLog(text) {
  runtimeLog += String(text) + '\n';
  // Guest execution is synchronous: timer-based batching cannot run here.
  // Bound each packet and avoid one browser task for every hot heap API call.
  // Sparse native profiling windows must remain observable during long loops.
  // Also flush at least once per second so sparse diagnostics are observable.
  if (runtimeLog.length >= 32768 || Date.now() - runtimeLogFlushed >= 1000 || String(text).startsWith('[wait-profile] ') || String(text).startsWith('[D3D9 present]')) flushRuntimeLog();
}
let dialogWaitMs = 0;
function updateGameWindow(event) {
  flushRuntimeLog();
  const response = new Int32Array(new SharedArrayBuffer(4));
  postMessage({type: 'window', event, response: response.buffer});
  while (Atomics.load(response, 0) === 0) Atomics.wait(response, 0, 0);
  const accepted = Atomics.load(response, 0) === 1;
  if (accepted) {
    if (event.type === 'destroy') gameWindows.delete(event.hwnd);
    else gameWindows.set(event.hwnd, {...event});
  }
  if (accepted) postMessage({type: 'log', text: `[window] ${event.type} hwnd=${event.hwnd.toString(16)} client=${event.clientWidth}x${event.clientHeight} visible=${event.visible} cursor=${event.cursor}`});
  return accepted ? 1 : 0;
}
function showGameDialog(title, text, flags) {
  flushRuntimeLog();
  const response = new Int32Array(new SharedArrayBuffer(4));
  const began = performance.now();
  postMessage({type: 'log', text: `[dialog] ${title}: ${text}`});
  postMessage({type: 'dialog', title, text, flags, response: response.buffer});
  while (Atomics.load(response, 0) === 0) Atomics.wait(response, 0, 0);
  dialogWaitMs += performance.now() - began;
  postMessage({type: 'status', text: 'Continuing original game code…'});
  return Atomics.load(response, 0);
}
function terminal(code, text = '') {
  if (finished) return;
  finished = true;
  flushRuntimeLog();
  if (entryStarted !== null) {
    postMessage({type: 'log', text: `Bootstrap execution: ${(performance.now() - entryStarted - dialogWaitMs).toFixed(1)} ms to diagnostic stop, including image loading and function registration, excluding dialog interaction (not a gameplay benchmark).`});
  }
  if (archive) {
    const {requests, bytes, hits, cacheBytes} = archive.stats;
    postMessage({type: 'log', text: `Asset reads: ${requests} range requests, ${bytes} bytes fetched, ${hits} cache hits, ${cacheBytes} bytes cached.`});
  }
  ownedPthreads?.close(); ownedPthreads = null;
  audioClient?.dispose(); audioClient = null;
  readerClient?.close(); readerClient = null;
  mediaClient?.close(); mediaClient = null;
  graphicsHost?.close(); graphicsHost = null;
  postMessage({type: 'terminal', code, text});
}
self.onmessage = async ({data}) => {
  if (data.type !== 'start' || started) return;
  started = true;
  try {
    const optimization = Number(data.optimization ?? 0);
    if (![0, 1, 2].includes(optimization)) throw new Error('Invalid optimization profile');
    if (data.threads !== undefined && typeof data.threads !== 'boolean') throw new Error('Invalid threading profile');
    const threaded = data.threads === true;
    if (threaded) {
      const {installOwnedWorkerClient} = await import('./owned-worker-client.js');
      if (!data.pthreadPort) throw new Error('Page-owned pthread transport missing');
      ownedPthreads = installOwnedWorkerClient(data.pthreadPort);
    }
    const base = new URL(`runtime/static-probe${optimization ? '-o' + optimization : ''}${threaded ? '-threads' : ''}/`, self.location.href);
    postMessage({type: 'log', text: `[diagnostic-options] assetReads=${data.traceFiles === true} imports=${Boolean(data.traceImports)} profiling=${data.graphics?.profile === true}`});
    postMessage({type: 'status', text: 'Fetching build provenance and original executable…'});
    const manifestResponse = await fetch(new URL('manifest.json', base));
    if (!manifestResponse.ok) throw new Error(`Manifest HTTP ${manifestResponse.status}`);
    const manifest = await manifestResponse.json();
    if (Boolean(manifest.threaded) !== threaded) throw new Error('Manifest threading profile mismatch');
    postMessage({type: 'log', text: `Build optimization: O${optimization}; Wasm SHA-256: ${manifest.artifacts['probe.wasm'].sha256}`});
    // Game files come from the player's own folder (public site) or, locally, from
    // the indexed archive under public/game. The public site never hosts them.
    let fileIndex, archiveRequest, bytes;
    if (data.gameFiles !== undefined) {
      if (!Array.isArray(data.gameFiles)) throw new Error('Invalid game folder');
      const exe = data.gameFiles.find(entry => entry?.path === 'MoviesSE.exe');
      if (!exe) throw new Error('The chosen folder has no MoviesSE.exe');
      bytes = new Uint8Array(await exe.file.arrayBuffer());
      importScripts(new URL('local-game-files.js', self.location.href).href);
      ({index: fileIndex, request: archiveRequest} = MoviesLocalGameFiles.createIndex(data.gameFiles, manifest.gameExeSha256));
    } else {
      const indexResponse = await fetch(new URL('game/file-index.json', self.location.href));
      if (!indexResponse.ok) throw new Error(`Game file index HTTP ${indexResponse.status}; run scripts/index-game-archive.py`);
      fileIndex = await indexResponse.json();
      if (fileIndex.executable?.sha256 !== manifest.gameExeSha256) throw new Error('Archive executable differs from compiled game');
      const response = await fetch(new URL('MoviesSE.exe', base));
      if (!response.ok) throw new Error(`Executable HTTP ${response.status}`);
      bytes = new Uint8Array(await response.arrayBuffer());
    }
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');
    if (hash !== manifest.gameExeSha256 || hash !== '48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a') throw new Error('Executable checksum mismatch');
    postMessage({type: 'status', text: 'Compiling and instantiating translated original code…'});
    importScripts(new URL('probe.js', base).href);
    importScripts(new URL('window-input.js', self.location.href).href);
    importScripts(new URL('directinput-host.js', self.location.href).href);
    const input = new MoviesWindowInput(data.inputBuffer);
    const cursor = new MoviesPointerSnapshot(data.cursorBuffer);
    const directInput = new MoviesWindowInput(data.directInputBuffer, MoviesDirectInputFields);
    const directInputControl = new Int32Array(data.directInputControl);
    importScripts(new URL('range-file.js', self.location.href).href, new URL('stored-archive-fs.js', self.location.href).href);
    importScripts(new URL('media-service-client.js', self.location.href).href);
    if (!data.media?.port) throw new Error('Page-owned media service transport missing');
    postMessage({type: 'status', text: 'Verifying and prewarming the real media encoder service…'});
    mediaClient = await MoviesMediaServiceClient.connect({
      ...data.media, assetBase: new URL('runtime/media-probe/', self.location.href).href,
    });
    const mediaProvider = mediaClient.provider();
    postMessage({type: 'log', text: '[media] Real WMV2/WMAv2 ASF service ready; bounded 1-second encode jobs, no WMV3 encoder.'});
    importScripts(new URL('media-reader-client.js', self.location.href).href);
    if (!data.reader?.port) throw new Error('Page-owned media reader transport missing');
    postMessage({type: 'status', text: 'Verifying and prewarming the real ASF audio reader…'});
    readerClient = await MoviesMediaReaderClient.connect({
      ...data.reader, assetBase: new URL('runtime/media-probe/', self.location.href).href,
    });
    const readerProvider = readerClient.readerProvider();
    if (!readerProvider.moviesMediaReaderAvailable()) throw new Error('Actual media reader backend unavailable');
    postMessage({type: 'log', text: '[media-reader] Real WMAv2 stereo 44100 Hz PCM reader ready; bounded input and sample queues.'});
    const {MoviesGraphicsClient} = await import('./graphics-client.js');
    const {createMoviesD3D9Host} = await import('./d3d9-host.js');
    if (!data.graphics?.commandPort) throw new Error('Page-owned GPU transport missing');
    postMessage({type: 'status', text: 'Initializing the real WebGPU device…'});
    const graphicsClient = new MoviesGraphicsClient({...data.graphics, onMetrics(metrics) {
      postMessage({type: 'log', text: `[graphics-profile] ${JSON.stringify(metrics)}`});
    }});
    if (Number.isInteger(data.traceAt) && data.traceAt > 0) globalThis.moviesTrace = {startAt: data.traceAt, remaining: 1, entries: [], onDone: entries => postMessage({type: 'graphics-trace', entries})};
    try { graphicsHost = createMoviesD3D9Host({client:graphicsClient, getWindow:hwnd => gameWindows.get(hwnd), presentationIntervalMask:1, deferred:true, pipelined:true}); }
    catch (error) { graphicsClient.close(); throw error; }
    postMessage({type: 'log', text: '[graphics] WebGPU 1024×768 ready; bounded D3D9 subset, animation-frame-paced presentation.'});
    const {createMoviesAudioClient} = await import('./audio-client.js');
    if (!data.audio) throw new Error('Page-prewarmed audio descriptor missing');
    audioClient = createMoviesAudioClient(data.audio);
    if (!audioClient.moviesAudioAvailable()) throw new Error('Audio renderer unavailable');
    const {mirrorUserdata, restoreUserdata} = await import('./userdata-store.js');
    const runtime = await createMoviesStaticProbe({
      ...audioClient,
      ...mediaProvider,
      ...readerProvider,
      moviesGraphicsCall: graphicsHost.moviesGraphicsCall,
      moviesProfileWaits: data.graphics?.profile === true,
      noInitialRun: true,
      // Pthread workers must load the generated factory, not this launcher.
      mainScriptUrlOrBlob: new URL('probe.js', base).href,
      moviesThreadFailureWatch: descriptor => {
        if (finished) return;
        postMessage({type:'thread-failure-watch', descriptor});
      },
      moviesShowDialog: showGameDialog,
      moviesWindowEvent: updateGameWindow,
      moviesPollWindowEvents: maxCount => input.drain(maxCount),
      moviesDirectInputAvailable: () => Atomics.load(directInputControl, 0) === 1,
      moviesDirectInputFocused: hwnd => Atomics.load(directInputControl, 1) === hwnd,
      moviesSetCursorPosition: (x, y) => cursor.warp(x, y),
      moviesCursorPosition: () => { const point=cursor.readBlocking(); if(!point)throw new Error('GetCursorPos has no genuine page pointer snapshot; move the mouse over the page.'); return point; },
      moviesAsyncKeyState: vk => MoviesReadAsyncKeyState(directInputControl, vk),
      moviesForegroundWindow: () => Atomics.load(directInputControl, 0) === 1 ? Atomics.load(directInputControl, 1) >>> 0 : 0,
      moviesPollDirectInputEvents: maxCount => directInput.drain(maxCount),
      locateFile: path => new URL(path, base).href,
      print: appendRuntimeLog,
      printErr: appendRuntimeLog,
      preRun: [module => {
        const restored = restoreUserdata(module.FS, data.userdata ?? {});
        postMessage({type: 'log', text: `[userdata] restored ${restored} saved file(s) into /userdata`});
        module.ENV.MOVIES_STARTUP_APIS = '1';
        if (data.traceFiles === true) module.ENV.PORT_LOG_FILES = '1';
        else delete module.ENV.PORT_LOG_FILES;
        if (data.traceDsound === true) module.ENV.PORT_LOG_DSOUND = '1';
        if (data.traceImports) module.ENV.PORT_LOG_IMPORTS = '1';
        else delete module.ENV.PORT_LOG_IMPORTS;
      }],
      onExit: code => terminal(code, code === 4 ? 'Stopped at an unsupported operation; see log.' : 'This result does not establish gameplay.'),
      onAbort: reason => terminal('abort', String(reason)),
    });
    mirrorUserdata(runtime.FS, change => postMessage({type: 'persist', change}, change.bytes ? [change.bytes.buffer] : []));
    archive = await MoviesStoredArchiveFS.mount(runtime.FS, {
      index: fileIndex, url: new URL('game/' + fileIndex.archive, self.location.href).href,
      path: '/game', preloaded: new Map([['MoviesSE.exe', bytes]]), ...(archiveRequest ? {request: archiveRequest} : {}),
    });
    postMessage({type: 'log', text: `Mounted ${fileIndex.fileCount} original files${archiveRequest ? ' from the chosen folder' : ''} with one 32 MiB range cache.`});
    postMessage({type: 'status', text: 'Executing the original entry point…'});
    entryStarted = performance.now();
    const code = runtime.callMain(['/game/MoviesSE.exe']);
    terminal(code ?? 0, 'This result does not establish gameplay.');
  } catch (error) {
    terminal(Number.isInteger(error?.status) ? error.status : 'error', String(error?.message || error));
  }
};
