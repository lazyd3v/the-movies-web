importScripts('/media-service-client.js');
onmessage=async({data})=>{
 let client;
 try{
  const [videoResponse,audioResponse,expectedResponse]=await Promise.all([fetch('service-frame.bgr'),fetch('service-audio.pcm'),fetch('service-result.json')]);
  const video=new Uint8Array(await videoResponse.arrayBuffer()),audio=new Uint8Array(await audioResponse.arrayBuffer()),expected=await expectedResponse.json();
  let missingRejected=false;try{const missing=await MoviesMediaServiceClient.connect({assetBase:'/runtime/media-probe/nonexistent/'});missing.close();}catch(e){missingRejected=true;}if(!missingRejected)throw Error('Missing codec was advertised');
  const start=performance.now();client=await MoviesMediaServiceClient.connect(data.service||{});const provider=client.provider(),token=provider.moviesMediaCreateWriter();if(!token)throw Error('Writer backend unavailable');
  const capabilities=client.request({op:'capabilities'});if(capabilities.wmv3Encoder)throw Error('False WMV3 capability');
  let invalidProfileRejected=false;try{client.request({op:'configure',token,profile:{...capabilities.profile,videoCodec:'wmv3'}});}catch(e){invalidProfileRejected=e.code==='PROFILE';}if(!invalidProfileRejected)throw Error('Invalid profile accepted');
  if(provider.moviesMediaConfigureWriter(token,capabilities.profile)!==1)throw Error('Configure was not acknowledged');
  for(let i=0;i<25;i++)client.request({op:'appendVideo',token,frameIndex:i,pts100ns:i*400000},video);
  for(let offset=0;offset<audio.length;offset+=8192)client.request({op:'appendAudio',token,sampleOffset:offset/4},audio.subarray(offset,Math.min(offset+8192,audio.length)));
  const finished=client.request({op:'finish',token}),output=new Uint8Array(finished.outputBytes);let chunks=0;
  for(let offset=0;offset<output.length;offset+=4096){const part=client.request({op:'readOutput',token,offset,length:4096});output.set(part.bytes,offset);chunks++;}
  const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',output))].map(x=>x.toString(16).padStart(2,'0')).join('');if(hash!==expected.sha256)throw Error('Browser/native-Wasm ASF output mismatch');
  if(provider.moviesMediaReleaseWriter(token)!==1||provider.moviesMediaReleaseWriter(token)!==0)throw Error('Writer lifetime acknowledgement incorrect');
  postMessage({ok:true,...finished,sha256:hash,matchesLocalWasm:true,readChunks:chunks,missingCodecRejected:missingRejected,wmv3ProfileRejected:invalidProfileRejected,duplicateReleaseRejected:true,totalMilliseconds:performance.now()-start,note:'Real synchronous SAB service requests from fixture worker; no bootstrap callbacks installed.'});
 }catch(e){postMessage({ok:false,error:String(e)});}finally{client?.close();}
};
