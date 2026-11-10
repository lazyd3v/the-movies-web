/* Build and publish the play site to themovies.romanf.cc.
 *
 *   node scripts/deploy-site.mjs [--skip-worker]
 *
 * 1. scripts/build-site.py with error reports to https://themovies.romanf.cc/api/report
 * 2. upload dist/v/<version>/ (files and .gz twins) to the R2 bucket themovies-site
 * 3. upload dist/index.html last, so visitors switch only to a complete release
 * 4. deploy workers/site (serves R2, the report API, isolation and cache headers)
 * Uses the logged-in wrangler (npx wrangler login). */
import {execFileSync, spawn} from 'node:child_process';
import {readdirSync, statSync} from 'node:fs';
import path from 'node:path';

const ROOT = path.resolve(import.meta.dirname, '..'), DIST = path.join(ROOT, 'dist'), BUCKET = 'themovies-site';
const ENDPOINT = 'https://themovies.romanf.cc/api/report';
const run = (cmd, args, opts = {}) => execFileSync(cmd, args, {stdio: 'inherit', cwd: ROOT, ...opts});

run('python3', ['scripts/build-site.py', '--report-endpoint', ENDPOINT]);
const version = readdirSync(path.join(DIST, 'v')).filter(d => !d.startsWith('.')).map(d => ({d, t: statSync(path.join(DIST, 'v', d)).mtimeMs})).sort((a, b) => b.t - a.t)[0].d;
const html = readFileSyncSafe(path.join(DIST, 'index.html'));
if (!html.includes(`/v/${version}/`)) throw new Error(`dist/index.html does not point at ${version}`);

function readFileSyncSafe(file) { return execFileSync('cat', [file]).toString(); }
function walk(dir) { return readdirSync(dir, {withFileTypes: true}).flatMap(e => e.isDirectory() ? walk(path.join(dir, e.name)) : [path.join(dir, e.name)]); }
function put(file) {
  const key = path.relative(DIST, file).split(path.sep).join('/');
  return new Promise((resolve, reject) => {
    const child = spawn('npx', ['wrangler@latest', 'r2', 'object', 'put', `${BUCKET}/${key}`, '--file', file, '--remote'], {cwd: ROOT, stdio: ['ignore', 'ignore', 'pipe']});
    let err = ''; child.stderr.on('data', d => { err += d; });
    child.on('close', code => code === 0 ? resolve(key) : reject(new Error(`${key}: ${err.slice(-500)}`)));
  });
}

const files = walk(path.join(DIST, 'v', version)).sort((a, b) => statSync(b).size - statSync(a).size);
console.log(`Uploading release ${version}: ${files.length} objects`);
let done = 0;
const queue = [...files];
await Promise.all(Array.from({length: 6}, async () => {
  while (queue.length) { const key = await put(queue.shift()); console.log(`  [${++done}/${files.length}] ${key}`); }
}));
await put(path.join(DIST, 'index.html'));
console.log('Uploaded index.html');
if (!process.argv.includes('--skip-worker')) run('npx', ['wrangler@latest', 'deploy'], {cwd: path.join(ROOT, 'workers/site')});
console.log(`Live: https://themovies.romanf.cc/ (release ${version})`);
