/* Synthetic Linux /proc/stat for Boxedwine's browser host. No host CPU usage
 * is measured: elapsed 100 Hz ticks are reported entirely as idle time. */
(function (global) {
  'use strict';
  function install(FS, { cpuCount = global.navigator?.hardwareConcurrency || 1, now = () => global.performance.now() } = {}) {
    if (typeof now !== 'function') throw new TypeError('now must be a clock function');
    cpuCount = Math.max(1, Math.min(64, Math.floor(Number(cpuCount)) || 1));
    const began = now();
    if (!Number.isFinite(began)) throw new TypeError('Clock must return finite milliseconds');
    const encode = new TextEncoder();
    const snapshotKey = Symbol('moviesProcStatSnapshot');
    let ticks = 0;
    function snapshot() {
      const current = now();
      if (!Number.isFinite(current)) throw new TypeError('Clock must return finite milliseconds');
      ticks = Math.max(ticks, Math.floor(Math.max(0, current - began) / 10));
      // Every CPU is synthetic idle, so aggregate idle is the per-CPU sum.
      const line = (label, idle) => `${label} 0 0 0 ${idle} 0 0 0 0 0 0\n`;
      let text = line('cpu', ticks * cpuCount);
      for (let i = 0; i < cpuCount; i++) text += line(`cpu${i}`, ticks);
      text += 'intr 0\nctxt 0\nbtime 0\nprocesses 0\nprocs_running 0\nprocs_blocked 0\n';
      return encode.encode(text);
    }
    FS.mkdirTree('/proc');
    FS.writeFile('/proc/stat', snapshot(), { mode: 0o444 });
    const node = FS.lookupPath('/proc/stat').node;
    // Preserve the regular-file type while removing write/execute permissions.
    node.mode = (node.mode & ~0o777) | 0o444;
    const originalGetattr = node.node_ops.getattr;
    const originalOpen = node.stream_ops.open;
    const dataFor = stream => stream[snapshotKey] || (stream[snapshotKey] = snapshot());
    node.node_ops = { ...node.node_ops, getattr(current) {
      const bytes = snapshot().length;
      return { ...originalGetattr(current), size: bytes, blocks: Math.ceil(bytes / 512) };
    } };
    node.stream_ops = { ...node.stream_ops,
      open(stream) {
        if (originalOpen) originalOpen(stream);
        stream[snapshotKey] = snapshot();
      },
      read(stream, buffer, offset, length, position) {
        if (![offset, length, position].every(value => Number.isSafeInteger(value) && value >= 0) || offset > buffer.length || length > buffer.length - offset) {
          throw new FS.ErrnoError(28);
        }
        const data = dataFor(stream);
        const count = Math.min(length, Math.max(0, data.length - position));
        buffer.set(data.subarray(position, position + count), offset);
        return count;
      },
      llseek(stream, offset, whence) {
        if (!Number.isSafeInteger(offset) || ![0, 1, 2].includes(whence)) throw new FS.ErrnoError(28);
        const position = offset + (whence === 1 ? stream.position : whence === 2 ? dataFor(stream).length : 0);
        if (!Number.isSafeInteger(position) || position < 0) throw new FS.ErrnoError(28);
        return position;
      },
      write() { throw new FS.ErrnoError(69); }
    };
    return { node, cpuCount };
  }
  global.MoviesProcStat = { install };
})(globalThis);
