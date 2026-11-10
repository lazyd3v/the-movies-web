import {GraphicsMetrics} from './graphics-metrics.js';
import {MoviesD3D9GPU} from './d3d9-gpu.js';
import {GRAPHICS_LIMITS, validateCommand, responseViews, publishResponse} from './graphics-protocol.js';
let inFlightSerial = 0;
let graphics = null, views = null, expectedSerial = 1, chain = Promise.resolve(), presentationPort, presentationAck, timeoutMs;
let metrics = null, slowReports = 0, droppedFrames = 0;
let nextHandle = 1, resourceBytes = 0;
const resources = new Map(), MAX_RESOURCES = 8192, MAX_RESOURCE_BYTES = 256 * 1024 * 1024;
function resource(handle) { const entry = resources.get(handle); if (!entry) throw new Error(`Unknown graphics resource ${handle}`); return entry.value; }
function reserve(bytes) {
  if (resources.size >= MAX_RESOURCES || resourceBytes + bytes > MAX_RESOURCE_BYTES || nextHandle > 0x7fffffff) throw new Error('Graphics resource budget exhausted');
}
function save(value, bytes) { const handle = nextHandle++; resources.set(handle, {value, bytes}); resourceBytes += bytes; return handle; }
async function execute(command, args) {
  switch (command) {
    case 'setRenderTargetFormat': return graphics.setRenderTargetFormat(args.format);
    case 'clear': graphics.clear(args); return null;
    case 'uploadVertices': reserve(args.bytes.byteLength); return save(graphics.uploadVertices(args.bytes, args.format, {adopt: true}), args.bytes.byteLength);
    case 'uploadIndices': reserve(args.bytes.byteLength); return save(graphics.uploadIndices(args.bytes, args.format, {adopt: true}), args.bytes.byteLength);
    case 'uploadTextureRGBA': reserve(args.bytes.byteLength); return save(graphics.uploadTextureRGBA(args.width, args.height, args.bytes), args.bytes.byteLength);
    case 'createRenderTarget': reserve(args.width * args.height * 6); return save(graphics.createRenderTarget(args.width, args.height), args.width * args.height * 6);
    case 'copyRenderTarget': graphics.copyRenderTarget(resource(args.source), resource(args.destination), args.region); return null;
    case 'readRenderTarget': return await graphics.readRenderTarget(resource(args.texture));
    case 'writeRenderTarget': graphics.writeRenderTarget(resource(args.texture), args.width, args.height, args.bytes); return null;
    case 'setRenderTarget': return graphics.setRenderTarget(args.texture ? resource(args.texture) : null);
    case 'drawIndexed': await graphics.drawIndexed({...args, vertices: resource(args.vertices), indices: resource(args.indices), texture: args.texture ? resource(args.texture) : null, ...(args.texture1 ? {texture1: resource(args.texture1)} : {})}); return null;
    case 'release': { const r = resources.get(args.handle); if (!r) throw new Error('Unknown released resource'); graphics.release(r.value); resourceBytes -= r.bytes; resources.delete(args.handle); return null; }
    case 'readPixels': return await graphics.readPixels();
    case 'validate': await graphics.validate(); return null;
    case 'present': {
      // Always-on, bounded slow-present report: phase timings reach the page log
      // even when the guest has already timed out, so stalls can be attributed.
      const phases = {began: performance.now()};
      let stamp = metrics ? performance.now() : 0;
      // Per-frame health check without waiting for GPU idle: blocking on
      // onSubmittedWorkDone here serialized the guest with the GPU every frame.
      graphics.healthy();
      phases.preValidated = performance.now();
      if (metrics) metrics.add('preValidateMs', performance.now()-stamp);
      stamp = metrics ? performance.now() : 0;
      // Surface prior asynchronous GPU validation failures before reporting a frame.
      const result = graphics.present((bitmap, serial) => {
      if (metrics) metrics.add('bitmapMs', performance.now()-stamp);
      const ackBegan = metrics ? performance.now() : 0; phases.bitmap = performance.now();
      // Optional third word: page-published hidden flag. A hidden page has no
      // visible scanout and may not service messages promptly, so the frame is
      // completed here and dropped instead of waiting for a page acknowledgement.
      if (presentationAck.length === 3 && Atomics.load(presentationAck, 2) === 1) { inFlightSerial = 0; if (serial % 60 === 0) presentationPort.postMessage({type: 'graphics-hidden-frame', serial, bitmap}, [bitmap]); else { bitmap.close(); presentationPort.postMessage({type: 'graphics-hidden-frame', serial}); } if (metrics) metrics.add('ackMs', 0); return {serial, acknowledged: true, hidden: true}; }
      // One frame in flight: wait for the page to acknowledge the PREVIOUS frame,
      // then hand over this one and return at once. The guest renders frame N+1
      // while the page scans out N, instead of idling until the next animation
      // frame every time (which halved the rate whenever a frame took > 1 vsync).
      // A page that stops producing animation frames (display asleep, lid closed,
      // throttled tab) must not kill the game: after a bounded wait the previous
      // frame is treated as dropped and the guest continues.
      if (inFlightSerial) {
        const deadline = performance.now() + Math.min(timeoutMs, 1000);
        while (Atomics.load(presentationAck, 0) !== inFlightSerial) {
          const remaining = deadline - performance.now();
          if (remaining <= 0) { droppedFrames++; if (droppedFrames === 1 || droppedFrames % 600 === 0) presentationPort.postMessage({type: 'graphics-slow-present', serial: inFlightSerial, dropped: droppedFrames}); break; }
          Atomics.wait(presentationAck, 0, Atomics.load(presentationAck, 0), remaining);
        }
        if (Atomics.load(presentationAck, 0) === inFlightSerial && Atomics.load(presentationAck, 1) !== 1) throw new Error('Page rejected presented bitmap');
      }
      if (metrics) metrics.add('ackMs', performance.now()-ackBegan);
      Atomics.store(presentationAck, 0, 0); Atomics.store(presentationAck, 1, 0);
      presentationPort.postMessage({type: 'graphics-frame', bitmap, serial}, [bitmap]);
      inFlightSerial = serial;
      return {serial, acknowledged: true};
      });
      phases.acknowledged = performance.now();
      stamp = metrics ? performance.now() : 0;
      graphics.healthy();
      phases.postValidated = performance.now();
      if (phases.postValidated - phases.began > 2000 && slowReports++ < 16) presentationPort.postMessage({type: 'graphics-slow-present', serial: result.serial,
        preValidateMs: phases.preValidated - phases.began, bitmapMs: phases.bitmap - phases.preValidated, ackMs: phases.acknowledged - phases.bitmap, postValidateMs: phases.postValidated - phases.acknowledged});
      if (metrics) { metrics.add('postValidateMs',performance.now()-stamp); const report=metrics.frame(result.serial,performance.now()); if(report) result.metrics=report; }
      return result;
    }
    case 'batch': {
      const results = [];
      for (let i = 0; i < args.commands.length; i++) {
        try { results.push(await execute(args.commands[i].command, args.commands[i].args ?? {})); }
        catch (error) { throw new Error(`Batch command ${i} failed (earlier commands may have executed): ${error.message}`); }
      }
      return results;
    }
    case 'dispose': graphics.dispose(); graphics = null; resources.clear(); resourceBytes = 0; return null;
    default: throw new Error(`Unsupported graphics command ${command}`);
  }
}
async function handle(message) {
  let serial = message?.serial;
  try {
    if (!message || Object.keys(message).some(k => !['serial', 'command', 'args'].includes(k)) || !Number.isInteger(serial) || serial < 1 || serial > 0x7fffffff) throw new Error('Invalid graphics message');
    if (message.command === 'init' && views === null) views = responseViews(message.args?.response);
    if (!views) throw new Error('Graphics worker not initialized');
    if (serial !== expectedSerial++) throw new Error('Graphics request serial mismatch');
    let result;
    if (message.command === 'init') {
      if (graphics || serial !== 1) throw new Error('Duplicate graphics initialization');
      const {width, height, timeoutMs: timeout, presentationPort: port, presentationAck: ack, profile = false} = message.args;
      if (Object.keys(message.args).some(k => !['width', 'height', 'timeoutMs', 'response', 'presentationPort', 'presentationAck','profile'].includes(k))) throw new Error('Unknown initialization field');
      if (![width, height].every(n => Number.isInteger(n) && n > 0 && n <= GRAPHICS_LIMITS.maxDimension)) throw new RangeError('Invalid target dimensions');
      if (width * height * 4 > views.payload.length) throw new RangeError('Target readback exceeds response capacity');
      if (!Number.isInteger(timeout) || timeout < 1 || timeout > GRAPHICS_LIMITS.maxTimeoutMs || !(ack instanceof SharedArrayBuffer) || ![8, 12].includes(ack.byteLength) || !(port instanceof MessagePort)) throw new TypeError('Invalid presentation transport');
      if(typeof profile !== 'boolean') throw new TypeError('Invalid profiling flag');
      if(profile) metrics=new GraphicsMetrics(['frameMs','preValidateMs','bitmapMs','ackMs','postValidateMs']);
      presentationPort = port; presentationAck = new Int32Array(ack); timeoutMs = timeout;
      graphics = await MoviesD3D9GPU.create({canvas: new OffscreenCanvas(width, height), width, height});
      result = graphics.capabilities;
    } else {
      if (!graphics) throw new Error('Graphics device unavailable or disposed');
      validateCommand(message.command, message.args); // Whole batch structurally validated before first execution.
      result = await execute(message.command, message.args);
    }
    publishResponse(views, serial, result);
  } catch (error) {
    if (views && Number.isInteger(serial) && serial > 0 && serial <= 0x7fffffff) {
      publishResponse(views, serial, {error: String(error?.message ?? error).slice(0, 512)}, true);
    }
  }
}
function enqueue(message) { chain = chain.then(() => handle(message)).catch(error => { console.error('Graphics dispatcher failure', error); }); }
self.onmessage = event => {
  if (event.data?.type === 'graphics-connect') {
    const port = event.data.port;
    if (!(port instanceof MessagePort)) throw new TypeError('Invalid graphics command port');
    port.onmessage = event => enqueue(event.data); port.start();
    self.postMessage({type: 'graphics-worker-connected'});
  } else enqueue(event.data);
};
self.postMessage({type: 'graphics-worker-ready'});
