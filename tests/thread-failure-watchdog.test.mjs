import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../public/static-probe.js',import.meta.url),'utf8').split('// BEGIN THREAD FAILURE WATCHDOG\n')[1].split('// END THREAD FAILURE WATCHDOG')[0];
const context=vm.createContext({SharedArrayBuffer,Int32Array,Uint8Array,TextDecoder,Atomics,Number,Error});vm.runInContext(source,context);
const create=context.createThreadFailureWatch;
const descriptor=(buffer=new SharedArrayBuffer(512))=>({sharedWasmBuffer:buffer,flagOffset:0,errorOffset:16,errorCapacity:256});
function timers(){return {poll:null,clears:0,setInterval(fn){this.poll=fn;return 1;},clearInterval(){this.clears++;}};}
test('watchdog only reports published failures and bounds/copies UTF8, once',()=>{const d=descriptor(),t=timers(),calls=[];const watch=create(d,s=>calls.push(s),t);new Uint8Array(d.sharedWasmBuffer,16,256).set(new TextEncoder().encode('failure é'));for(let i=0;i<10;i++)t.poll();assert.equal(calls.length,0);Atomics.store(new Int32Array(d.sharedWasmBuffer),0,1);t.poll();assert.deepEqual(calls,['failure é']);t.poll();watch.close();assert.equal(calls.length,1);assert.equal(t.clears,1);});
test('invalid descriptors rejected; stop closes watcher without inferring failure',()=>{for(const changes of [{flagOffset:1},{flagOffset:512},{errorOffset:400},{errorCapacity:4097},{errorCapacity:0},{errorOffset:2},{sharedWasmBuffer:new ArrayBuffer(512)}])assert.throws(()=>create({...descriptor(),...changes},()=>{},timers()),/descriptor/);const d=descriptor(),t=timers();const watch=create(d,()=>assert.fail('late failure after stop'),t);watch.close();Atomics.store(new Int32Array(d.sharedWasmBuffer),0,1);t.poll();assert.equal(t.clears,1);});
test('shared Wasm growth leaves initial failure views readable',()=>{const memory=new WebAssembly.Memory({initial:1,maximum:2,shared:true}),d=descriptor(memory.buffer),t=timers(),calls=[];create(d,s=>calls.push(s),t);memory.grow(1);new Uint8Array(memory.buffer,16,256).set(new TextEncoder().encode('after growth'));Atomics.store(new Int32Array(memory.buffer),0,1);t.poll();assert.deepEqual(calls,['after growth']);});
