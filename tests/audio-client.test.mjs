import test from 'node:test';
import assert from 'node:assert/strict';
import {Worker} from 'node:worker_threads';
import {once} from 'node:events';
import {createAudioRing,audioViews,consumeAudioQuantum,AUDIO} from '../public/audio-ring.js';
import {createMoviesAudioClient} from '../public/audio-client.js';
test('guest client copies mono PCM, reports only consumed IDs and can reopen',()=>{
 const descriptor=createAudioRing({capacityFrames:128}),views=audioViews(descriptor),client=createMoviesAudioClient(descriptor);
 const token=client.moviesAudioOpen(1);assert(token);const pcm=new Uint8Array([0,64,0,192]);assert.equal(client.moviesAudioWrite(token,123,pcm),1);pcm.fill(0);
 assert.deepEqual(client.moviesAudioPoll(token),[]);assert.equal(client.moviesAudioClose(token),0);
 const out=[new Float32Array(128),new Float32Array(128)];consumeAudioQuantum(views,out);assert.equal(out[0][0],0.5);assert.equal(out[1][1],-0.5);assert.deepEqual(client.moviesAudioPoll(token),[123]);assert.equal(client.moviesAudioClose(token),1);
 assert(client.moviesAudioOpen(2));client.dispose();assert.equal(client.moviesAudioAvailable(),0);
});
test('synchronous reset waits for independent renderer ACK, timeout fails honestly',async()=>{
 const descriptor=createAudioRing({capacityFrames:128}),client=createMoviesAudioClient(descriptor,{timeoutMs:1000}),token=client.moviesAudioOpen(2);
 Atomics.store(new Int32Array(descriptor.controlBuffer),AUDIO.STATE,AUDIO.PAUSED);client.moviesAudioWrite(token,55,new Uint8Array(512));
 const url=new URL('../public/audio-ring.js',import.meta.url).href;
 const worker=new Worker(new URL('data:text/javascript,'+encodeURIComponent(`import {parentPort,workerData} from 'node:worker_threads';import {audioViews,consumeAudioQuantum} from ${JSON.stringify(url)};const views=audioViews(workerData),out=[new Float32Array(128),new Float32Array(128)];setInterval(()=>consumeAudioQuantum(views,out),2);parentPort.postMessage('ready');`)),{workerData:descriptor});
 try{await once(worker,'message');assert.equal(client.moviesAudioReset(token),1);assert.deepEqual(client.moviesAudioPoll(token),[]);assert.equal(Atomics.load(new Int32Array(descriptor.controlBuffer),AUDIO.CONSUMED),0);assert.equal(client.moviesAudioClose(token),1);}finally{client.dispose();await worker.terminate();}
 const stalled=createMoviesAudioClient(createAudioRing(),{timeoutMs:10}),id=stalled.moviesAudioOpen(2);assert.throws(()=>stalled.moviesAudioReset(id),/timed out/);stalled.dispose();
});
