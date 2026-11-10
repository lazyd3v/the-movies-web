/* The player's own game folder as a virtual stored archive.
 *
 * The public site never hosts game files. The page hands the worker the File
 * objects of the folder the player chose; this module lays them end to end as one
 * virtual "archive" so MoviesStoredArchiveFS can mount them unchanged, and serves
 * its range reads synchronously with FileReaderSync (game code reads files
 * synchronously, from a worker). Nothing is copied or uploaded. */
(function (global) {
  'use strict';
  const NO_ARCHIVE_HASH = '0'.repeat(64); // there is no archive file to hash

  /** entries: [{path, file}] relative to the folder holding MoviesSE.exe. */
  function createIndex(entries, executableSha256) {
    if (!Array.isArray(entries) || !entries.length) throw new TypeError('No game files were provided');
    const sorted = [...entries].sort((a, b) => a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
    const files = [], blobs = [];
    let offset = 0;
    for (const {path, file} of sorted) {
      if (typeof path !== 'string' || !file || typeof file.size !== 'number' || typeof file.slice !== 'function') throw new TypeError('Invalid game file entry');
      files.push({path, size: file.size, offset});
      blobs.push({start: offset, end: offset + file.size, file});
      offset += file.size;
    }
    const index = {schemaVersion: 1, archive: 'local-folder', archiveBytes: offset, archiveSha256: NO_ARCHIVE_HASH,
      executable: {path: 'MoviesSE.exe', sha256: executableSha256}, fileCount: files.length, totalBytes: offset, files};
    return {index, request: createRequest(blobs, offset)};
  }

  /** Range request over the files laid end to end: (url, start, end inclusive, size). */
  function createRequest(blobs, size, Reader = global.FileReaderSync) {
    if (typeof Reader !== 'function') throw new Error('FileReaderSync is unavailable in this context');
    const reader = new Reader();
    return (url, start, end, total) => {
      if (total !== size || start < 0 || end >= size || end < start) throw new RangeError('Invalid local game file range');
      const out = new Uint8Array(end - start + 1);
      // Binary search for the first file containing start; then copy across files.
      let lo = 0, hi = blobs.length - 1;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (blobs[mid].end <= start) lo = mid + 1; else hi = mid; }
      let at = start;
      for (let i = lo; at <= end && i < blobs.length; i++) {
        const {start: from, end: to, file} = blobs[i];
        if (to <= at) continue;
        const stop = Math.min(to, end + 1);
        const bytes = new Uint8Array(reader.readAsArrayBuffer(file.slice(at - from, stop - from)));
        if (bytes.length !== stop - at) throw new Error('A game file changed or could not be read; choose the folder again');
        out.set(bytes, at - start);
        at = stop;
      }
      if (at !== end + 1) throw new Error('Local game file range incomplete');
      return out;
    };
  }

  global.MoviesLocalGameFiles = {createIndex, createRequest};
})(globalThis);
