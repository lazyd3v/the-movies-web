/* Public play page: choose the player's own game folder, check it, and start.
 * The game itself is driven by static-probe.js in play mode (body[data-mode=play]);
 * this module only supplies globalThis.moviesGameFiles = [{path, file}] with paths
 * relative to the folder holding MoviesSE.exe. Files never leave the computer.
 * Chrome/Edge keep the folder handle (IndexedDB) so the next visit needs one click;
 * other browsers fall back to a directory <input> each visit. */
const BUILD = 'runtime/static-probe-o1-threads/';
const $ = id => document.getElementById(id);
const chooseButton = $('choose-folder'), reuseButton = $('reuse-folder'), folderInput = $('folder-input');
const folderStatus = $('folder-status'), playButton = $('start'), gameSurface = $('game-surface');
const status = $('status'), loadingText = $('loading-text');

function say(text, kind = '') { folderStatus.textContent = text; folderStatus.className = kind; }

// Requirements: threads need cross-origin isolation; rendering needs WebGPU.
const missing = [];
if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer !== 'function') missing.push('cross-origin isolation (SharedArrayBuffer)');
if (!navigator.gpu) missing.push('WebGPU');
if (missing.length) {
  const warning = $('unsupported');
  warning.textContent = `This browser lacks ${missing.join(' and ')}. Use a current desktop Chrome or Edge.`;
  warning.hidden = false;
}

import {OPT_OUT_KEY, adapterDescription, buildReport, reportingAvailable, sendReport} from './crash-report.js';

const manifest = fetch(new URL('manifest.json', new URL(BUILD, document.baseURI)))
  .then(response => { if (!response.ok) throw new Error(`HTTP ${response.status}`); return response.json(); });
const expected = manifest.then(value => value.gameExeSha256);

async function sha256(file) {
  const digest = await crypto.subtle.digest('SHA-256', await file.arrayBuffer());
  return Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
}

/** entries: [{path: 'Folder/Sub/file', file}] as found under the chosen folder. */
async function accept(entries, name) {
  playButton.disabled = true;
  globalThis.moviesGameFiles = null;
  const exes = entries.filter(e => e.path.split('/').pop().toLowerCase() === 'moviesse.exe')
    .sort((a, b) => a.path.split('/').length - b.path.split('/').length);
  if (!exes.length) { say(`“${name}” has no MoviesSE.exe. Choose the game's install folder (the one with MoviesSE.exe and Data).`, 'error'); return false; }
  const root = exes[0].path.slice(0, exes[0].path.length - 'moviesse.exe'.length);
  const files = [];
  for (const {path, file} of entries) {
    if (!path.startsWith(root)) continue;
    let relative = path.slice(root.length);
    if (relative.split('/').some(part => !part || part.startsWith('.')) || /[\\:\x00-\x1f]/.test(relative)) continue;
    if (relative.toLowerCase() === 'moviesse.exe') relative = 'MoviesSE.exe';
    files.push({path: relative, file});
  }
  const seen = new Set();
  const unique = files.filter(({path}) => { const key = path.toLowerCase(); if (seen.has(key)) return false; seen.add(key); return true; });
  if (!unique.some(({path}) => /^data\/pak\/[^/]+\.pak$/i.test(path))) { say('This folder has MoviesSE.exe but no Data/pak files. Is the game fully installed?', 'error'); return false; }
  say('Checking MoviesSE.exe…');
  let wanted;
  try { wanted = await expected; } catch (error) { say(`Could not load the game build information (${error.message}).`, 'error'); return false; }
  const exe = unique.find(({path}) => path === 'MoviesSE.exe');
  if (await sha256(exe.file) !== wanted) {
    say('This MoviesSE.exe is a different version from the one this port was built for (English 1.2, June 2006). Other versions and languages are not supported yet.', 'error');
    return false;
  }
  const gigabytes = unique.reduce((sum, {file}) => sum + file.size, 0) / 1e9;
  globalThis.moviesGameFiles = unique;
  say(`Ready: “${name}”, ${unique.length} files (${gigabytes.toFixed(1)} GB).`, 'ok');
  playButton.disabled = missing.length > 0;
  return true;
}

// --- Folder access: File System Access API with a remembered handle ---------
const DB = 'movies-play', STORE = 'handles', KEY = 'game-folder';
function database() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}
async function stored(value) {
  const db = await database();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, value === undefined ? 'readonly' : 'readwrite'), store = tx.objectStore(STORE);
    const request = value === undefined ? store.get(KEY) : store.put(value, KEY);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}
async function walk(handle, prefix = '', out = []) {
  for await (const entry of handle.values()) {
    if (entry.name.startsWith('.')) continue;
    const path = prefix + entry.name;
    if (entry.kind === 'directory') await walk(entry, path + '/', out);
    else out.push({path, file: await entry.getFile()});
  }
  return out;
}
async function useHandle(handle) {
  say(`Reading “${handle.name}”…`);
  try { if (await accept(await walk(handle), handle.name)) await stored(handle).catch(() => {}); }
  catch (error) { say(`Could not read the folder: ${error.message}`, 'error'); }
}

if (typeof showDirectoryPicker === 'function') {
  chooseButton.addEventListener('click', async () => {
    let handle;
    try { handle = await showDirectoryPicker({id: 'movies-game', mode: 'read'}); }
    catch (error) { if (error.name !== 'AbortError') say(error.message, 'error'); return; }
    reuseButton.hidden = true;
    await useHandle(handle);
  });
  stored().then(async handle => {
    if (!handle) return;
    const permission = await handle.queryPermission({mode: 'read'});
    if (permission === 'granted') { await useHandle(handle); return; }
    reuseButton.textContent = `Use “${handle.name}” again`;
    reuseButton.hidden = false;
    reuseButton.onclick = async () => {
      if (await handle.requestPermission({mode: 'read'}) !== 'granted') { say('Permission to read the folder was not given.', 'error'); return; }
      reuseButton.hidden = true;
      await useHandle(handle);
    };
  }).catch(() => {});
} else {
  // Fallback: a directory <input> (no persistence between visits).
  chooseButton.addEventListener('click', () => folderInput.click());
  folderInput.addEventListener('change', async () => {
    const entries = [...folderInput.files].map(file => ({path: file.webkitRelativePath || file.name, file}));
    const name = entries[0]?.path.split('/')[0] ?? 'folder';
    say(`Reading “${name}”…`);
    await accept(entries, name);
  });
}

// --- Error reports (production site only; see crash-report.js) -------------
const endpoint = document.body.dataset.reportEndpoint ?? '';
const reportsAvailable = reportingAvailable(endpoint, location.hostname);
const reportToggle = $('send-reports');
if (reportsAvailable) {
  $('report-notice').hidden = false;
  reportToggle.checked = localStorage.getItem(OPT_OUT_KEY) !== '1';
  reportToggle.addEventListener('change', () => reportToggle.checked ? localStorage.removeItem(OPT_OUT_KEY) : localStorage.setItem(OPT_OUT_KEY, '1'));
}
let playStartedAt = 0;
async function details() {
  const build = await manifest.then(value => value.artifacts?.['probe.wasm']?.sha256 ?? '').catch(() => '');
  // frameMetrics is static-probe.js's presentation counter (a classic-script global).
  const frames = typeof frameMetrics === 'object' ? frameMetrics.count : null;
  return {siteVersion: document.body.dataset.siteVersion ?? '', build, userAgent: navigator.userAgent, gpu: await adapterDescription(),
    sessionSeconds: playStartedAt ? (performance.now() - playStartedAt) / 1000 : null, frames};
}
async function reportFailure(reason) {
  if (!reportsAvailable || !reportToggle.checked) return;
  const report = buildReport({kind: 'crash', reason, logText: $('log').textContent, ...await details()});
  const id = await sendReport(endpoint, report);
  if (id) say(`${folderStatus.textContent} An error report was sent (reference ${id.slice(0, 8)}). Thank you.`, 'error');
}

// --- Report a problem dialog -------------------------------------------------
// Sends a 'user' report when the site has a report endpoint; elsewhere (local
// builds) the same report is copied to the clipboard instead.
const reportDialog = $('report-dialog'), reportForm = $('report-form'), reportStatus = $('report-status');
const reportSend = $('report-send'), reportDescription = $('report-description'), reportContact = $('report-contact'), includeLog = $('report-include-log');
reportSend.textContent = reportsAvailable ? 'Send report' : 'Copy report';
async function userReport() {
  const extra = includeLog.checked ? {logText: $('log').textContent, ...await details()} : {siteVersion: document.body.dataset.siteVersion ?? ''};
  return buildReport({kind: 'user', description: reportDescription.value.trim(), contact: reportContact.value.trim(), reason: status.textContent, ...extra});
}
async function refreshPreview() {
  const {logTail, ...rest} = await userReport();
  $('report-preview').textContent = JSON.stringify(rest, null, 2) + (logTail ? `\n\nTechnical log: last ${logTail.split('\n').length} lines` : '');
}
for (const button of document.querySelectorAll('.report-open')) button.addEventListener('click', () => {
  document.exitPointerLock?.();
  reportStatus.textContent = reportsAvailable ? '' : 'Reports are off on this copy of the site; the report will be copied so you can send it yourself.';
  reportStatus.className = ''; reportSend.disabled = false;
  reportDialog.showModal(); reportDescription.focus(); refreshPreview();
});
for (const input of [reportDescription, reportContact, includeLog]) input.addEventListener('input', refreshPreview);
$('report-cancel').addEventListener('click', () => reportDialog.close());
reportForm.addEventListener('submit', async event => {
  event.preventDefault();
  if (!reportDescription.value.trim()) { reportDescription.focus(); return; }
  reportSend.disabled = true;
  const report = await userReport();
  if (!reportsAvailable) {
    try { await navigator.clipboard.writeText(JSON.stringify(report, null, 2)); reportStatus.textContent = 'Copied. Paste it into a message to @romanf_cc.'; reportStatus.className = 'ok'; }
    catch { reportStatus.textContent = 'Could not copy to the clipboard.'; reportStatus.className = 'error'; }
    reportSend.disabled = false; return;
  }
  reportStatus.textContent = 'Sending…'; reportStatus.className = '';
  const id = await sendReport(endpoint, report);
  if (id) {
    reportStatus.textContent = `Sent, thank you! Reference ${id.slice(0, 8)}.`; reportStatus.className = 'ok';
    reportDescription.value = ''; setTimeout(() => { if (reportDialog.open) reportDialog.close(); }, 2500);
  } else { reportStatus.textContent = 'Could not send the report. Please try again in a minute.'; reportStatus.className = 'error'; reportSend.disabled = false; }
});

// --- Playing ----------------------------------------------------------------
// static-probe.js starts the game on the same click; this listener adds fullscreen.
playButton.addEventListener('click', () => {
  if (!globalThis.moviesGameFiles?.length) return;
  playStartedAt = performance.now();
  document.body.classList.add('playing');
  gameSurface.requestFullscreen?.({navigationUI: 'hide'}).catch(() => {});
});
new MutationObserver(() => { loadingText.textContent = status.textContent || 'Loading…'; })
  .observe(status, {childList: true, characterData: true, subtree: true});
addEventListener('movies-game-stopped', ({detail}) => {
  document.body.classList.remove('playing');
  // Quit and the game's own exit are not failures.
  if (detail?.text && !['Stopped by user.', 'The game closed.'].includes(detail.text)) { say(detail.text, 'error'); reportFailure(detail.text); }
  playButton.disabled = !globalThis.moviesGameFiles?.length || missing.length > 0;
});
