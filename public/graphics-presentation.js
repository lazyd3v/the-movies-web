import {GraphicsMetrics, emitGraphicsMetrics} from './graphics-metrics.js';
/* D3DPRESENT_INTERVAL_ONE approximation for the browser's virtual display:
 * one consumed frame per requestAnimationFrame callback while the page is
 * visible. Hidden pages receive (almost) no animation frames, which previously
 * timed Present out and made the game treat its device as lost; there the frame
 * is consumed immediately instead (no scanout exists to pace against).
 * Physical scanout is not observable. */
export function createGraphicsPresenter({ack, consume, requestFrame = globalThis.requestAnimationFrame?.bind(globalThis),
  cancelFrame = globalThis.cancelAnimationFrame?.bind(globalThis), timeoutMs = 10000, onError = () => {}, profile = false, onMetrics = null,
  isHidden = () => globalThis.document?.visibilityState === 'hidden', visibilityTarget = globalThis.document ?? null}) {
  if (!(ack instanceof Int32Array) || !(ack.buffer instanceof SharedArrayBuffer) || ![2, 3].includes(ack.length) || typeof consume !== 'function' || typeof requestFrame !== 'function' || typeof cancelFrame !== 'function') throw new TypeError('Invalid graphics presenter');
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 60000) throw new RangeError('Invalid presentation timeout');
  if(typeof profile !== 'boolean' || (onMetrics !== null && typeof onMetrics !== 'function')) throw new TypeError('Invalid presentation profiling');
  const metrics=profile?new GraphicsMetrics(['frameMs','rafWaitMs','consumeMs']):null;
  if (typeof isHidden !== 'function') throw new TypeError('Invalid visibility query');
  let pending = null, lastSerial = 0, closed = false;
  function deliver(frame) {
    if (pending !== frame) return;
    try {
      const began=metrics?performance.now():0;
      if(metrics)metrics.add('rafWaitMs',began-frame.arrived);
      const result = consume(frame.bitmap, frame.serial);
      if (result && typeof result.then === 'function') throw new Error('Bitmap consumer must finish synchronously inside animation frame');
      if(metrics){const now=performance.now();metrics.add('consumeMs',now-began);const report=metrics.frame(frame.serial,now);finish(frame,true);emitGraphicsMetrics(onMetrics,report?{source:'graphics-page',...report}:null);}else finish(frame, true);
    } catch (error) { finish(frame, false, error); }
  }
  const publishVisibility = () => { if (ack.length === 3) Atomics.store(ack, 2, isHidden() ? 1 : 0); };
  const onVisibility = () => { publishVisibility(); if (pending && isHidden()) deliver(pending); };
  publishVisibility();
  visibilityTarget?.addEventListener?.('visibilitychange', onVisibility);
  function finish(frame, success, error) {
    if (pending !== frame) return;
    pending = null; clearTimeout(frame.timer); cancelFrame(frame.raf);
    frame.bitmap.close(); Atomics.store(ack, 1, success ? 1 : 0); Atomics.store(ack, 0, frame.serial); Atomics.notify(ack, 0);
    if (error) onError(error);
  }
  return {
    submit({bitmap, serial}) {
      if (closed || !Number.isInteger(serial) || serial < 1 || serial > 0x7fffffff || serial <= lastSerial || !bitmap || typeof bitmap.close !== 'function') {
        bitmap?.close?.(); throw new Error('Invalid or stale graphics frame');
      }
      // The worker stops waiting for an acknowledgement after a bounded time (slow or
      // sleeping display) and presents the next frame; the superseded one is dropped
      // without an acknowledgement, which the worker no longer waits for.
      if (pending) { const old = pending; pending = null; clearTimeout(old.timer); cancelFrame(old.raf); old.bitmap.close(); }
      lastSerial = serial;
      const frame = {bitmap, serial, timer: null, raf: null, arrived: metrics ? performance.now() : 0}; pending = frame;
      if (isHidden()) { deliver(frame); return; }
      // No animation frame within the timeout (throttled or occluded page): present anyway.
      frame.timer = setTimeout(() => deliver(frame), timeoutMs);
      frame.raf = requestFrame(() => deliver(frame));
    },
    close() { closed = true; visibilityTarget?.removeEventListener?.('visibilitychange', onVisibility); if (pending) finish(pending, false, new Error('Graphics presenter closed')); }
  };
}
