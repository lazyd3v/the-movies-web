#!/usr/bin/env node
/* Compile every fixed-function shader variant in headless Chrome (real WebGPU/Tint)
 * and report WGSL errors. Usage: node scripts/bench/wgsl-check.mjs (dev server running). */
import {spawn} from 'node:child_process';
import {rmSync} from 'node:fs';
const port = 9334, dir = `/tmp/movies-wgsl-chrome`; rmSync(dir, {recursive: true, force: true});
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`, '--enable-unsafe-webgpu', 'about:blank'], {stdio: 'ignore'});
const sleep = ms => new Promise(r => setTimeout(r, ms));
let version; for (let i = 0; i < 100 && !version; i++) { try { version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch { await sleep(100); } }
const ws = new WebSocket(version.webSocketDebuggerUrl); await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map(); ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id) { pending.get(m.id)?.(m); pending.delete(m.id); } };
const send = (method, params = {}, sessionId) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
const {result: {targetId}} = await send('Target.createTarget', {url: 'http://127.0.0.1:4173/static-probe.html'});
const {result: {sessionId}} = await send('Target.attachToTarget', {targetId, flatten: true});
await sleep(1500);
const expression = `(async()=>{const {shader}=await import('/d3d9-gpu.js');const a=await navigator.gpu.requestAdapter();const d=await a.requestDevice();const out=[];let n=0;
 for(const uv of [false,true])for(const rhw of [false,true])for(const fmt of [22,25])for(const blend of [false,true])for(const [comb,tex] of [[false,false],[true,false],[true,true]])for(const uv2 of [false,true])for(const tex1 of [false,true])for(const lit of [false,true])for(const nd of [false,true]){
  if(lit&&(rhw||!uv2))continue;if(nd&&(!rhw||uv2||lit))continue;if(rhw&&!uv)continue;if(tex&&!uv)continue;if(uv2&&!uv)continue;if(tex1&&(!comb||!uv))continue;
  const m=d.createShaderModule({code:shader(uv,fmt,rhw,blend,false,comb,tex,uv2,tex1,lit,nd)});const info=await m.getCompilationInfo();
  const errs=info.messages.filter(x=>x.type==='error').map(x=>x.lineNum+':'+x.linePos+' '+x.message);if(errs.length)out.push({uv,rhw,fmt,blend,comb,tex,uv2,tex1,lit,errs:errs.slice(0,3)});n++;}
 return JSON.stringify({compiled:n,errors:out})})()`;
const r = await send('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true}, sessionId);
console.log(r.result?.result?.value ?? JSON.stringify(r.result?.exceptionDetails ?? r));
chrome.kill('SIGKILL'); process.exit(0);
