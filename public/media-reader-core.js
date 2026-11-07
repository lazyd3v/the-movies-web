(function(scope){
 const RATE=44100,CHANNELS=2,PACKET=8192,TICKS=10000000;
 function range(start100ns=0,duration100ns=null){
  if(!Number.isSafeInteger(start100ns)||start100ns<0||start100ns>3600*TICKS||duration100ns!==null&&(!Number.isSafeInteger(duration100ns)||duration100ns<0||duration100ns>3600*TICKS)||duration100ns!==null&&start100ns+duration100ns>3600*TICKS)throw Error('Invalid bounded 100ns range');
  return {start100ns,end100ns:duration100ns===null?null:start100ns+duration100ns};
 }
 function audioStreamInfo(input){
  const view=new DataView(input.buffer,input.byteOffset,input.byteLength),hex=(at)=>Array.from(input.subarray(at,at+16),v=>v.toString(16).padStart(2,'0')).join('');
  if(input.length<30||hex(0)!=='3026b2758e66cf11a6d900aa0062ce6c')throw Error('Invalid ASF header');
  const headerSize=view.getBigUint64(16,true),count=view.getUint32(24,true);if(headerSize>BigInt(input.length)||headerSize<30n||count>4096)throw Error('ASF header bounds');
  const end=Number(headerSize),numbers=new Set(),audio=[];let at=30;
  for(let i=0;i<count;i++){
   if(at+24>end)throw Error('Truncated ASF header object');const size64=view.getBigUint64(at+16,true);if(size64<24n||size64>BigInt(end-at))throw Error('ASF object bounds');const size=Number(size64);
   if(hex(at)==='9107dcb7b7a9cf118ee600c00c205365'){
    if(size<78)throw Error('Truncated ASF stream properties');const flags=view.getUint16(at+72,true),number=flags&0x7f;
    if(!number||numbers.has(number)||78+view.getUint32(at+64,true)+view.getUint32(at+68,true)>size)throw Error('Invalid ASF stream properties');numbers.add(number);
    if(hex(at+24)==='409e69f84d5bcf11a8fd00805f5c442b'){if(flags&0x8000)throw Error('Encrypted ASF audio unsupported');audio.push(number);}
   }at+=size;
  }
  if(at!==end||audio.length!==1)throw Error('Exactly one ordinary ASF audio stream required');return {audioStreamNumber:audio[0],sourceStreamCount:numbers.size};
 }
 function audioStreamNumber(input){return audioStreamInfo(input).audioStreamNumber;}
 function probe(core,input){
  const streamInfo=audioStreamInfo(input);
  core.FS.writeFile('/reader.asf',input);core.setLogger(()=>{});core.reset();core.setTimeout(30000);
  const path='/reader-metadata.json';core.FS.writeFile(path,new Uint8Array());const node=core.FS.lookupPath(path).node,base=node.stream_ops;
  node.stream_ops={...base,write(s,b,o,l,p,c){if(p+l>65536)throw new core.FS.ErrnoError(51);return base.write(s,b,o,l,p,c);}};
  // This core's ffprobe wrapper leaves ret=-1 even on a valid completed probe;
  // require parsed, bounded real metadata rather than fabricate a success code.
  core.ffprobe('-v','error','-show_entries','stream=index,codec_name,codec_type,sample_rate,channels:format=format_name,duration','-of','json','-o',path,'/reader.asf');
  const raw=JSON.parse(core.FS.readFile(path,{encoding:'utf8'})),audio=raw.streams?.find(s=>s.codec_type==='audio'),duration=Number(raw.format?.duration);
  if(raw.format?.format_name!=='asf'||raw.streams?.length!==streamInfo.sourceStreamCount||!audio||audio.codec_name!=='wmav2'||Number(audio.sample_rate)!==RATE||audio.channels!==CHANNELS||!Number.isFinite(duration)||duration<0||duration>3600)throw Error('Unsupported ASF audio format or duration; only WMAv2 stereo44100 is verified');
  return {container:'asf',codec:'wmav2',audioStreamIndex:audio.index,audioStreamNumber:streamInfo.audioStreamNumber,sourceStreamCount:streamInfo.sourceStreamCount,sampleRate:RATE,channels:CHANNELS,bitsPerSample:16,blockAlign:4,format:'S16LE',containerDuration100ns:Math.round(duration*TICKS),maxSampleBytes:PACKET};
 }
 function decode(core,requested,publish){
  const selection=range(requested.start100ns,requested.duration100ns),frames=[];let decoded=0,delivered=0,packets=0;
  core.setLogger(({message})=>{const m=message.includes('Parsed_ashowinfo_')&&message.match(/\bn:\s*(\d+)\s+pts:\s*(-?\d+).*rate:(\d+) nb_samples:(\d+)/);if(m){if(frames.length>=64||Number(m[3])!==RATE)throw Error('Audio timing queue exceeded or format changed');frames.push({pts:Number(m[2]),samples:Number(m[4]),used:0});}});
  const FS=core.FS,dev=FS.makedev(90,0);
  FS.registerDevice(dev,{open:s=>{s.seekable=false;},write(s,buffer,offset,length){
   if(length%4)throw Error('Misaligned PCM write');let at=0;
   while(at<length){const frame=frames[0];if(!frame)throw Error('PCM without source timestamps');const samples=Math.min(PACKET/4,frame.samples-frame.used,(length-at)/4),pts=frame.pts+frame.used;
    const first=Math.max(0,Math.min(samples,Math.ceil((selection.start100ns*RATE-pts*TICKS)/TICKS)));
    const last=selection.end100ns===null?samples:Math.max(0,Math.min(samples,Math.ceil((selection.end100ns*RATE-pts*TICKS)/TICKS)));
    if(last>first){const start=pts+first,end=pts+last;publish(buffer.subarray(offset+at+first*4,offset+at+last*4),{start100ns:Math.round(start*TICKS/RATE),duration100ns:Math.round(end*TICKS/RATE)-Math.round(start*TICKS/RATE),sourcePts:start,timeBase:[1,RATE],sampleFrames:last-first,decodedSampleOffset:decoded+first});delivered+=last-first;packets++;}
    frame.used+=samples;decoded+=samples;at+=samples*4;if(frame.used===frame.samples)frames.shift();
   }return length;
  }});FS.mkdev('/reader.pcm',0o222,dev);core.reset();core.setTimeout(60000);
  const code=core.exec('-loglevel','info','-i','/reader.asf','-map','0:a:0','-af','ashowinfo','-acodec','pcm_s16le','-f','s16le','/reader.pcm');
  if(code!==0||frames.length)throw Error('Audio decode failed or timing accounting incomplete: '+code);
  return {decodedSampleFrames:decoded,deliveredSampleFrames:delivered,packets,eos:true};
 }
 async function load(assetBase){
  const base=new URL(assetBase,location.href);if(base.origin!==location.origin)throw Error('Codec assets must be same-origin');
  async function get(name,expected,limit){const r=await fetch(new URL(name,base));if(!r.ok)throw Error('Codec HTTP '+r.status);const bytes=new Uint8Array(await r.arrayBuffer());if(bytes.length>limit)throw Error('Codec size limit');const hash=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(v=>v.toString(16).padStart(2,'0')).join('');if(hash!==expected)throw Error('Codec integrity mismatch');return bytes;}
  const [js,wasm]=await Promise.all([get('ffmpeg-core.js','b266ab5b952555881dd6310663986994a182acb2b7ff25cf10a25f7a37ac2b21',120000),get('ffmpeg-core.wasm','9f57947a5bd530d8f00c5b3f2cb2a3492faa7e5d823315342d6a8656d0a6b7b7',33000000)]);
  const blob=URL.createObjectURL(new Blob([js],{type:'text/javascript'}));try{importScripts(blob);}finally{URL.revokeObjectURL(blob);}
  const core=await createFFmpegCore({wasmBinary:wasm});let logs=[];core.setLogger(({message})=>logs.push(message));core.exec('-decoders');if(!logs.some(x=>/^\s*A\S*\s+wmav2\s/.test(x)))throw Error('WMAv2 decoder unavailable');return core;
 }
 scope.MoviesMediaReaderCore={load,probe,decode,range,audioStreamNumber,audioStreamInfo};
})(globalThis);
