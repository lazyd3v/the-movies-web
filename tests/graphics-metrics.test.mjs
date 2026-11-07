import test from 'node:test';
import assert from 'node:assert/strict';
import {GraphicsMetrics, emitGraphicsMetrics} from '../public/graphics-metrics.js';
import {createGraphicsPresenter} from '../public/graphics-presentation.js';
test('bounded aggregation discards warmup and resets windows without mutating emitted report',()=>{
 const m=new GraphicsMetrics(['frameMs','bridgeMs'],{frames:2});m.add('bridgeMs',999);assert.equal(m.frame(1,100),null);
 m.add('bridgeMs',3);m.add('bridgeMs',5);assert.equal(m.frame(2,120),null);m.add('bridgeMs',7);const first=m.frame(3,150);
 assert.deepEqual(first,{frames:2,lastSerial:3,values:{frameMs:{count:2,sum:50,max:30},bridgeMs:{count:3,sum:15,max:7}}});
 assert.equal(m.frame(4,175),null);const second=m.frame(5,205);assert.equal(second.values.bridgeMs.sum,0);assert.equal(second.values.frameMs.sum,55);assert.equal(first.values.bridgeMs.sum,15);
 for(let i=6;i<1006;i++)m.frame(i,i+200);assert.equal(Object.keys(m.values).length,2);assert.ok(m.frames<2);
});
test('metrics validates keys and clocks and contains callback failures',()=>{
 const m=new GraphicsMetrics(['frameMs']);assert.throws(()=>m.add('constructor',1));assert.throws(()=>m.add('frameMs',NaN));assert.throws(()=>m.add('frameMs',-1));m.frame(1,5);assert.throws(()=>m.frame(1,6));assert.throws(()=>m.frame(2,4));assert.doesNotThrow(()=>emitGraphicsMetrics(()=>{throw Error('diagnostic failure');},{frames:1}));
});
test('profiled presentation keeps RAF acknowledgement and reports only after120 measured frames',()=>{
 const ack=new Int32Array(new SharedArrayBuffer(8));let callback,consumed=0,closed=0;const reports=[];
 const p=createGraphicsPresenter({ack,profile:true,onMetrics:r=>{assert.equal(Atomics.load(ack,0),r.lastSerial);reports.push(r);throw Error('ignored telemetry failure');},consume:()=>consumed++,requestFrame:fn=>{callback=fn;return 1;},cancelFrame:()=>{}});
 for(let serial=1;serial<=121;serial++){p.submit({serial,bitmap:{close(){closed++;}}});assert.equal(Atomics.load(ack,0),serial-1);callback();assert.equal(Atomics.load(ack,0),serial);assert.equal(Atomics.load(ack,1),1);if(serial<121)assert.equal(reports.length,0);}
 assert.equal(consumed,121);assert.equal(closed,121);assert.equal(reports.length,1);assert.equal(reports[0].values.rafWaitMs.count,120);assert.equal(reports[0].values.consumeMs.count,120);p.close();
});
