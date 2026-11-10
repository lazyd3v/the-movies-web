/* Problem reports for the public play page, stored in D1 (schema.sql).
 *
 * POST /report            — one JSON report: automatic ("crash", sent when the game
 *                           stops with an error) or written by the player ("user",
 *                           the Report a problem dialog). See public/crash-report.js.
 * GET  /reports           — newest reports without logs (Bearer REPORTS_TOKEN).
 * GET  /reports/summary   — counts grouped by kind and signature (Bearer REPORTS_TOKEN).
 * GET  /reports/<id>      — one full report including its log (Bearer REPORTS_TOKEN).
 *
 * No IP address is stored: a salted, daily-rotating hash of it is kept only to
 * throttle floods (LIMITS.perHour per client). */
export const LIMITS = Object.freeze({body: 256 * 1024, logTail: 160 * 1024, text: 2000, description: 4000, contact: 200, perHour: 20});
const ID = /^[0-9a-f-]{36}$/;

const json = (value, status = 200, headers = {}) =>
  new Response(JSON.stringify(value), {status, headers: {'Content-Type': 'application/json; charset=utf-8', ...headers}});

export default {
  async fetch(request, env) {
    const url = new URL(request.url), origin = request.headers.get('Origin') ?? '';
    const allowed = String(env.ALLOWED_ORIGINS ?? '').split(',').map(s => s.trim()).filter(Boolean);
    const cors = allowed.includes(origin) ? {'Access-Control-Allow-Origin': origin, 'Vary': 'Origin'} : {};
    if (url.pathname === '/report') {
      if (request.method === 'OPTIONS') return new Response(null, {status: 204, headers: {...cors, 'Access-Control-Allow-Methods': 'POST', 'Access-Control-Allow-Headers': 'Content-Type', 'Access-Control-Max-Age': '86400'}});
      if (request.method !== 'POST') return json({error: 'method'}, 405, cors);
      if (!allowed.includes(origin)) return json({error: 'origin'}, 403);
      return receive(request, env, cors);
    }
    if (url.pathname === '/reports' || url.pathname.startsWith('/reports/')) {
      if (request.method !== 'GET') return json({error: 'method'}, 405);
      if (!authorized(request, env)) return json({error: 'unauthorized'}, 401);
      if (url.pathname === '/reports') return list(url, env);
      if (url.pathname === '/reports/summary') return summary(env);
      const id = url.pathname.slice('/reports/'.length);
      if (!ID.test(id)) return json({error: 'id'}, 400);
      const row = await env.DB.prepare('SELECT * FROM reports WHERE id = ?').bind(id).first();
      return row ? json(row) : json({error: 'not found'}, 404);
    }
    return json({error: 'not found'}, 404);
  },
};

/** Validated, size-bounded report fields; throws on malformed input. */
export function normalize(body) {
  if (!body || typeof body !== 'object' || body.schema !== 1) throw new Error('schema');
  const text = (value, max = LIMITS.text) => typeof value === 'string' ? value.slice(0, max) : '';
  const number = value => Number.isFinite(value) ? value : null;
  const kind = body.kind === 'user' ? 'user' : 'crash';
  const reason = text(body.reason), description = text(body.description, LIMITS.description).trim();
  if (kind === 'crash' && !reason) throw new Error('reason');
  if (kind === 'user' && !description) throw new Error('description');
  const logTail = text(body.logTail, LIMITS.logTail);
  return {
    kind, reason, description, contact: text(body.contact, LIMITS.contact).trim(), logTail,
    signature: kind === 'user' ? `user: ${description.split('\n')[0].slice(0, 120)}` : signature(reason, logTail),
    siteVersion: text(body.siteVersion, 80), build: text(body.build, 80), userAgent: text(body.userAgent, 400),
    gpu: text(body.gpu, 200), sessionSeconds: number(body.sessionSeconds), frames: number(body.frames),
  };
}

/** Groups reports of the same bug: the last STOP / unsupported / error line, else the reason. */
export function signature(reason, logTail) {
  const lines = String(logTail).split('\n');
  const line = lines.findLast(l => /^STOP: /.test(l)) ?? lines.findLast(l => /unsupported|failed:|Error:|abort/i.test(l)) ?? reason;
  // Counters and timings differ between runs of the same bug; addresses do not.
  return line.replace(/\b(calls|frames|t|serial|ms)=\d+(\.\d+)?/g, '$1=#').replace(/\d+(\.\d+)? ms\b/g, '# ms').slice(0, 300);
}

async function receive(request, env, cors) {
  const declared = Number(request.headers.get('Content-Length') ?? 0);
  if (declared > LIMITS.body) return json({error: 'too large'}, 413, cors);
  const raw = await request.text();
  if (raw.length > LIMITS.body) return json({error: 'too large'}, 413, cors);
  let report;
  try { report = normalize(JSON.parse(raw)); } catch (error) { return json({error: `invalid: ${error.message}`}, 400, cors); }
  const now = Date.now(), client = await clientHash(request, env, now);
  const recent = await env.DB.prepare('SELECT COUNT(*) AS n FROM reports WHERE client = ? AND created > ?').bind(client, now - 3600_000).first();
  if ((recent?.n ?? 0) >= LIMITS.perHour) return json({error: 'rate limited'}, 429, cors);
  const id = crypto.randomUUID();
  await env.DB.prepare(`INSERT INTO reports (id, created, client, kind, signature, reason, description, contact, site_version, build, user_agent, gpu, country, session_seconds, frames, log_tail)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`).bind(id, now, client, report.kind, report.signature, report.reason, report.description, report.contact,
    report.siteVersion, report.build, report.userAgent, report.gpu, request.cf?.country ?? '', report.sessionSeconds, report.frames, report.logTail).run();
  return json({id}, 201, cors);
}

async function clientHash(request, env, now) {
  const day = Math.floor(now / 86400_000), ip = request.headers.get('CF-Connecting-IP') ?? '';
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(`${env.CLIENT_SALT ?? ''}|${day}|${ip}`));
  return Array.from(new Uint8Array(digest).slice(0, 12), b => b.toString(16).padStart(2, '0')).join('');
}

function authorized(request, env) {
  const expected = `Bearer ${env.REPORTS_TOKEN ?? ''}`, actual = request.headers.get('Authorization') ?? '';
  if (!env.REPORTS_TOKEN || actual.length !== expected.length) return false;
  let difference = 0;
  for (let i = 0; i < expected.length; i++) difference |= expected.charCodeAt(i) ^ actual.charCodeAt(i);
  return difference === 0;
}

const COLUMNS = 'id, created, kind, signature, reason, description, contact, site_version, build, user_agent, gpu, country, session_seconds, frames';

async function list(url, env) {
  const limit = Math.min(200, Math.max(1, Number(url.searchParams.get('limit')) || 50));
  const since = Number(url.searchParams.get('since')) || 0, kind = url.searchParams.get('kind') ?? '', wanted = url.searchParams.get('signature') ?? '';
  const {results} = await env.DB.prepare(`SELECT ${COLUMNS} FROM reports WHERE created > ? AND (? = '' OR kind = ?) AND (? = '' OR signature = ?) ORDER BY created DESC LIMIT ?`)
    .bind(since, kind, kind, wanted, wanted, limit).all();
  return json(results ?? []);
}

async function summary(env) {
  const {results} = await env.DB.prepare('SELECT kind, signature, COUNT(*) AS count, MIN(created) AS first, MAX(created) AS last, MAX(site_version) AS site_version FROM reports GROUP BY kind, signature ORDER BY last DESC LIMIT 200').all();
  return json(results ?? []);
}
