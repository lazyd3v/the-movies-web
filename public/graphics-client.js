import {GraphicsMetrics, emitGraphicsMetrics} from './graphics-metrics.js';
import {GRAPHICS_LIMITS, HEADER, validateCommand, responseViews, consumeResponse} from './graphics-protocol.js';
function copyBytes(input, budget) {
  if (!input || !Number.isInteger(input.byteLength) || input.byteLength < 1 || input.byteLength > budget.remaining) throw new RangeError('Graphics upload budget exceeded');
  budget.remaining -= input.byteLength;
  if (input instanceof ArrayBuffer) return input.slice(0);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength).slice().buffer;
  throw new TypeError('Expected upload bytes');
}
function prepare(command, args, budget = {remaining: GRAPHICS_LIMITS.maxUploadBytes}, inBatch = false) {
  if (!args || typeof args !== 'object' || Array.isArray(args)) throw new TypeError('Invalid command args');
  if (command === 'batch' && (inBatch || !Array.isArray(args.commands) || !args.commands.length || args.commands.length > GRAPHICS_LIMITS.maxBatchCommands)) throw new RangeError('Invalid graphics batch');
  const copy = {...args};
  if (command.startsWith('upload')) copy.bytes = copyBytes(copy.bytes, budget);
  if (command === 'batch') copy.commands = args.commands.map(entry => ({command: entry.command, args: prepare(entry.command, entry.args ?? {}, budget, true)}));
  return copy;
}
function transfers(command, args) { return command.startsWith('upload') ? [args.bytes] : command === 'batch' ? args.commands.flatMap(e => transfers(e.command, e.args)) : []; }
export class MoviesGraphicsClient {
  #worker; #views; #serial = 0; #closed = false; #busy = false; #timeout; #metrics = null; #onMetrics = null;
  constructor({width = 256, height = 256, presentationPort, presentationAck, timeoutMs = 15000,
    responseBytes = Math.max(1024 * 1024, width * height * 4), workerURL = new URL('./graphics-worker.js', import.meta.url), commandPort = null, responseBuffer = null, profile = false, onMetrics = null} = {}) {
    if (typeof document !== 'undefined') throw new Error('Synchronous graphics client must run in a guest worker');
    if (typeof WorkerGlobalScope !== 'undefined' && !commandPort) throw new Error('Browser graphics requires page-prepared commandPort before blocking');
    if (commandPort && (!responseBuffer || typeof commandPort.postMessage !== 'function')) throw new TypeError('Prepared graphics transport requires commandPort and responseBuffer');
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > GRAPHICS_LIMITS.maxTimeoutMs) throw new RangeError('Invalid graphics timeout');
    for (const n of [width, height]) if (!Number.isInteger(n) || n < 1 || n > GRAPHICS_LIMITS.maxDimension) throw new RangeError('Invalid target dimensions');
    if (!Number.isInteger(responseBytes) || responseBytes < Math.max(1024, width * height * 4) || responseBytes > GRAPHICS_LIMITS.maxResponseBytes) throw new RangeError('Invalid response budget');
    if (!(presentationAck instanceof SharedArrayBuffer) || presentationAck.byteLength !== 8 || !presentationPort?.postMessage) throw new TypeError('Dedicated presentation port and8-byte SAB required');
    if (typeof profile !== 'boolean' || (onMetrics !== null && typeof onMetrics !== 'function')) throw new TypeError('Invalid graphics profiling options');
    this.#onMetrics = onMetrics;
    if (profile) this.#metrics = new GraphicsMetrics(['frameMs', 'bridgeMs', 'uploadBytes', ...['setRenderTargetFormat','clear','uploadVertices','uploadIndices','uploadTextureRGBA','drawIndexed','release','readPixels','present','validate','batch','dispose'].map(n => n + 'Ms')]);
    this.#timeout = timeoutMs;
    const response = responseBuffer ?? new SharedArrayBuffer(GRAPHICS_LIMITS.headerBytes + responseBytes); this.#views = responseViews(response);
    if (this.#views.payload.length < width * height * 4) throw new RangeError('Response cannot hold target pixels');
    this.#worker = commandPort ?? new Worker(workerURL, {type: 'module', name: 'movies-gpu'});
    this.#worker.start?.();
    try { this.capabilities = this.#exchange('init', {width, height, timeoutMs, response, presentationAck, presentationPort, profile}, [presentationPort]); }
    catch (error) { this.close(); throw error; }
  }
  #exchange(command, args, transfer = []) {
    if (this.#closed || this.#busy) throw new Error('Graphics client closed or reentrant');
    if (++this.#serial > 0x7fffffff) { this.close(); throw new Error('Graphics serial exhausted'); }
    this.#busy = true;
    try {
      if (Atomics.load(this.#views.header, HEADER.transportFailure)) throw new Error('Graphics host reported a worker startup/runtime failure');
      Atomics.store(this.#views.header, HEADER.serial, this.#serial);
      Atomics.store(this.#views.header, HEADER.status, 0);
      this.#worker.postMessage({serial: this.#serial, command, args}, transfer);
      const deadline = performance.now() + this.#timeout;
      while (Atomics.load(this.#views.header, HEADER.status) === 0) {
        if (Atomics.load(this.#views.header, HEADER.transportFailure)) throw new Error('Graphics host reported a worker failure');
        const remaining = deadline - performance.now();
        if (remaining <= 0) { this.close(); throw new Error(`Graphics worker timeout during ${command}`); }
        Atomics.wait(this.#views.header, HEADER.status, 0, remaining);
      }
      return consumeResponse(this.#views, this.#serial);
    } catch (error) { this.close(); throw error; }
    finally { this.#busy = false; }
  }
  call(command, args = {}) {
    const began = this.#metrics ? performance.now() : 0;
    const owned = prepare(command, args); validateCommand(command, owned);
    const bytes = this.#metrics ? (command.startsWith('upload') ? owned.bytes.byteLength : command === 'batch' ? owned.commands.reduce((n,e)=>n+(e.command.startsWith('upload')?e.args.bytes.byteLength:0),0) : 0) : 0;
    const result = this.#exchange(command, owned, transfers(command, owned));
    if (this.#metrics) {
      const now = performance.now(), elapsed = now - began;
      this.#metrics.add('bridgeMs', elapsed); this.#metrics.add(command + 'Ms', elapsed);
      if (bytes) this.#metrics.add('uploadBytes', bytes);
      if (command === 'present') {
        const report = this.#metrics.frame(result.serial, now);
        if (report) emitGraphicsMetrics(this.#onMetrics, {source:'graphics-client', ...report, worker:result.metrics ?? null});
      }
    }
    return result;
  }
  uploadVertices(bytes, format = 'XYZ_DIFFUSE') { return this.call('uploadVertices', {bytes, format}); }
  uploadIndices(bytes, format = 'uint16') { return this.call('uploadIndices', {bytes, format}); }
  uploadTextureRGBA(width, height, bytes) { return this.call('uploadTextureRGBA', {width, height, bytes}); }
  setRenderTargetFormat(args) { return this.call('setRenderTargetFormat', args); }
  clear(args = {}) { return this.call('clear', args); }
  drawIndexed(args) { return this.call('drawIndexed', args); }
  readPixels() { return this.call('readPixels'); }
  present() { return this.call('present'); }
  validate() { return this.call('validate'); }
  release(handle) { return this.call('release', {handle}); }
  batch(commands) { return this.call('batch', {commands}); }
  dispose() { if (!this.#closed) { try { this.call('dispose'); } finally { this.close(); } } }
  close() { if (!this.#closed) { this.#closed = true; this.#worker?.terminate?.(); this.#worker?.close?.(); } }
}
