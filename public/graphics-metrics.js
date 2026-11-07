/* Fixed-key bounded aggregates. No per-call logs or retained sample arrays. */
export class GraphicsMetrics {
  constructor(names, {frames = 120} = {}) {
    if (!Array.isArray(names) || !names.includes('frameMs') || names.length > 64 || new Set(names).size !== names.length || names.some(n => typeof n !== 'string' || n.length > 64) || !Number.isInteger(frames) || frames < 1 || frames > 10000) throw new TypeError('Invalid metrics configuration');
    this.names = [...names]; this.limit = frames; this.previous = null; this.reset();
  }
  reset() { this.frames = 0; this.values = Object.fromEntries(this.names.map(n => [n, {count: 0, sum: 0, max: 0}])); }
  add(name, value) {
    const target = Object.hasOwn(this.values, name) ? this.values[name] : null;
    if (!target || !Number.isFinite(value) || value < 0) throw new TypeError('Invalid metrics sample');
    target.count++; target.sum += value; target.max = Math.max(target.max, value);
  }
  frame(serial, now) {
    if (!Number.isInteger(serial) || serial < 1 || !Number.isFinite(now) || (this.previous && (serial <= this.previous.serial || now < this.previous.now))) throw new TypeError('Invalid metrics frame');
    if (!this.previous) { this.previous = {serial, now}; this.reset(); return null; }
    this.add('frameMs', now - this.previous.now); this.previous = {serial, now};
    if (++this.frames < this.limit) return null;
    const report = {frames: this.frames, lastSerial: serial, values: this.values};
    this.reset(); return report;
  }
}
export function emitGraphicsMetrics(callback, report) {
  // Instrumentation failures must never change an original graphics result.
  if (report && typeof callback === 'function') { try { callback(report); } catch {} }
}
