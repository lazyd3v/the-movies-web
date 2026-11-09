importScripts(new URL('media-reader-core.js',self.MOVIES_DECODER_BASE||location.href).href);
let core,control,slot;
onmessage=async({data})=>{try{
 if(data.op==='initialize'){core=await MoviesMediaReaderCore.load(data.assetBase);postMessage({type:'ready'});}
 else if(data.op==='open'){if(!core)throw Error('Decoder not ready');control=new Int32Array(data.control);slot=new Uint8Array(data.slot);const metadata=MoviesMediaReaderCore.probe(core,new Uint8Array(data.input));postMessage({type:'opened',metadata});}
 else if(data.op==='openVideo'){if(!core)throw Error('Decoder not ready');control=new Int32Array(data.control);const metadata=MoviesMediaReaderCore.probeVideo(core,new Uint8Array(data.input));postMessage({type:'opened',metadata});}
 else if(data.op==='decodeVideo'){
  slot=new Uint8Array(data.slot);
  /* One frame in flight: the consumer clears control[0] once it copied the frame.
   * A paused movie may hold a frame indefinitely; -1 cancels. */
  const stats=MoviesMediaReaderCore.decodeVideo(core,data.metadata,data.start||0,(frame,index)=>{
   if(frame.length>slot.length)throw Error('Frame exceeds queue slot');slot.set(frame);Atomics.store(control,0,1);postMessage({type:'frame',length:frame.length,index});
   while(Atomics.load(control,0)===1)Atomics.wait(control,0,1,15000);if(Atomics.load(control,0)!==0)throw Error('Video reader cancelled');
  });postMessage({type:'eos',stats,wasmHeapBytes:core.HEAPU8.buffer.byteLength});
 }
 else if(data.op==='decode'){
  const stats=MoviesMediaReaderCore.decode(core,data.range,(bytes,timing)=>{
   if(bytes.length>slot.length)throw Error('PCM exceeds queue slot');slot.set(bytes);Atomics.store(control,0,1);postMessage({type:'sample',length:bytes.length,timing});
   while(Atomics.load(control,0)===1){if(Atomics.wait(control,0,1,15000)==='timed-out')throw Error('PCM consumer timeout');}if(Atomics.load(control,0)!==0)throw Error('Reader cancelled');
  });postMessage({type:'eos',stats,wasmHeapBytes:core.HEAPU8.buffer.byteLength});
 }
}catch(e){postMessage({type:'error',error:String(e)});}};
