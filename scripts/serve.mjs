import http from 'node:http';
import { createReadStream } from 'node:fs';
import { realpath, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.wasm': 'application/wasm', '.zip': 'application/zip', '.png': 'image/png', '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.txt': 'text/plain; charset=utf-8' };
const within = (root, target) => target === root || target.startsWith(root + path.sep);

/** A single HTTP byte range. null means invalid or unsatisfiable. */
export function parseRange(value, size) {
  const match = /^bytes=(\d*)-(\d*)$/.exec(value);
  if (!match || (!match[1] && !match[2]) || size === 0) return null;
  let start, end;
  if (!match[1]) {
    const suffix = Number(match[2]);
    if (!Number.isSafeInteger(suffix) || suffix <= 0) return null;
    start = Math.max(0, size - suffix);
    end = size - 1;
  } else {
    start = Number(match[1]);
    end = match[2] ? Number(match[2]) : size - 1;
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start >= size || end < start) return null;
    end = Math.min(end, size - 1);
  }
  return { start, end };
}

export function createServer({ root = fileURLToPath(new URL('../public/', import.meta.url)) } = {}) {
  const resolvedRoot = realpath(root);
  return http.createServer(async (req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    const fail = (status, message) => {
      res.statusCode = status;
      res.setHeader('Content-Type', 'text/plain; charset=utf-8');
      res.setHeader('Content-Length', Buffer.byteLength(message));
      res.end(req.method === 'HEAD' ? undefined : message);
    };
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.setHeader('Allow', 'GET, HEAD');
      return fail(405, 'Method not allowed\n');
    }
    try {
      // Decode the raw request target before normalization so dot segments cannot
      // be hidden by URL parsing. Backslashes are rejected on every platform.
      let pathname;
      try { pathname = decodeURIComponent((req.url || '/').split('?')[0]); }
      catch { return fail(400, 'Malformed URL\n'); }
      if (!pathname.startsWith('/') || pathname.includes('\0') || pathname.includes('\\') || pathname.split('/').some(part => part === '..')) {
        return fail(403, 'Forbidden\n');
      }
      const base = await resolvedRoot;
      let candidate = path.resolve(base, '.' + pathname);
      if (!within(base, candidate)) return fail(403, 'Forbidden\n');
      candidate = await realpath(candidate);
      if (!within(base, candidate)) return fail(403, 'Forbidden\n');
      let info = await stat(candidate);
      if (info.isDirectory()) {
        candidate = await realpath(path.join(candidate, 'index.html'));
        if (!within(base, candidate)) return fail(403, 'Forbidden\n');
        info = await stat(candidate);
      }
      if (!info.isFile()) return fail(404, 'Not found\n');
      res.setHeader('Accept-Ranges', 'bytes');
      res.setHeader('Content-Type', MIME[path.extname(candidate).toLowerCase()] || 'application/octet-stream');
      res.setHeader('Cache-Control', 'no-cache');
      res.setHeader('Last-Modified', info.mtime.toUTCString());
      let range;
      if (req.headers.range) {
        range = parseRange(req.headers.range, info.size);
        if (!range) {
          res.setHeader('Content-Range', `bytes */${info.size}`);
          return fail(416, 'Range not satisfiable\n');
        }
        res.statusCode = 206;
        res.setHeader('Content-Range', `bytes ${range.start}-${range.end}/${info.size}`);
      }
      res.setHeader('Content-Length', range ? range.end - range.start + 1 : info.size);
      if (req.method === 'HEAD') return res.end();
      const stream = createReadStream(candidate, range || {});
      stream.on('error', () => {
        if (!res.headersSent) fail(500, 'Read error\n');
        else res.destroy();
      });
      res.on('close', () => stream.destroy());
      stream.pipe(res);
    } catch (error) {
      fail(error.code === 'ENOENT' || error.code === 'ENOTDIR' ? 404 : 500, error.code === 'ENOENT' || error.code === 'ENOTDIR' ? 'Not found\n' : 'Server error\n');
    }
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  if (args.length && (args.length !== 2 || args[0] !== '--port' || !/^\d+$/.test(args[1]))) {
    console.error('Usage: node scripts/serve.mjs [--port 4173]');
    process.exit(1);
  }
  const port = args.length ? Number(args[1]) : 4173;
  if (port < 1 || port > 65535) throw new Error('Port must be between 1 and 65535');
  const server = createServer();
  server.on('error', error => { console.error(error.message); process.exitCode = 1; });
  server.listen(port, '127.0.0.1', () => console.log(`Local: http://127.0.0.1:${port}`));
}
