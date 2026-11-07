/* Start GPU worker from the page BEFORE the guest blocks. Browser nested-worker
 * module startup cannot be assumed to progress while its owner Atomics.waits.
 * The page remains able to report worker script/runtime errors through SAB. */
import {GRAPHICS_LIMITS, HEADER, responseViews, publishResponse} from './graphics-protocol.js';
export async function prepareGraphicsTransport({width, height, timeoutMs = 15000, workerURL = new URL('./graphics-worker.js', import.meta.url)} = {}) {
  if (![width, height].every(n => Number.isInteger(n) && n > 0 && n <= GRAPHICS_LIMITS.maxDimension)) throw new RangeError('Invalid graphics target');
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > GRAPHICS_LIMITS.maxTimeoutMs) throw new RangeError('Invalid startup timeout');
  const responseBuffer = new SharedArrayBuffer(GRAPHICS_LIMITS.headerBytes + Math.max(1024 * 1024, width * height * 4));
  const views = responseViews(responseBuffer), channel = new MessageChannel();
  const worker = new Worker(workerURL, {type: 'module', name: 'movies-gpu'});
  let rejectStartup, ready = false;
  const startup = new Promise((resolve, reject) => {
    rejectStartup = reject;
    const timer = setTimeout(() => { worker.terminate(); reject(new Error('Graphics worker module startup/connection timeout')); }, timeoutMs);
    worker.onmessage = ({data}) => {
      if (data?.type === 'graphics-worker-ready') worker.postMessage({type: 'graphics-connect', port: channel.port2}, [channel.port2]);
      else if (data?.type === 'graphics-worker-connected') { ready = true; clearTimeout(timer); resolve(); }
    };
    worker.onerror = event => {
      event.preventDefault(); clearTimeout(timer);
      const error = new Error(`Graphics worker script/runtime error: ${event.message} at ${event.filename}:${event.lineno}`);
      const serial = Math.max(1, Atomics.load(views.header, HEADER.serial));
      publishResponse(views, serial, {error: error.message.slice(0, 512)}, true);
      Atomics.store(views.header, HEADER.transportFailure, 1); Atomics.notify(views.header, HEADER.status);
      if (!ready) rejectStartup(error);
    };
  });
  try { await startup; }
  catch (error) { worker.terminate(); channel.port1.close(); throw error; }
  return {commandPort: channel.port1, responseBuffer, close() { worker.terminate(); channel.port1.close(); }};
}
