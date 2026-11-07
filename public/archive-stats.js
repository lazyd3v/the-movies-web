/* Archive activity and browser scheduling diagnostics, never game FPS. */
(function () {
  const element = document.getElementById('archive-stats');
  if (!element) return;
  const mib = bytes => (bytes / (1024 * 1024)).toFixed(1);
  const gpuInfo = (function () {
    let gl;
    try {
      gl = document.createElement('canvas').getContext('webgl2');
      if (!gl) return 'WebGL 2 unavailable';
      const debug = gl.getExtension('WEBGL_debug_renderer_info');
      const vendor = gl.getParameter(debug ? debug.UNMASKED_VENDOR_WEBGL : gl.VENDOR);
      const renderer = gl.getParameter(debug ? debug.UNMASKED_RENDERER_WEBGL : gl.RENDERER);
      return `${vendor} · ${renderer}`;
    } catch (error) {
      return 'unavailable: ' + String(error.message || error).slice(0, 100);
    } finally {
      // This diagnostic context must not retain another GPU allocation.
      try { gl?.getExtension('WEBGL_lose_context')?.loseContext(); } catch {}
    }
  })();
  const began = performance.now();
  let sampleStarted = began, frameCount = 0, lastFrame = null, refreshHz = null;
  let presentationCount = 0, totalPresents = 0, presentsPerSecond = null;
  const presentationHooks = [];
  // player.html loads the upstream shell before this script. Its Module hooks
  // therefore exist before player.js finishes fetching and starts the runtime.
  for (const name of ['boxedwinePresentFrame', 'boxedwinePresentSoftwareFrame']) {
    const runtime = globalThis.Module;
    const original = runtime?.[name];
    if (typeof original !== 'function') continue;
    const wrapped = function () {
      presentationCount++;
      totalPresents++;
      return original.apply(this, arguments);
    };
    runtime[name] = wrapped;
    presentationHooks.push({ runtime, name, original, wrapped });
  }
  let active = true, frameRequest;
  let jitSummary = 'JIT metrics: waiting for runtime', jitPolling = false;
  async function updateJit() {
    if (!active || jitPolling || !globalThis.moviesArchive) return;
    const runtime = globalThis.Module;
    if (typeof runtime?.getWasmJitMtRuntimeBatchStats !== 'function') {
      jitSummary = 'JIT metrics: unavailable in this execution mode';
      return;
    }
    jitPolling = true;
    try {
      const batch = runtime.getWasmJitMtRuntimeBatchStats();
      const summary = {
        translated: batch.translatedFileBacked + batch.translatedAnonymous,
        pending: batch.pendingEntries,
        failures: batch.permanentFailures,
        oomBlocks: batch.groupedOomBlocks,
        groupCompiles: batch.groupedLocalCompiles,
        groupHits: batch.groupedBrokerHits
      };
      jitSummary = 'JIT ' + JSON.stringify(summary);
      if (typeof runtime.getWasmJitModuleBrokerStats === 'function') {
        // Upstream collects worker replies asynchronously. Never overlap polls
        // or serialize its potentially large per-owner arrays into the DOM.
        try {
          const broker = await runtime.getWasmJitModuleBrokerStats();
          summary.localCompiles = broker.localCompiles;
          summary.brokerHits = broker.brokerHits;
          summary.workers = `${broker.workerStatsReplied}/${broker.workerStatsExpected}`;
          summary.missingReplies = broker.missingWorkerReplies;
        } catch (error) {
          summary.brokerError = String(error.message || error).slice(0, 100);
        }
      }
      if (active) jitSummary = 'JIT ' + JSON.stringify(summary);
    } catch (error) {
      if (active) jitSummary = 'JIT metrics unavailable: ' + String(error.message || error).slice(0, 120);
    } finally {
      jitPolling = false;
    }
  }
  function heartbeat() {
    if (!active) return;
    lastFrame = performance.now();
    frameCount++;
    frameRequest = requestAnimationFrame(heartbeat);
  }
  function update() {
    const now = performance.now();
    const sampleElapsed = now - sampleStarted;
    if (sampleElapsed >= 500) {
      refreshHz = frameCount * 1000 / sampleElapsed;
      presentsPerSecond = presentationCount * 1000 / sampleElapsed;
      frameCount = 0;
      presentationCount = 0;
      sampleStarted = now;
    }
    const stats = globalThis.moviesArchive?.stats;
    const archive = stats
      ? `Game archive: ${stats.requests.toLocaleString()} requests · ${mib(stats.bytes)} MiB read · ${stats.hits.toLocaleString()} cache hits · ${mib(stats.cacheBytes)} MiB cached`
      : 'Game archive: waiting for mount';
    const refresh = refreshHz === null ? 'measuring' : `${refreshHz.toFixed(1)} Hz`;
    const presents = !presentationHooks.length ? 'unavailable' : presentsPerSecond === null ? 'measuring' : `${presentsPerSecond.toFixed(1)} /s`;
    const age = lastFrame === null ? 'waiting for first frame' : `last frame ${(now - lastFrame).toFixed(0)} ms ago`;
    const heapBytes = globalThis.Module?.HEAPU8?.buffer.byteLength;
    const heap = heapBytes === undefined ? 'pending' : `${mib(heapBytes)} MiB`;
    const text = `${archive}\nGame presents ${presents} (presentation calls; ${totalPresents.toLocaleString()} total) · Browser refresh ${refresh} (not game FPS) · ${document.visibilityState} · ${age} · elapsed ${((now - began) / 1000).toFixed(1)} s\n${jitSummary} · Wasm heap ${heap}\nBrowser GPU: ${gpuInfo}`;
    if (element.textContent !== text) element.textContent = text;
  }
  element.style.whiteSpace = 'pre-line';
  frameRequest = requestAnimationFrame(heartbeat);
  update();
  const timer = setInterval(update, 1000);
  const jitTimer = setInterval(updateJit, 5000);
  document.addEventListener('visibilitychange', update);
  window.addEventListener('pagehide', () => {
    active = false;
    clearInterval(timer);
    clearInterval(jitTimer);
    cancelAnimationFrame(frameRequest);
    document.removeEventListener('visibilitychange', update);
    for (const hook of presentationHooks) {
      if (hook.runtime[hook.name] === hook.wrapped) hook.runtime[hook.name] = hook.original;
    }
  }, { once: true });
})();
