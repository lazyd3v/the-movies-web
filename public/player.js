/* Integration hooks; the upstream Boxedwine runtime stays unmodified. */
(async function () {
  const params = new URLSearchParams(location.search);
  const log = (text) => Module.print('[The Movies] ' + text);
  // Wine tracing can produce thousands of lines per second. Avoid upstream's
  // textarea append + forced layout and a postMessage for every single line.
  const output = document.getElementById('output');
  const LOG_LIMIT = 1024 * 1024; // At most 2 MiB of UTF-16 text per buffer.
  let pending = [], pendingStart = 0, pendingSize = 0, flushTimer = null;
  function flushLogs() {
    if (flushTimer !== null) { clearTimeout(flushTimer); flushTimer = null; }
    if (!pendingSize) return;
    const text = pending.slice(pendingStart).join('\n');
    pending = []; pendingStart = 0; pendingSize = 0;
    output.value = (output.value + text + '\n').slice(-LOG_LIMIT);
    output.scrollTop = output.scrollHeight;
    parent.postMessage({ type: 'movies-log', text }, location.origin);
  }
  function queueLog(text) {
    text = text.slice(-(LOG_LIMIT - 1));
    pending.push(text); pendingSize += text.length + 1;
    while (pendingSize > LOG_LIMIT) pendingSize -= pending[pendingStart++].length + 1;
    if (pendingStart > 1024) { pending = pending.slice(pendingStart); pendingStart = 0; }
    if (flushTimer === null) flushTimer = setTimeout(flushLogs, 100);
  }
  let manifest;
  try {
    const response = await fetch('game/manifest.json');
    if (!response.ok) throw new Error('Run npm run prepare:game first.');
    manifest = await response.json();
    const mediaResponse = await fetch('media/manifest.json');
    const media = mediaResponse.ok ? await mediaResponse.json() : null;
    // Mount the archive using bounded HTTP range reads rather than copying 2.5 GB.
    buildAppFileSystem = function (callback) {
      try {
        // Build 121 dereferences a null FILE* when Linux /proc/stat is absent.
        // This is virtual idle CPU accounting, not measurement of the host CPU.
        MoviesProcStat.install(FS);
        globalThis.moviesArchive = MoviesRangeFile.mount(FS, {
          path: '/' + Config.appZipFile,
          url: new URL('game/movies.zip', location.href).href,
          size: manifest.archiveBytes
        });
        log('Original game mounted (' + manifest.fileCount + ' files); 32 MiB read cache.');
        callback();
      } catch (error) { fail(error); }
    };
    const originalBuildBrowserFileSystem = buildBrowserFileSystem;
    buildBrowserFileSystem = function () {
      originalBuildBrowserFileSystem();
      document.getElementById('downloadbtn').style.display = '';
    };
    Module.print = function (...args) {
      const text = args.join(' ');
      queueLog(text);
      if (text.includes('first JIT block executed')) {
        flushLogs(); // Deliver preceding milestones before this immediate status.
        parent.postMessage({ type: 'movies-status', text: 'Emulator running · application startup in progress' }, location.origin);
      }
    };
    Module.printErr = function (...args) {
      const text = args.join(' ');
      queueLog(text);
      if (text.includes('Unhandled promise rejection')) fail(new Error(text));
    };
    const oldOnExit = Module.onExit;
    Module.onExit = function (code) { flushLogs(); if (oldOnExit) oldOnExit(code); };
    window.addEventListener('pagehide', flushLogs);
    loadFile = function (prefix, name, callback) {
      fetch(prefix + name).then(response => {
        if (!response.ok) throw new Error(name + ': HTTP ' + response.status);
        return response.arrayBuffer();
      }).then(buffer => callback(new Uint8Array(buffer))).catch(fail);
    };
    Module.onAbort = (reason) => fail(new Error(String(reason)));
    window.addEventListener('unhandledrejection', event => fail(event.reason instanceof Error ? event.reason : new Error(String(event.reason))));
    window.addEventListener('error', event => fail(event.error || new Error(event.message)));
    document.getElementById('output').style.display = 'block';
    // Use upstream URL parsing, including literal comma-separated preset syntax.
    Config.urlParams = 'root=TinyCore15Wine11.0-web.zip&rootBase=runtime/&app=movies.zip' +
      '&p=' + (params.get('diagnostic') === 'notepad' ? 'notepad.exe' : media ? 'c:/media-setup.bat' : 'launch.bat') +
      '&registry=winxp,mousewarp-disable&resolution=800x600&safe-memory=' + (params.get('safe') === '1' ? 'true' : 'false') + '&sound=true' +
      '&storage=' + (params.get('fresh') === '1' ? 'memory' : 'indexeddb') + '&storageKey=the-movies-v1&w=c:/files';
    if (media && params.get('diagnostic') !== 'notepad') {
      Config.urlParams += '&overlay=media.zip&overlayBase=media/';
      getEnvProp = () => 'WINEDLLOVERRIDES=' + media.dllOverrides;
    }
    if (params.get('fresh') === '1') Config.urlParams += '&rootfs=memory';
    if (['media', 'graphics'].includes(params.get('trace'))) {
      const originalParams = getEmulatorParams;
      getEmulatorParams = function () {
        const args = originalParams();
        // The media bootstrap enables verbose channels immediately before the
        // game, after expensive COM registration and Wine service startup.
        args.splice(args.indexOf('/bin/wine'), 0, '-env', 'WINEDEBUG=-all', '-env', 'MOVIES_TRACE=' + (params.get('trace') === 'graphics' ? 'graphics' : '1'));
        return args;
      };
    }
    const script = document.createElement('script');
    const runtimePaths = { 'chain128': 'runtime/chain128/boxedwine.js', 'read-ahead': 'runtime/read-ahead/boxedwine.js', 'mt-jit': 'runtime/boxedwine.js', 'interpreter': 'runtime/interpreter/boxedwine.js', 'single-jit': 'runtime/single-jit/boxedwine.js' };
    script.src = runtimePaths[params.get('engine')] || runtimePaths['mt-jit'];
    script.onerror = () => fail(new Error('Runtime missing. Run python3 scripts/setup-runtime.py.'));
    document.body.append(script);
  } catch (error) { fail(error); }
  function fail(error) {
    const text = 'Launch failed: ' + error.message;
    document.getElementById('status').textContent = text;
    queueLog(text);
    flushLogs();
    parent.postMessage({ type: 'movies-error', text: text.split('\n')[0].slice(0, 250) }, location.origin);
  }
})();
