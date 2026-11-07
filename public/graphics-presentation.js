import {GraphicsMetrics, emitGraphicsMetrics} from './graphics-metrics.js';
/* D3DPRESENT_INTERVAL_ONE approximation for the browser's virtual display:
 * one consumed frame per requestAnimationFrame callback, never an immediate
 * acknowledgement before that callback. Physical scanout is not observable. */
export function createGraphicsPresenter({ack, consume, requestFrame = globalThis.requestAnimationFrame?.bind(globalThis),
  cancelFrame = globalThis.cancelAnimationFrame?.bind(globalThis), timeoutMs = 10000, onError = () => {}, profile = false, onMetrics = null}) {
  if (!(ack instanceof Int32Array) || !(ack.buffer instanceof SharedArrayBuffer) || ack.length !== 2 || typeof consume !== 'function' || typeof requestFrame !== 'function' || typeof cancelFrame !== 'function') throw new TypeError('Invalid graphics presenter');
  if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 60000) throw new RangeError('Invalid presentation timeout');
  if(typeof profile !== 'boolean' || (onMetrics !== null && typeof onMetrics !== 'function')) throw new TypeError('Invalid presentation profiling');
  const metrics=profile?new GraphicsMetrics(['frameMs','rafWaitMs','consumeMs']):null;
  let pending = null, lastSerial = 0, closed = false;
  function finish(frame, success, error) {
    if (pending !== frame) return;
    pending = null; clearTimeout(frame.timer); cancelFrame(frame.raf);
    frame.bitmap.close(); Atomics.store(ack, 1, success ? 1 : 0); Atomics.store(ack, 0, frame.serial); Atomics.notify(ack, 0);
    if (error) onError(error);
  }
  return {
    submit({bitmap, serial}) {
      if (closed || pending || !Number.isInteger(serial) || serial < 1 || serial > 0x7fffffff || serial <= lastSerial || !bitmap || typeof bitmap.close !== 'function') {
        bitmap?.close?.(); throw new Error('Invalid, overlapping or stale graphics frame');
      }
      lastSerial = serial;
      const frame = {bitmap, serial, timer: null, raf: null, arrived: metrics ? performance.now() : 0}; pending = frame;
      frame.timer = setTimeout(() => finish(frame, false, new Error('Presentation animation-frame timeout')), timeoutMs);
      frame.raf = requestFrame(() => {
        if (pending !== frame) return;
        try {
          const began=metrics?performance.now():0;
          if(metrics)metrics.add('rafWaitMs',began-frame.arrived);
          const result = consume(bitmap, serial);
          if (result && typeof result.then === 'function') throw new Error('Bitmap consumer must finish synchronously inside animation frame');
          if(metrics){const now=performance.now();metrics.add('consumeMs',now-began);const report=metrics.frame(serial,now);finish(frame,true);emitGraphicsMetrics(onMetrics,report?{source:'graphics-page',...report}:null);}else finish(frame, true);
        } catch (error) { finish(frame, false, error); }
      });
    },
    close() { closed = true; if (pending) finish(pending, false, new Error('Graphics presenter closed')); }
  };
}
