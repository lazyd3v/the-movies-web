/* themovies.romanf.cc: the public play site, served from R2, plus the problem-report API.
 *
 * Files come from the SITE bucket exactly as scripts/build-site.py lays them out in
 * dist/ (scripts/deploy-site.mjs uploads them): index.html at the root, and each
 * release under v/<content hash>/ with .gz twins of compressible files.
 * - /v/<hash>/...  immutable: cached at the edge (Cache API) and in browsers for a year.
 * - /              revalidated on every visit; it points at the current release.
 * - /api/report, /api/reports...  the collector in workers/crash-reports (D1).
 * Every response carries the cross-origin isolation headers the game's threads need. */
import reports from '../../crash-reports/src/index.js';

const MIME = {html: 'text/html; charset=utf-8', js: 'text/javascript; charset=utf-8', css: 'text/css; charset=utf-8', json: 'application/json; charset=utf-8',
  wasm: 'application/wasm', jpg: 'image/jpeg', png: 'image/png', webp: 'image/webp', svg: 'image/svg+xml', ico: 'image/x-icon'};
const ISOLATION = {'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin', 'X-Content-Type-Options': 'nosniff'};
const RELEASE = /^\/v\/[0-9a-f]{12}\/[A-Za-z0-9_\-./]+$/;

export function withHeaders(response, extra = {}) {
  const headers = new Headers(response.headers);
  for (const [name, value] of Object.entries({...ISOLATION, ...extra})) headers.set(name, value);
  return new Response(response.body, {status: response.status, statusText: response.statusText, headers, encodeBody: 'manual'});
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname.startsWith('/api/')) {
      const inner = new URL(request.url); inner.pathname = url.pathname.slice('/api'.length);
      return withHeaders(await reports.fetch(new Request(inner, request), env, ctx));
    }
    if (request.method !== 'GET' && request.method !== 'HEAD') return withHeaders(new Response('Method not allowed\n', {status: 405, headers: {Allow: 'GET, HEAD'}}));
    const path = url.pathname === '/' ? '/index.html' : url.pathname;
    const release = RELEASE.test(path);
    if (!release && path !== '/index.html') return withHeaders(new Response('Not found\n', {status: 404}));
    if (path.split('/').some(part => part === '..' || part === '.')) return withHeaders(new Response('Forbidden\n', {status: 403}));
    const gzip = /\bgzip\b/.test(request.headers.get('Accept-Encoding') ?? '');
    // Immutable release files: answer from this colo's cache when possible.
    const cacheKey = release ? new Request(`${url.origin}${path}${gzip ? '#gz' : ''}`) : null;
    if (cacheKey) { const hit = await caches.default.match(cacheKey); if (hit) return request.method === 'HEAD' ? withHeaders(new Response(null, hit)) : withHeaders(hit); }
    const key = path.slice(1), extension = key.split('.').pop().toLowerCase();
    let object = gzip ? await env.SITE.get(`${key}.gz`) : null, encoded = !!object;
    if (!object) object = await env.SITE.get(key);
    if (!object) return withHeaders(new Response('Not found\n', {status: 404}));
    const headers = {'Content-Type': MIME[extension] ?? 'application/octet-stream', 'Vary': 'Accept-Encoding', 'ETag': object.httpEtag,
      'Cache-Control': release ? 'public, max-age=31536000, immutable' : 'no-cache', ...(encoded ? {'Content-Encoding': 'gzip'} : {}),
      'Content-Length': String(object.size)};
    if (!release && request.headers.get('If-None-Match') === object.httpEtag) return withHeaders(new Response(null, {status: 304, headers}));
    const response = new Response(request.method === 'HEAD' ? null : object.body, {headers, encodeBody: 'manual'});
    if (cacheKey && request.method === 'GET') {
      const [forClient, forCache] = response.body.tee();
      ctx.waitUntil(caches.default.put(cacheKey, new Response(forCache, {headers, encodeBody: 'manual'})));
      return withHeaders(new Response(forClient, {headers, encodeBody: 'manual'}));
    }
    return withHeaders(response);
  },
};
