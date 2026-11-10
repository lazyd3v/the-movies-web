/* Problem reports from the public play page to workers/crash-reports (stored in D1).
 * Automatic reports follow the rules below; the Report a problem dialog sends a
 * 'user' report only when the player presses Send.
 *
 * Sent only when all hold: the site was built with a report endpoint
 * (scripts/build-site.py --report-endpoint), the page is not served from a local
 * development host, and the player has not turned reports off. A report holds
 * the stop reason, the tail of the technical log, the build, the browser and the
 * WebGPU adapter's vendor/architecture. No game files and no folder names. */
export const OPT_OUT_KEY = 'movies-error-reports-off';
const LOG_LINES = 400, LOG_CHARS = 150 * 1024;

export function isLocalHost(hostname) {
  const host = String(hostname ?? '').toLowerCase().replace(/^\[|\]$/g, '');
  return !host || host === 'localhost' || host === '::1' || /^127\./.test(host) || host.endsWith('.localhost') || host.endsWith('.test');
}

/** Configured for this deployment (independent of the player's choice). */
export function reportingAvailable(endpoint, hostname) {
  return typeof endpoint === 'string' && /^https:\/\//.test(endpoint) && !isLocalHost(hostname);
}

/** kind 'crash' (automatic, needs reason) or 'user' (Report a problem, needs description). */
export function buildReport({kind = 'crash', reason = '', description = '', contact = '', logText = '', siteVersion = '', build = '', userAgent = '', gpu = '', sessionSeconds = null, frames = null}) {
  const lines = String(logText).split('\n');
  let logTail = logText ? lines.slice(-LOG_LINES).join('\n') : '';
  if (logTail.length > LOG_CHARS) logTail = logTail.slice(-LOG_CHARS);
  return {schema: 1, kind: kind === 'user' ? 'user' : 'crash', reason: String(reason ?? '').slice(0, 2000),
    description: String(description ?? '').slice(0, 4000), contact: String(contact ?? '').slice(0, 200), logTail, siteVersion, build, userAgent, gpu,
    sessionSeconds: Number.isFinite(sessionSeconds) ? Math.round(sessionSeconds) : null, frames: Number.isFinite(frames) ? frames : null};
}

export async function adapterDescription(gpu = globalThis.navigator?.gpu) {
  try {
    const adapter = await gpu?.requestAdapter?.();
    const info = adapter?.info ?? {};
    return [info.vendor, info.architecture].filter(Boolean).join(' ').slice(0, 200);
  } catch { return ''; }
}

/** Returns the report id, or null when it could not be delivered. */
export async function sendReport(endpoint, report, fetchImpl = globalThis.fetch) {
  try {
    const response = await fetchImpl(endpoint, {method: 'POST', mode: 'cors', credentials: 'omit',
      headers: {'Content-Type': 'application/json'}, body: JSON.stringify(report)});
    if (!response.ok) return null;
    return (await response.json())?.id ?? null;
  } catch { return null; }
}
