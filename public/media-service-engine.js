/* Real, bounded WMV2/WMAv2 encode sessions. No WMV3 encoding or implicit profiles. */
(function(scope){
 const PROFILE=Object.freeze({width:640,height:480,fpsNumerator:25,fpsDenominator:1,pixelFormat:'bgr24',videoCodec:'wmv2',videoBitrate:1000000,audioCodec:'wmav2',sampleRate:44100,channels:2,sampleFormat:'s16le',audioBitrate:128000});
 const FRAME=921600,MAX_RAW=32*1024*1024,MAX_OUTPUT=4*1024*1024,MAX_GLOBAL=64*1024*1024;
 function create(core){
  const sessions=new Map();let next=1,total=0,healthy=true;const FS=core.FS;
  function error(code,message){const e=Error(message);e.code=code;throw e;}
  function get(token){const s=sessions.get(token);if(!s)error('INVALID_HANDLE','Unknown writer token');return s;}
  function room(s,bytes){if(bytes>s.budget-s.bytes||bytes>MAX_GLOBAL-total)error('QUOTA','Writer byte budget exceeded before mutation');}
  function unlink(path){try{FS.unlink(path);}catch(e){if(e.errno!==44)throw e;}}
  function response(command,data=new Uint8Array()){
   const {op,token}=command;
   if(op==='capabilities')return {profile:PROFILE,maxFrames:25,maxRawBytes:MAX_RAW,maxOutputBytes:MAX_OUTPUT,maxGlobalBytes:MAX_GLOBAL,wmv3Encoder:false,ready:healthy};
   if(op==='createWriter'){
    if(!healthy)error('BACKEND','Codec service failed; recreate service');
    if(sessions.size>=4)error('QUOTA','At most four live writer sessions');
    const id=next++;if(id>0x7fffffff)error('QUOTA','Writer token space exhausted');
    const dir='/writer-'+id;FS.mkdir(dir);sessions.set(id,{token:id,dir,state:'created',bytes:0,budget:MAX_RAW,frames:0,audioFrames:0,outputBytes:0});return {token:id};
   }
   if(op==='releaseWriter'){
    const s=get(token);for(const f of ['video.raw','audio.raw','output.asf'])unlink(s.dir+'/'+f);FS.rmdir(s.dir);total-=s.bytes+s.outputBytes;sessions.delete(token);return {released:true};
   }
   if(op==='status'){const s=get(token);return {state:s.state,bytes:s.bytes,frames:s.frames,audioSampleFrames:s.audioFrames,outputBytes:s.outputBytes,totalBytes:total};}
   const s=get(token);
   if(op==='configure'){
    if(s.state!=='created')error('STATE','Configure exactly once before samples');
    const p=command.profile;if(!p||Object.keys(p).length!==Object.keys(PROFILE).length||Object.entries(PROFILE).some(([k,v])=>p[k]!==v))error('PROFILE','Only explicit WMV2/BGR24 640x480@25 + WMAv2/S16LE stereo44100 profile is supported');
    const budget=command.byteBudget??MAX_RAW;if(!Number.isSafeInteger(budget)||budget<0||budget>MAX_RAW)error('QUOTA','Invalid raw byte budget');
    FS.writeFile(s.dir+'/video.raw',new Uint8Array());FS.writeFile(s.dir+'/audio.raw',new Uint8Array());s.budget=budget;s.state='configured';return {configured:true,profile:PROFILE};
   }
   if(op==='appendVideo'||op==='appendAudio'){
    if(s.state!=='configured')error('STATE','Writer is not accepting samples');
    const video=op==='appendVideo';
    if(video){if(data.length!==FRAME||command.frameIndex!==s.frames||command.pts100ns!==s.frames*400000||s.frames>=25)error('SAMPLE','Video requires consecutive full frames, exact 25fps PTS, at most25 frames');}
    else if(!data.length||data.length%4||command.sampleOffset!==s.audioFrames||s.audioFrames+data.length/4>44100)error('SAMPLE','Audio requires consecutive interleaved S16LE samples, at most44100 stereo frames');
    room(s,data.length);const path=s.dir+(video?'/video.raw':'/audio.raw'),previous=FS.stat(path).size,stream=FS.open(path,'a');
    try{const n=FS.write(stream,data,0,data.length);if(n!==data.length)error('IO','Short raw sample write');}catch(e){FS.truncate(path,previous);throw e;}finally{FS.close(stream);}
    s.bytes+=data.length;total+=data.length;if(video)s.frames++;else s.audioFrames+=data.length/4;
    return {acceptedBytes:data.length,frames:s.frames,audioSampleFrames:s.audioFrames};
   }
   if(op==='finish'){
    if(!healthy)error('BACKEND','Codec service failed; recreate service');
    if(s.state!=='configured'||!s.frames||s.audioFrames!==s.frames*1764)error('STATE','Finish needs video plus exactly matching 25fps audio duration');
    if(MAX_OUTPUT>MAX_GLOBAL-total)error('QUOTA','No bounded output reservation available');
    const path=s.dir+'/output.asf';FS.writeFile(path,new Uint8Array());const node=FS.lookupPath(path).node,base=node.stream_ops;
    node.stream_ops={...base,write(stream,buffer,offset,length,position,canOwn){if(position+length>MAX_OUTPUT)throw new FS.ErrnoError(51);return base.write(stream,buffer,offset,length,position,canOwn);}};
    s.state='encoding';let log=[];core.setLogger(({message})=>{log.push(message);if(log.length>30)log.shift();});core.reset();core.setTimeout(30000);
    let code;
    try{code=core.exec('-f','rawvideo','-pixel_format','bgr24','-video_size','640x480','-framerate','25','-i',s.dir+'/video.raw','-f','s16le','-ar','44100','-ac','2','-i',s.dir+'/audio.raw','-t',String(s.frames/25),'-c:v','wmv2','-b:v','1000k','-c:a','wmav2','-b:a','128k','-f','asf',path);}catch(e){healthy=false;s.state='failed';unlink(path);throw e;}
    if(code!==0){healthy=false;s.state='failed';unlink(path);error('ENCODE','FFmpeg failed: '+code+' '+log.join('\n'));}
    const size=FS.stat(path).size;if(!size||size>MAX_OUTPUT){healthy=false;s.state='failed';unlink(path);error('ENCODE','Invalid bounded ASF output size');}
    s.outputBytes=size;total+=size;s.state='finished';return {finished:true,outputBytes:size,container:'asf',videoCodec:'wmv2',audioCodec:'wmav2',requestedSeconds:s.frames/25,note:'Encoder delay/padding is retained; not sample-exact container duration.'};
   }
   if(op==='readOutput'){
    if(s.state!=='finished')error('STATE','Output is not ready');const {offset,length}=command;
    if(!Number.isSafeInteger(offset)||!Number.isSafeInteger(length)||offset<0||length<0||length>1040000||offset>s.outputBytes)error('BOUNDS','Invalid output read range');
    const bytes=new Uint8Array(Math.min(length,s.outputBytes-offset)),stream=FS.open(s.dir+'/output.asf','r');try{const n=FS.read(stream,bytes,0,bytes.length,offset);if(n!==bytes.length)error('IO','Short ASF read');}finally{FS.close(stream);}
    return {offset,bytes,eof:offset+bytes.length===s.outputBytes};
   }
   error('OP','Unsupported media-service operation');
  }
  return {request:response};
 }
 scope.MoviesMediaServiceEngine={create,PROFILE};
})(globalThis);
