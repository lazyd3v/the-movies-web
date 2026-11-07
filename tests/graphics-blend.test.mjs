import test from 'node:test';import assert from 'node:assert/strict';
import {resolveBlend,PipelineCache} from '../public/graphics-blend.js';
test('classic factor mapping uses RGBA semantics, including saturated alpha=one',()=>{
 const expected=[['zero','zero'],['one','one'],['src','src-alpha'],['one-minus-src','one-minus-src-alpha'],['src-alpha','src-alpha'],['one-minus-src-alpha','one-minus-src-alpha'],['dst-alpha','dst-alpha'],['one-minus-dst-alpha','one-minus-dst-alpha'],['dst','dst-alpha'],['one-minus-dst','one-minus-dst-alpha'],['src-alpha-saturated','one']];
 for(let s=1;s<=11;s++)for(let d=1;d<=11;d++){const b=resolveBlend({blend:true,blendFactors:{source:s,destination:d}});assert.deepEqual([b.color.srcFactor,b.alpha.srcFactor],expected[s-1]);assert.deepEqual([b.color.dstFactor,b.alpha.dstFactor],expected[d-1]);}
 assert.equal(resolveBlend({}),null);assert.equal(resolveBlend({blend:true}).source,5);assert.equal(resolveBlend({blend:true,blendMode:'destination-times-source'}).destination,3);
 assert.throws(()=>resolveBlend({blend:true,blendMode:'source-color',blendFactors:{source:1,destination:1}}));
});
test('lazy bounded pipeline cache awaits creation, retains hits, retries failures and evicts LRU',async()=>{
 const evicted=[],cache=new PipelineCache(2,key=>evicted.push(key));let builds=0,release;
 const pending=cache.get('a',()=>{builds++;return new Promise(r=>release=r)});assert.equal(cache.size,0);release('A');assert.equal(await pending,'A');
 assert.equal(await cache.get('a',()=>{throw Error('unexpected rebuild')}),'A');await cache.get('b',async()=> 'B');await cache.get('a',async()=> 'wrong');
 await assert.rejects(cache.get('c',async()=>{throw Error('GPU compile failed')}),/compile/);assert.equal(cache.size,2);assert.deepEqual(evicted,[]);
 await cache.get('c',async()=> 'C');assert.deepEqual(evicted,['b']);assert.equal(cache.size,2);
 await cache.get('b',async()=>{builds++;return 'B2'});assert.deepEqual(evicted,['b','a']);assert.equal(builds,2);cache.clear();assert.equal(cache.size,0);
});
