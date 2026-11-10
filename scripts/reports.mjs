/* Read crash reports from the collector (workers/crash-reports).
 *
 *   MOVIES_REPORTS_URL=https://themovies.romanf.cc/api MOVIES_REPORTS_TOKEN=$(cat .cache/reports-token) \
 *     node scripts/reports.mjs [summary | list [days] | get <id> | pull [days]]
 *
 * summary  bugs grouped by signature, newest first (default)
 * list     newest reports without logs
 * get      one report with its log
 * pull     save every report of the last N days (default 7) to .cache/reports/<id>.json */
import {mkdirSync, writeFileSync} from 'node:fs';

const base = process.env.MOVIES_REPORTS_URL, token = process.env.MOVIES_REPORTS_TOKEN;
if (!base || !token) { console.error('Set MOVIES_REPORTS_URL and MOVIES_REPORTS_TOKEN'); process.exit(1); }
const get = async path => {
  const response = await fetch(new URL(path.slice(1), base.endsWith('/') ? base : base + '/'), {headers: {Authorization: `Bearer ${token}`}});
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status} ${await response.text()}`);
  return response.json();
};
const when = ms => new Date(ms).toISOString().replace('T', ' ').slice(0, 16);
const [command = 'summary', arg] = process.argv.slice(2);
const since = days => Date.now() - Number(days ?? 7) * 86400_000;

if (command === 'summary') {
  for (const row of await get('/reports/summary')) console.log(`${String(row.count).padStart(5)}  ${row.kind.padEnd(5)}  last ${when(row.last)}  ${row.site_version ?? ''}  ${row.signature}`);
} else if (command === 'list') {
  for (const row of await get(`/reports?limit=200&since=${since(arg)}`)) console.log(`${when(row.created)}  ${row.id}  ${row.kind.padEnd(5)}  ${row.site_version ?? ''}  ${row.signature}`);
} else if (command === 'get' && arg) {
  const row = await get(`/reports/${arg}`);
  const {log_tail, ...rest} = row; console.log(JSON.stringify(rest, null, 2)); console.log('\n' + (log_tail ?? ''));
} else if (command === 'pull') {
  mkdirSync('.cache/reports', {recursive: true});
  const rows = await get(`/reports?limit=200&since=${since(arg)}`);
  for (const row of rows) writeFileSync(`.cache/reports/${row.id}.json`, JSON.stringify(await get(`/reports/${row.id}`), null, 2));
  console.log(`Saved ${rows.length} report(s) to .cache/reports/`);
} else { console.error('Usage: node scripts/reports.mjs [summary | list [days] | get <id> | pull [days]]'); process.exit(1); }
