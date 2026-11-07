/* Chunked Emscripten character devices; one frame accumulator, no output MEMFS. */
(function(scope){
 const WIDTH=640,HEIGHT=480,FRAME_BYTES=WIDTH*HEIGHT*3,AUDIO_PACKET=8192;
 function run(core,input,publish,{mode='both'}={}) {
  if(!['both','video','audio'].includes(mode))throw Error('Invalid stream mode');
  const FS=core.FS,video=new Uint8Array(mode==='audio'?0:FRAME_BYTES),audio=new Uint8Array(mode==='video'?0:AUDIO_PACKET);
  let videoUsed=0,audioUsed=0,frames=0,audioBytes=0;const pts=[],audioPts=[];let timeBase;
  core.setLogger(({message})=>{
   const tb=message.match(/config in time_base: (\d+)\/(\d+)/);if(tb)timeBase=[Number(tb[1]),Number(tb[2])];
   const am=message.includes('Parsed_ashowinfo_') && message.match(/\bn:\s*(\d+)\s+pts:\s*(-?\d+).*rate:(\d+) nb_samples:(\d+)/);
   if(am){if(audioPts.length>=64)throw Error('Audio timestamp queue overflow');audioPts.push({index:Number(am[1]),pts:Number(am[2]),rate:Number(am[3]),samples:Number(am[4]),used:0});}
   const m=message.includes('Parsed_showinfo_') && message.match(/\bn:\s*(\d+)\s+pts:\s*(-?\d+)\s+pts_time:/);
   if(m){if(pts.length>=64)throw Error('Timestamp queue overflow');pts.push({index:Number(m[1]),pts:Number(m[2]),timeBase});}
  });
  function audioTiming(bytes) {
   let remaining=bytes/4;const spans=[];
   while(remaining){const head=audioPts[0];if(!head||head.rate!==44100)throw Error('Missing audio timestamp');const samples=Math.min(remaining,head.samples-head.used);spans.push({pts:head.pts+head.used,timeBase:[1,head.rate],samples});head.used+=samples;remaining-=samples;if(head.used===head.samples)audioPts.shift();}
   return {sampleOffset:audioBytes/4,sampleRate:44100,channels:2,spans};
  }
  function device(path,major,kind,buffer) {
   const dev=FS.makedev(major,0);
   FS.registerDevice(dev,{open:s=>{s.seekable=false;},write(s,source,offset,length){
    let at=0;
    while(at<length){let used=kind==='video'?videoUsed:audioUsed;const take=Math.min(buffer.length-used,length-at);buffer.set(source.subarray(offset+at,offset+at+take),used);used+=take;at+=take;
     if(used===buffer.length){
      if(kind==='video'){const timing=pts.shift();if(!timing||timing.index!==frames||!timing.timeBase)throw Error('Missing or out-of-order video PTS');publish(kind,buffer,timing);frames++;}
      else{publish(kind,buffer,audioTiming(used));audioBytes+=used;}
      used=0;
     }
     if(kind==='video')videoUsed=used;else audioUsed=used;
    }return length;
   }});FS.mkdev(path,0o222,dev);
  }
  if(mode!=='audio')device('/video.bgr',80,'video',video);
  if(mode!=='video')device('/audio.pcm',81,'audio',audio);
  FS.writeFile('/input.wmv',input);core.reset();core.setTimeout(60000);
  const args=['-i','/input.wmv'];
  if(mode!=='audio')args.push('-map','0:v:0','-vf','showinfo','-vsync','0','-pix_fmt','bgr24','-f','rawvideo','/video.bgr');
  if(mode!=='video')args.push('-map','0:a:0','-af','ashowinfo','-acodec','pcm_s16le','-f','s16le','/audio.pcm');
  const code=core.exec(...args);
  if(code!==0)throw Error('Stream decoder exit '+code);
  if(videoUsed||pts.length)throw Error('Incomplete frame/timestamp accounting');
  if(audioUsed){if(audioUsed%4)throw Error('Incomplete PCM sample');publish('audio',audio.subarray(0,audioUsed),audioTiming(audioUsed));audioBytes+=audioUsed;}
  if(audioPts.length)throw Error('Unconsumed audio timestamps');
  return {frames,audioBytes,audioSampleFrames:audioBytes/4,frameBytes:FRAME_BYTES,queueContract:'publish must consume or copy before returning',videoFormat:'BGR24',audioFormat:'S16LE stereo 44100Hz',videoAccumulatorBytes:video.length,audioAccumulatorBytes:audio.length};
 }
 scope.MoviesStreamDecoder={run,FRAME_BYTES};
})(globalThis);
