import {GRAPHICS_LIMITS, VERTEX_FORMATS, validateCommand} from './graphics-protocol.js';

// Construct only after the page-owned GPU worker has initialized. getWindow
// returns acknowledged guest-window geometry, never a guessed display size.
/* deferred: queue commands whose reply the guest does not need and send them as
 * one batch at the next synchronous command (present, readback, dispose) or when
 * the batch limits fill. Resource handles are predicted: the GPU worker assigns
 * them sequentially (graphics-worker.js save()), so the host mirrors its counter
 * and verifies every predicted handle when the batch reply arrives. A failure
 * inside a deferred command surfaces at that flush, i.e. at the next Present. */
const DEFERRABLE = new Set(['clear', 'drawIndexed', 'release', 'setRenderTarget', 'copyRenderTarget', 'writeRenderTarget', 'uploadVertices', 'uploadIndices', 'uploadTextureRGBA', 'createRenderTarget']);
export function createMoviesD3D9Host({client, getWindow, presentationIntervalMask = 0, deferred = false, pipelined = false}) {
  const caps = client?.capabilities;
  if (caps?.backend !== 'WebGPU' || typeof client.call !== 'function' || typeof getWindow !== 'function') throw new TypeError('An initialized GPU client and window lookup are required');
  if (![1, 0x80000000].includes(presentationIntervalMask)) throw new Error('One verified presentation pacing mode is required');
  for (const n of [caps.width, caps.height]) if (!Number.isInteger(n) || n < 1 || n > GRAPHICS_LIMITS.maxDimension) throw new RangeError('Invalid initialized GPU dimensions');
  const maxBufferBytes = Math.min(GRAPHICS_LIMITS.maxUploadBytes, caps.limits?.maxBufferSize ?? 0);
  if (!Number.isSafeInteger(maxBufferBytes) || maxBufferBytes < 1) throw new Error('Missing GPU buffer limit');
  const renderTargetFormats = Object.freeze([...(caps.renderTargetFormats ?? [22])]);
  if (!renderTargetFormats.length || renderTargetFormats.some(f => ![22,25].includes(f))) throw new Error('Unsupported logical backbuffer capabilities');
  const capabilities = Object.freeze({backend: 'WebGPU', width: caps.width, height: caps.height,
    depthFormat: caps.depthFormat, maxBufferBytes, presentationIntervalMask,
    renderTargetFormats,
    maxTextureDimension: Math.min(GRAPHICS_LIMITS.maxDimension, caps.limits?.maxTextureDimension2D ?? 0)});
  let device = null, closed = false;
  const handles = new Map(), targetSizes = new Map();
  let queue = [], queuedBytes = 0, nextHandle = 1;
  function direct(command, args) {
    const result = client.call(command, args);
    if (result && typeof result.then === 'function') throw new Error('Graphics operations must acknowledge synchronously');
    return result;
  }
  function flush() {
    while (queue.length) {
      const batch = queue.splice(0, GRAPHICS_LIMITS.maxBatchCommands); queuedBytes = 0;
      const commands = batch.map(({command, args}) => ({command, args}));
      const verify = results => {
        if (!Array.isArray(results) || results.length !== batch.length) throw new Error('Deferred graphics batch was not acknowledged');
        batch.forEach((entry, i) => { if (entry.expect !== undefined && results[i] !== entry.expect) throw new Error(`Deferred ${entry.command} returned handle ${results[i]}, expected ${entry.expect}`); });
      };
      // Pipelined when the client supports it: the worker executes this batch while
      // the guest produces the next commands; the reply is verified at the next exchange.
      // Every queued upload carries a buffer sliced from guest memory for this call alone.
      if (pipelined && typeof client.post === 'function') client.post('batch', {commands}, verify, {owned: true});
      else verify(direct('batch', {commands}));
    }
  }
  function sync(command, args) { flush(); const result = direct(command, args); if (command.startsWith('upload') || command === 'createRenderTarget') nextHandle = result + 1; return result; }
  function defer(command, args) {
    const bytes = args.bytes?.byteLength ?? 0;
    if (queue.length >= GRAPHICS_LIMITS.maxBatchCommands || queuedBytes + bytes > GRAPHICS_LIMITS.maxUploadBytes) flush();
    const entry = {command, args};
    if (command.startsWith('upload') || command === 'createRenderTarget') entry.expect = nextHandle++;
    queue.push(entry); queuedBytes += bytes;
    if (command === 'setRenderTarget') return args.texture === null ? {width: caps.width, height: caps.height} : targetSizes.get(args.texture);
    return entry.expect ?? null;
  }
  /* Diagnostic one-shot frame trace: set globalThis.moviesTrace = {remaining: n, entries: []}
   * in the game worker to record every bridge command of the next n frames. */
  function trace(command, args, result) {
    const t = globalThis.moviesTrace; if (!t || !(t.remaining > 0)) return;
    if (command === 'present') t.presents = (t.presents ?? 0) + 1;
    if ((t.presents ?? 0) < (t.startAt ?? 0) || (t.presents === t.startAt && command === 'present') || t.entries.length > 20000) return;
    const {bytes, world, view, projection, ...rest} = args ?? {};
    t.entries.push({command, ...rest, ...(bytes ? {bytes: bytes.byteLength} : {}), ...(world ? {world: Array.from(world).map(x => +x.toFixed(3))} : {}), ...(command === 'drawIndexed' ? {vertexFormat: formats.get(args.vertices), textureSize: sizes.get(args.texture), texture1Size: sizes.get(args.texture1)} : {}), ...(result !== undefined && result !== null && typeof result !== 'object' ? {result} : {})});
    if (command === 'present' && t.presents > (t.startAt ?? 0) && --t.remaining === 0) t.onDone?.(t.entries);
  }
  const formats = new Map(), sizes = new Map();
  function call(command, args = {}) {
    const result = callInner(command, args);
    if (command === 'uploadVertices') formats.set(result, args.format);
    if (command === 'uploadTextureRGBA' || command === 'createRenderTarget') sizes.set(result, `${args.width}x${args.height}${command === 'createRenderTarget' ? 'RT' : ''}`);
    trace(command, args, result);
    return result;
  }
  function callInner(command, args = {}) {
    if (closed) throw new Error('D3D graphics host closed');
    if (command === 'capabilities') return capabilities;
    if (command === 'createDevice') {
      if (device) throw new Error('Only one D3D device is supported');
      if (Object.keys(args).some(k => !['width', 'height', 'hwnd', 'depth16', 'windowed', 'presentationInterval', 'backBufferFormat'].includes(k))) throw new Error('Unknown device option');
      const backBufferFormat = args.backBufferFormat ?? 22;
      if (!renderTargetFormats.includes(backBufferFormat)) throw new Error('Unsupported logical backbuffer format');
      const window = getWindow(args.hwnd), interval = args.presentationInterval === 0 ? 1 : args.presentationInterval;
      if (!Number.isInteger(args.hwnd) || args.hwnd <= 0 || !window || window.clientWidth !== caps.width || window.clientHeight !== caps.height || args.width !== caps.width || args.height !== caps.height) throw new Error('Device must match an acknowledged game window and initialized GPU dimensions');
      if (typeof args.windowed !== 'boolean' || typeof args.depth16 !== 'boolean' || (args.depth16 && caps.depthFormat !== 'depth16unorm')) throw new Error('Unsupported device depth/window parameters');
      if (![1, 0x80000000].includes(interval) || !((presentationIntervalMask >>> 0) & interval)) throw new Error('Unsupported presentation interval');
      sync('validate', {}); // Surface real device loss before claiming creation.
      if (sync('setRenderTargetFormat', {format:backBufferFormat}) !== backBufferFormat) throw new Error('Backbuffer format was not acknowledged');
      device = {...args, backBufferFormat, presentationInterval: interval};
      return true;
    }
    if (!device) throw new Error('D3D device has not been created');
    if (command === 'destroyDevice') {
      if (handles.size) throw new Error('Live GPU resources remain on device destruction');
      sync('dispose', {}); device = null; closed = true;
      return true;
    }
    if (!['clear', 'present', 'uploadVertices', 'uploadIndices', 'uploadTextureRGBA', 'createRenderTarget', 'setRenderTarget', 'readRenderTarget', 'readPixels', 'writeRenderTarget', 'copyRenderTarget', 'drawIndexed', 'release'].includes(command)) throw new Error(`Unsupported D3D bridge command ${command}`);
    validateCommand(command, args);
    if (command === 'uploadVertices' && !VERTEX_FORMATS.includes(args.format)) throw new Error('Native vertex format unsupported');
    if (command.startsWith('upload') && args.bytes.byteLength > maxBufferBytes) throw new RangeError('Upload exceeds implemented device limit');
    if ((command === 'uploadTextureRGBA' || command === 'createRenderTarget') && (args.width > capabilities.maxTextureDimension || args.height > capabilities.maxTextureDimension)) throw new RangeError('Texture exceeds actual device limit');
    if (command === 'setRenderTarget' && args.texture !== null && handles.get(args.texture) !== 'createRenderTarget') throw new Error('Render target does not belong to this device');
    if ((command === 'readRenderTarget' || command === 'writeRenderTarget') && handles.get(args.texture) !== 'createRenderTarget') throw new Error('Render target does not belong to this device');
    if (command === 'copyRenderTarget' && (handles.get(args.source) !== 'createRenderTarget' || handles.get(args.destination) !== 'createRenderTarget')) throw new Error('Render target does not belong to this device');
    if (command === 'writeRenderTarget' && args.bytes.byteLength > maxBufferBytes) throw new RangeError('Upload exceeds implemented device limit');
    if (command === 'release' && !handles.has(args.handle)) throw new Error('Resource does not belong to this device');
    if (command === 'drawIndexed' && (handles.get(args.vertices) !== 'uploadVertices' || handles.get(args.indices) !== 'uploadIndices' || (args.texture != null && !['uploadTextureRGBA', 'createRenderTarget'].includes(handles.get(args.texture))) || (args.texture1 != null && !['uploadTextureRGBA', 'createRenderTarget'].includes(handles.get(args.texture1))))) throw new Error('Invalid native draw resources');
    const result = deferred && DEFERRABLE.has(command) ? defer(command, args) : sync(command, args);
    if (command === 'createRenderTarget') targetSizes.set(result, {width: args.width, height: args.height});
    if (command.startsWith('upload') || command === 'createRenderTarget') {
      if (!Number.isInteger(result) || result < 1 || result > 0x7fffffff || handles.has(result)) throw new Error('Invalid GPU resource handle');
      handles.set(result, command);
    }
    if (command === 'release') { handles.delete(args.handle); targetSizes.delete(args.handle); }
    if (command === 'setRenderTarget' && !(Number.isInteger(result?.width) && Number.isInteger(result?.height))) throw new Error('Render target switch was not acknowledged');
    if (command === 'present' && (result?.acknowledged !== true || !Number.isInteger(result.serial) || result.serial < 1)) throw new Error('GPU presentation was not acknowledged');
    return result;
  }
  return {moviesGraphicsCall: call, flush, close() { closed = true; device = null; handles.clear(); queue = []; client.close(); }};
}
