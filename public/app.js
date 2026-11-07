const $ = id => document.getElementById(id);
const logs = [];
let logChars = 0;
const MAX_LOG_CHARS = 4 * 1024 * 1024;
let ready = true;
let mediaReady = false;
try {
  const res = await fetch('game/manifest.json');
  if (!res.ok) throw new Error('Run npm run prepare:game');
  const game = await res.json();
  $('game-status').textContent = `${game.fileCount} files · ${(game.totalBytes / 1024 ** 3).toFixed(2)} GiB`;
} catch (e) { $('game-status').textContent = e.message; ready = false; }
try {
  const files = ['runtime/boxedwine.wasm', 'runtime/boxedwine.js', 'runtime/boxedwine-shell.js', 'runtime/TinyCore15Wine11.0-web.zip'];
  const responses = await Promise.all(files.map(file => fetch(file, { method: 'HEAD' })));
  if (responses.some(r => !r.ok)) throw new Error('Run python3 scripts/setup-runtime.py');
  $('runtime-status').textContent = 'Boxedwine build 121 · Wine 11 / WebGL';
} catch (e) { $('runtime-status').textContent = e.message; ready = false; }
// The locally built read-ahead variant is optional. A standard runtime setup
// must remain launchable when either of its two files is unavailable.
const readAheadOption = Array.from($('engine').options).find(option => option.value === 'read-ahead');
if (readAheadOption) {
  let available = false;
  try {
    const responses = await Promise.all(['runtime/read-ahead/boxedwine.js', 'runtime/read-ahead/boxedwine.wasm'].map(file => fetch(file, { method: 'HEAD' })));
    available = responses.every(response => response.ok);
  } catch { /* The standard runtime remains the fallback. */ }
  readAheadOption.disabled = !available;
  if (!available && $('engine').value === 'read-ahead') $('engine').value = 'mt-jit';
}
try {
  const response = await fetch('media/manifest.json');
  if (!response.ok) throw new Error('Run python3 scripts/setup-media.py');
  const media = await response.json();
  const archive = await fetch('media/media.zip', {method: 'HEAD'});
  if (!archive.ok) throw new Error('Media overlay is missing');
  $('media-status').textContent = `${media.components.length} native compatibility components`;
  mediaReady = true;
} catch (e) { $('media-status').textContent = e.message; }
const supported = crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined' && typeof WebAssembly !== 'undefined' && !!document.createElement('canvas').getContext('webgl2');
$('browser-status').textContent = supported ? 'WebAssembly, shared memory and WebGL 2 available' : 'Needs WebGL 2 and cross-origin isolation (use npm start)';
ready &&= supported;
$('launch').disabled = !ready || !mediaReady;
$('diagnostic').disabled = !ready;
function launch(diagnostic = false) {
  logs.length = 0;
  logChars = 0;
  $('setup').hidden = true; $('session').hidden = false;
  $('session-status').textContent = diagnostic ? 'Starting Wine / Notepad diagnostic…' : 'Starting original game…';
  const trace = new URLSearchParams(location.search).get('trace');
  $('player').src = 'player.html?engine=' + $('engine').value + ($('strict-memory').checked ? '&safe=1' : '') + (diagnostic ? '&diagnostic=notepad' : '') + (['media', 'graphics'].includes(trace) ? '&trace=' + trace : '');
}
$('launch').onclick = () => launch();
$('diagnostic').onclick = () => launch(true);
$('stop').onclick = () => { $('player').src = 'about:blank'; $('session').hidden = true; $('setup').hidden = false; };
$('fullscreen').onclick = () => $('player').requestFullscreen().catch(error => { $('session-status').textContent = error.message; });
window.addEventListener('message', event => {
  if (event.origin !== location.origin || event.source !== $('player').contentWindow) return;
  if (!['movies-log', 'movies-error', 'movies-status'].includes(event.data?.type)) return;
  if (typeof event.data.text !== 'string') return;
  const text = event.data.text.slice(-MAX_LOG_CHARS);
  logs.push(text); logChars += text.length + 1;
  while ((logChars > MAX_LOG_CHARS || logs.length > 10000) && logs.length > 1) logChars -= logs.shift().length + 1;
  for (const rawLine of text.split('\n')) {
    const line = rawLine.replace(/\x1b\[[?\d;]*[a-zA-Z]/g, '');
    if (line.includes('MEDIA_SETUP_BEGIN')) $('session-status').textContent = 'Preparing Windows Media components (first launch only)…';
    if (line.includes('MEDIA_REGISTER ')) $('session-status').textContent = 'Registering Windows Media: ' + line.split('MEDIA_REGISTER ')[1];
    if (line.includes('The Movies: starting')) $('session-status').textContent = 'Original game process started…';
    if (line.includes('The Movies: exited')) $('session-status').textContent = line;
    if (line.includes('MEDIA_SETUP_FAILED')) $('session-status').textContent = 'Windows Media setup failed — see boot log';
  }
  if (event.data.type === 'movies-error' || event.data.type === 'movies-status') $('session-status').textContent = event.data.text;
});
$('export').onclick = () => {
  const url = URL.createObjectURL(new Blob([logs.join('\n')], { type: 'text/plain' }));
  const link = document.createElement('a'); link.href = url; link.download = 'the-movies-boot.log'; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
};
