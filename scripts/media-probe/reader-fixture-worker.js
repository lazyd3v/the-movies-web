importScripts('/media-reader-client.js');
onmessage=async()=>{
 let client;
 try{
  const [source,reference,result]=await Promise.all([fetch('input.wmv').then(r=>r.arrayBuffer()),fetch('stream-reference.pcm').then(r=>r.arrayBuffer()),fetch('reader-result.json').then(r=>r.json())]);
  const input=new Uint8Array(source),native=new Int16Array(reference),start=performance.now();client=await MoviesMediaReaderClient.connect();const provider=client.readerProvider();if(!provider.moviesMediaReaderAvailable())throw Error('Decoder unavailable');
  if(provider.moviesMediaCreateReader(32*1024*1024+1)!==0)throw Error('Input quota not enforced');
  const invalid=provider.moviesMediaCreateReader(16);provider.moviesMediaAppendReaderInput(invalid,0,new Uint8Array(16));if(provider.moviesMediaOpenReader(invalid)!==null)throw Error('Malformed ASF accepted');if(provider.moviesMediaReleaseReader(invalid)!==1)throw Error('Malformed-reader cleanup failed');
  const token=provider.moviesMediaCreateReader(input.length);if(!token)throw Error('Reader create failed');
  if(provider.moviesMediaAppendReaderInput(token,4,input.subarray(0,4))!==0)throw Error('Out-of-order input accepted');
  if(provider.moviesMediaOpenReader(token)!==null)throw Error('Incomplete reader opened');
  for(let at=0;at<input.length;at+=65536)if(provider.moviesMediaAppendReaderInput(token,at,input.subarray(at,Math.min(input.length,at+65536)))!==1)throw Error('Input upload failed');
  const metadata=provider.moviesMediaOpenReader(token);if(!metadata)throw provider.lastError;const runs=[];
  for(let index=0;index<result.runs.length;index++){
   const expected=result.runs[index];if(index&&provider.moviesMediaSetReaderRange(token,expected.selection.start100ns,expected.selection.duration100ns)!==1)throw provider.lastError;
   const output=new Uint8Array(expected.bytes);let at=0,packets=0,maxDiff=0,first=null,last=null;
   for(;;){const sample=provider.moviesMediaReadReaderSample(token,4096);if(sample===null)throw provider.lastError;if(sample.eos)break;
    if(!sample.bytes?.length||sample.bytes.length>4096||at+sample.bytes.length>output.length)throw Error('Invalid sample bounds');
    const pcm=new Int16Array(sample.bytes.buffer);for(let i=0;i<pcm.length;i++)maxDiff=Math.max(maxDiff,Math.abs(pcm[i]-native[sample.decodedSampleOffset*2+i]));if(maxDiff>1)throw Error('PCM differs from native reference');
    output.set(sample.bytes,at);at+=sample.bytes.length;packets++;first??=sample.start100ns;last=sample.start100ns+sample.duration100ns;
   }
   const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',output))].map(x=>x.toString(16).padStart(2,'0')).join('');if(at!==expected.bytes||hash!==expected.sha256)throw Error('Range byte/hash mismatch');
   if(expected.first&&(first!==expected.first.start100ns||last!==expected.last.start100ns+expected.last.duration100ns))throw Error('Range boundary timestamp mismatch');
   runs.push({selection:expected.selection,bytes:at,packets,sha256:hash,nativeMaxS16Difference:maxDiff,first100ns:first,lastEnd100ns:last,eos:true});
  }
  if(provider.moviesMediaReleaseReader(token)!==1||provider.moviesMediaReleaseReader(token)!==0)throw Error('Reader lifetime error');
  postMessage({ok:true,metadata,runs,quotaRejected:true,malformedInputRejected:true,incompleteInputRejected:true,outOfOrderInputRejected:true,duplicateReleaseRejected:true,queueBytesPerReader:8192,totalMilliseconds:performance.now()-start,note:'Reader backend only; no game integration. Decoder audio PTS differs from native7.1 by up to10.07ms.'});
 }catch(e){postMessage({ok:false,error:String(e)});}finally{client?.close();}
};
