const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
require('../../public/media-reader-core.js');require('../../public/media-service-client.js');require('../../public/media-reader-client.js');
assert.deepEqual(MoviesMediaReaderCore.range(2**31+9,null),{start100ns:2**31+9,end100ns:null});assert.deepEqual(MoviesMediaReaderCore.range(1,0),{start100ns:1,end100ns:1});assert.throws(()=>MoviesMediaReaderCore.range(NaN,null));assert.throws(()=>MoviesMediaReaderCore.range(0,-1));
const header=new Uint8Array(108),headerView=new DataView(header.buffer);header.set(Buffer.from('3026b2758e66cf11a6d900aa0062ce6c','hex'));headerView.setBigUint64(16,108n,true);headerView.setUint32(24,1,true);header.set(Buffer.from('9107dcb7b7a9cf118ee600c00c205365','hex'),30);headerView.setBigUint64(46,78n,true);header.set(Buffer.from('409e69f84d5bcf11a8fd00805f5c442b','hex'),54);headerView.setUint16(102,7,true);assert.equal(MoviesMediaReaderCore.audioStreamNumber(header),7);headerView.setUint16(102,0x8007,true);assert.throws(()=>MoviesMediaReaderCore.audioStreamNumber(header),/Encrypted/);headerView.setUint16(102,7,true);headerView.setBigUint64(46,999999999999999999n,true);assert.throws(()=>MoviesMediaReaderCore.audioStreamNumber(header),/bounds/);
const control=new Int32Array(new SharedArrayBuffer(20)),payload=new Uint8Array(new SharedArrayBuffer(1048576));
class StrictDecoder extends TextDecoder{decode(v,...args){assert(!(v?.buffer instanceof SharedArrayBuffer));return super.decode(v,...args);}}
const ctx=vm.createContext({importScripts(){},MoviesMediaReaderCore,TextEncoder,TextDecoder:StrictDecoder,Uint8Array,Int32Array,SharedArrayBuffer,Atomics,setTimeout,clearTimeout,console,testControl:control,testPayload:payload});
vm.runInContext(fs.readFileSync('public/media-reader-service.js','utf8'),ctx);vm.runInContext('control=testControl;payload=testPayload;initialized=true;',ctx);
async function call(command,bytes=new Uint8Array()){
 const text=new TextEncoder().encode(JSON.stringify(command));payload.set(text);payload.set(bytes,text.length);control[0]=1;control[3]=text.length;control[4]=bytes.length;await ctx.onmessage({data:{op:'request'}});const result=JSON.parse(new TextDecoder().decode(payload.slice(0,control[3])));if(control[2])throw Error(result.error);return result;
}
(async()=>{
 assert.equal((await call({op:'capabilities'})).ready,true);await assert.rejects(call({op:'createReader',size:32*1024*1024+1}),/quota/);
 const token=(await call({op:'createReader',size:16})).token,second=(await call({op:'createReader',size:4})).token;
 await assert.rejects(call({op:'createReader',size:1}),/quota/);await assert.rejects(call({op:'appendInput',token,offset:1},new Uint8Array(4)),/consecutive/);
 assert.equal((await call({op:'appendInput',token,offset:0},new Uint8Array(4))).received,4);await assert.rejects(call({op:'openReader',token}),/incomplete/);
 assert.equal((await call({op:'appendInput',token,offset:4},new Uint8Array(12))).received,16);await assert.rejects(call({op:'appendInput',token,offset:16},new Uint8Array(4)),/declared/);
 assert.equal((await call({op:'releaseReader',token})).released,true);await assert.rejects(call({op:'releaseReader',token}),/Unknown/);await call({op:'releaseReader',token:second});assert.equal(vm.runInContext('totalInput',ctx),0);
 const reader=Object.create(MoviesMediaReaderClient.prototype);reader.request=()=>({eos:true});assert.equal(reader.readerProvider().moviesMediaReadReaderSample(1,4).eos,true);reader.request=()=>{throw Error('actual decode error');};assert.equal(reader.readerProvider().moviesMediaReadReaderSample(1,4),null);assert.match(reader.lastError.message,/actual decode error/);assert.equal(reader.readerProvider().moviesMediaReaderAvailable(),false);
 console.log('Reader state tests passed: safe100ns, input quotas/order/atomicity, lifetime refunds, shared JSON copies, EOS versus error.');
})().catch(e=>{console.error(e);process.exitCode=1});
