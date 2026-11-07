/* Bounded, read-only HTTP backing for large game archives in Emscripten FS. */
(function (global) {
  'use strict';
  const integer = value => Number.isSafeInteger(value) && value >= 0;

  function requestRange(url, start, end, size) {
    let mismatch;
    const attempted = new Set();
    for (const overlap of [0, 1, 2, 4, 8, 16, 256, 4096]) {
      const wireStart = Math.max(0, start - overlap);
      if (attempted.has(wireStart)) continue;
      attempted.add(wireStart);
      const xhr = new XMLHttpRequest();
      xhr.open('GET', url, false);
      xhr.setRequestHeader('Range', `bytes=${wireStart}-${end}`);
      // ArrayBuffer responseType is forbidden for synchronous main-thread XHR.
      xhr.overrideMimeType('text/plain; charset=x-user-defined');
      xhr.send();
      if (xhr.status !== 206) throw new Error(`Range request returned HTTP ${xhr.status}; expected 206`);
      if (xhr.getResponseHeader('Content-Range') !== `bytes ${wireStart}-${end}/${size}`) {
        throw new Error('Invalid Content-Range for game archive');
      }
      const text = xhr.responseText;
      if (text.length !== end - wireStart + 1) {
        // Browsers sniff UTF BOMs before honoring x-user-defined. Retry with
        // preceding bytes so binary FF FE / FE FF / EF BB BF is not at byte 0.
        mismatch = `Truncated range response: ${wireStart}-${end} expected ${end-wireStart+1}, got ${text.length}, type ${xhr.getResponseHeader('Content-Type')}`;
        continue;
      }
      const discard = start - wireStart;
      const data = new Uint8Array(end - start + 1);
      for (let i = 0; i < data.length; i++) data[i] = text.charCodeAt(discard + i) & 255;
      return data;
    }
    throw new Error(`${mismatch}; byte-preserving decode failed after ${attempted.size} bounded attempts`);
  }

  /** request, when supplied for testing, receives (url, start, end, totalSize). */
  function createRangeReader({ url, size, chunkSize = 256 * 1024, maxChunks = 128, request = requestRange }) {
    if (typeof url !== 'string' || !url || !integer(size) || !integer(chunkSize) || chunkSize === 0 || !integer(maxChunks) || maxChunks === 0 || typeof request !== 'function') {
      throw new TypeError('Invalid range reader configuration');
    }
    const cache = new Map();
    const stats = { hits: 0, requests: 0, bytes: 0, cacheBytes: 0 };
    function chunk(index) {
      if (cache.has(index)) {
        const data = cache.get(index);
        cache.delete(index);
        cache.set(index, data);
        stats.hits++;
        return data;
      }
      const start = index * chunkSize;
      const end = Math.min(start + chunkSize - 1, size - 1);
      stats.requests++;
      const data = request(url, start, end, size);
      if (!ArrayBuffer.isView(data) || data.BYTES_PER_ELEMENT !== 1 || data.length !== end - start + 1) {
        throw new Error('Range request must return the exact requested bytes');
      }
      // Retain an owned copy: a custom transport may reuse its response buffer.
      const owned = new Uint8Array(data.length);
      owned.set(data);
      stats.bytes += owned.length;
      cache.set(index, owned);
      stats.cacheBytes += owned.length;
      while (cache.size > maxChunks) {
        const key = cache.keys().next().value;
        stats.cacheBytes -= cache.get(key).length;
        cache.delete(key);
      }
      return owned;
    }
    function read(target, offset, length, position) {
      if (!ArrayBuffer.isView(target) || target.BYTES_PER_ELEMENT !== 1 || !integer(offset) || !integer(length) || !integer(position) || offset > target.length || length > target.length - offset) {
        throw new RangeError('Invalid read buffer or bounds');
      }
      const count = Math.min(length, Math.max(0, size - position));
      let copied = 0;
      while (copied < count) {
        const at = position + copied;
        const data = chunk(Math.floor(at / chunkSize));
        const inside = at % chunkSize;
        const amount = Math.min(count - copied, data.length - inside);
        target.set(data.subarray(inside, inside + amount), offset + copied);
        copied += amount;
      }
      return count;
    }
    return { read, size, stats };
  }

  function mount(FS, options) {
    const { path, ...settings } = options;
    if (typeof path !== 'string' || !path.startsWith('/') || path.endsWith('/') || path.split('/').some(part => part === '..' || part === '.')) {
      throw new TypeError('A normalized absolute file path is required');
    }
    const reader = createRangeReader(settings);
    const slash = path.lastIndexOf('/');
    const node = FS.createFile(path.slice(0, slash) || '/', path.slice(slash + 1), {}, true, false);
    const originalGetattr = node.node_ops.getattr;
    node.node_ops = { ...node.node_ops, getattr(current) {
      return { ...originalGetattr(current), size: reader.size, blocks: Math.ceil(reader.size / 512) };
    } };
    node.stream_ops = { ...node.stream_ops,
      read(stream, buffer, offset, length, position) {
        return reader.read(buffer, offset, length, position);
      },
      llseek(stream, offset, whence) {
        if (!Number.isSafeInteger(offset) || ![0, 1, 2].includes(whence)) throw new FS.ErrnoError(28);
        const position = offset + (whence === 1 ? stream.position : whence === 2 ? reader.size : 0);
        if (!integer(position)) throw new FS.ErrnoError(28);
        return position;
      }
    };
    return { node, ...reader };
  }

  global.MoviesRangeFile = { createRangeReader, mount };
})(globalThis);
