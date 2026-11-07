/* Original ZIP_STORED payloads exposed as files through one bounded HTTP cache. */
(function (global) {
  'use strict';
  const integer = n => Number.isSafeInteger(n) && n >= 0;
  const hash = s => typeof s === 'string' && /^[a-f0-9]{64}$/.test(s);
  function relative(path) {
    return typeof path === 'string' && path && !/[\\:\x00-\x1f]/.test(path) &&
      path.split('/').every(part => part && part !== '.' && part !== '..');
  }
  function validateIndex(index) {
    if (!index || index.schemaVersion !== 1 || !integer(index.archiveBytes) || !hash(index.archiveSha256) ||
        !relative(index.archive) || index.archive.includes('/') || !Array.isArray(index.files) ||
        index.fileCount !== index.files.length || !index.executable || !hash(index.executable.sha256)) {
      throw new TypeError('Invalid stored archive index');
    }
    const files = [], names = new Set();
    let total = 0;
    for (const entry of index.files) {
      if (!entry || !relative(entry.path) || !integer(entry.size) || !integer(entry.offset) ||
          entry.offset > index.archiveBytes || entry.size > index.archiveBytes - entry.offset ||
          names.has(entry.path.toLowerCase())) throw new TypeError('Invalid indexed file or duplicate path');
      names.add(entry.path.toLowerCase());
      total += entry.size;
      if (!integer(total)) throw new TypeError('Index total overflow');
      files.push({ path: entry.path, size: entry.size, offset: entry.offset });
    }
    for (const entry of files) {
      const parts = entry.path.split('/');
      for (let i = 1; i < parts.length; i++) {
        if (names.has(parts.slice(0, i).join('/').toLowerCase())) throw new TypeError('File/directory collision');
      }
    }
    const intervals = files.filter(e => e.size).sort((a, b) => a.offset - b.offset);
    for (let i = 1; i < intervals.length; i++) {
      if (intervals[i - 1].offset + intervals[i - 1].size > intervals[i].offset) throw new TypeError('Overlapping file ranges');
    }
    if (total !== index.totalBytes || !files.some(e => e.path === index.executable.path)) throw new TypeError('Index totals/executable mismatch');
    return files;
  }
  async function mount(FS, { index, url, path = '/game', preloaded = new Map(), request } = {}) {
    const files = validateIndex(index);
    if (typeof path !== 'string' || !path.startsWith('/') || !relative(path.slice(1))) throw new TypeError('Normalized absolute mount path required');
    if (!(preloaded instanceof Map)) throw new TypeError('preloaded must be a Map');
    // Copy index/bytes before the async hash check; caller mutation cannot alter validated ranges.
    const archiveBytes = index.archiveBytes, executable = { ...index.executable };
    const owned = new Map();
    for (const [name, bytes] of preloaded) {
      const entry = files.find(e => e.path === name);
      if (!entry || name !== executable.path || !(bytes instanceof Uint8Array) || bytes.length !== entry.size) {
        throw new TypeError('Only the indexed original executable may be preloaded');
      }
      const copy = bytes.slice();
      const digest = new Uint8Array(await global.crypto.subtle.digest('SHA-256', copy));
      const actual = Array.from(digest, x => x.toString(16).padStart(2, '0')).join('');
      if (actual !== executable.sha256) throw new Error('Preloaded executable hash mismatch');
      owned.set(name, copy);
    }
    const reader = global.MoviesRangeFile.createRangeReader({ url, size: archiveBytes,
      chunkSize: 256 * 1024, maxChunks: 128, ...(request ? { request } : {}) });
    if (FS.analyzePath(path).exists) throw new Error('Mount path must not already exist');
    const denyWrite = () => { throw new FS.ErrnoError(69); }; // EROFS
    const dirs = new Set([path]);
    for (const entry of files) {
      const parts = entry.path.split('/');
      for (let i = 1; i < parts.length; i++) dirs.add(`${path}/${parts.slice(0, i).join('/')}`);
    }
    for (const dir of [...dirs].sort((a, b) => a.length - b.length)) FS.mkdirTree(dir);
    const nodes = new Map();
    for (const entry of files) {
      const full = `${path}/${entry.path}`, slash = full.lastIndexOf('/');
      const node = FS.createFile(full.slice(0, slash), full.slice(slash + 1), {}, true, false);
      const originalGetattr = node.node_ops.getattr;
      node.node_ops = { ...node.node_ops,
        getattr(current) { return { ...originalGetattr(current), size: entry.size, blocks: Math.ceil(entry.size / 512) }; },
        setattr: denyWrite
      };
      node.stream_ops = { ...node.stream_ops,
        open(stream) { if ((stream.flags & 3) || (stream.flags & (64 | 512 | 1024))) denyWrite(); },
        read(stream, buffer, offset, length, position) {
          if (!ArrayBuffer.isView(buffer) || buffer.BYTES_PER_ELEMENT !== 1 || !integer(offset) || !integer(length) ||
              !integer(position) || offset > buffer.length || length > buffer.length - offset) throw new FS.ErrnoError(28);
          const count = Math.min(length, Math.max(0, entry.size - position));
          if (!count) return 0;
          const bytes = owned.get(entry.path);
          if (bytes) { buffer.set(bytes.subarray(position, position + count), offset); return count; }
          return reader.read(buffer, offset, count, entry.offset + position);
        },
        llseek(stream, offset, whence) {
          if (!Number.isSafeInteger(offset) || ![0, 1, 2].includes(whence)) throw new FS.ErrnoError(28);
          const position = offset + (whence === 1 ? stream.position : whence === 2 ? entry.size : 0);
          if (!integer(position)) throw new FS.ErrnoError(28);
          return position;
        },
        write: denyWrite, allocate: denyWrite, msync: denyWrite,
        mmap() { throw new FS.ErrnoError(43); } // ENODEV: never map empty MEMFS contents
      };
      nodes.set(entry.path, node);
    }
    // Lock directories after population, preventing unlink/rename/replacement.
    for (const dir of dirs) {
      const node = FS.lookupPath(dir).node;
      node.mode = (node.mode & ~0o777) | 0o555;
      node.node_ops = { ...node.node_ops, setattr: denyWrite, mknod: denyWrite,
        rename: denyWrite, unlink: denyWrite, rmdir: denyWrite, symlink: denyWrite };
    }
    return { reader, stats: reader.stats, nodes, path };
  }
  global.MoviesStoredArchiveFS = { mount, validateIndex };
})(globalThis);
