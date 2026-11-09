export const AUDIO = Object.freeze({READ:0,WRITE:1,STATE:2,RESET_REQUEST:3,RESET_ACK:4,UNDERRUN:5,RENDERED:6,CONSUMED:7,RESET_READ:8,PAUSED:0,RUNNING:1,CLOSED:2});
export function createAudioRing({channels=2,capacityFrames=32768}={}){
 if(![1,2].includes(channels)||!Number.isInteger(capacityFrames)||capacityFrames<128||capacityFrames>65536||(capacityFrames&(capacityFrames-1)))throw Error('Unsupported bounded audio ring format');
 return {channels,capacityFrames,sampleRate:44100,controlBuffer:new SharedArrayBuffer(9*4),sampleBuffer:new SharedArrayBuffer(capacityFrames*channels*2)};
}
export function audioViews(descriptor){
 const {channels,capacityFrames,sampleRate,controlBuffer,sampleBuffer}=descriptor;
 if(sampleRate!==44100||![1,2].includes(channels)||!Number.isInteger(capacityFrames)||capacityFrames<128||capacityFrames>65536||(capacityFrames&(capacityFrames-1))||!(controlBuffer instanceof SharedArrayBuffer)||controlBuffer.byteLength!==36||!(sampleBuffer instanceof SharedArrayBuffer)||sampleBuffer.byteLength!==capacityFrames*channels*2)throw Error('Invalid audio ring descriptor');
 return {...descriptor,control:new Int32Array(controlBuffer),samples:new Int16Array(sampleBuffer)};
}
// Called only on the rendering thread. No allocations or producer callbacks.
export function consumeAudioQuantum(ring,outputs){
 const c=ring.control;for(const channel of outputs)channel.fill(0);
 const frames=outputs[0]?.length||0;if(!frames)return true;
 const req=Atomics.load(c,AUDIO.RESET_REQUEST);
 if(req!==Atomics.load(c,AUDIO.RESET_ACK)){
  Atomics.store(c,AUDIO.RESET_READ,Atomics.load(c,AUDIO.READ));
  Atomics.store(c,AUDIO.READ,Atomics.load(c,AUDIO.WRITE));
  Atomics.store(c,AUDIO.RESET_ACK,req);Atomics.notify(c,AUDIO.RESET_ACK);return true;
 }
 const state=Atomics.load(c,AUDIO.STATE);if(state===AUDIO.CLOSED)return false;if(state!==AUDIO.RUNNING)return true;
 const read=Atomics.load(c,AUDIO.READ)>>>0,write=Atomics.load(c,AUDIO.WRITE)>>>0,available=(write-read)>>>0;
 if(available>ring.capacityFrames){Atomics.store(c,AUDIO.STATE,AUDIO.CLOSED);return false;}
 const count=Math.min(frames,available),mask=ring.capacityFrames-1;
 for(let frame=0;frame<count;frame++){
  const index=((read+frame)&mask)*ring.channels;
  for(let channel=0;channel<outputs.length;channel++)outputs[channel][frame]=ring.samples[index+(ring.channels===1?0:Math.min(channel,1))]/32768;
 }
 Atomics.add(c,AUDIO.CONSUMED,count);Atomics.add(c,AUDIO.RENDERED,frames);Atomics.add(c,AUDIO.UNDERRUN,frames-count);Atomics.store(c,AUDIO.READ,(read+count)|0);return true;
}
const passed=(position,end)=>((position-end)>>>0)<0x80000000;
export class MoviesAudioProducer{
 constructor(descriptor){this.ring=audioViews(descriptor);this.jobs=[];this.ids=new Set();this.resetRequest=null;this.closed=false;}
 write(id,pcm){
  if(this.closed||Atomics.load(this.ring.control,AUDIO.STATE)===AUDIO.CLOSED)throw Error('Audio sink closed');
  if(this.resetRequest!==null)throw Error('Audio reset acknowledgement pending');
  if(!Number.isSafeInteger(id)||id<0||this.ids.has(id)||!(pcm instanceof Int16Array)||!pcm.length||pcm.length%this.ring.channels)throw Error('Invalid or duplicate PCM buffer');
  const count=pcm.length/this.ring.channels,c=this.ring.control,read=Atomics.load(c,AUDIO.READ)>>>0,write=Atomics.load(c,AUDIO.WRITE)>>>0;
  if(this.jobs.length>=256||count>this.ring.capacityFrames-((write-read)>>>0))throw Error('Audio queue full; no samples accepted');
  for(let frame=0;frame<count;frame++){const target=((write+frame)&(this.ring.capacityFrames-1))*this.ring.channels;for(let ch=0;ch<this.ring.channels;ch++)this.ring.samples[target+ch]=pcm[frame*this.ring.channels+ch];}
  this.jobs.push({id,frames:count,end:(write+count)>>>0});this.ids.add(id);Atomics.store(c,AUDIO.WRITE,(write+count)|0);return {acceptedFrames:count};
 }
 pause(){if(this.closed)throw Error('Audio sink closed');Atomics.store(this.ring.control,AUDIO.STATE,AUDIO.PAUSED);}
 resume(){if(this.closed)throw Error('Audio sink closed');Atomics.store(this.ring.control,AUDIO.STATE,AUDIO.RUNNING);}
 requestReset(){if(this.closed||this.resetRequest!==null)throw Error('Audio reset unavailable');const c=this.ring.control;this.resetRequest=(Atomics.load(c,AUDIO.RESET_REQUEST)+1)|0;Atomics.store(c,AUDIO.RESET_REQUEST,this.resetRequest);return this.resetRequest;}
 poll(){
  const c=this.ring.control,result=[];const resetDone=this.resetRequest!==null&&Atomics.load(c,AUDIO.RESET_ACK)===this.resetRequest;
  if(this.resetRequest!==null&&!resetDone)return {buffers:[],resetDone:false,queuedBuffers:this.jobs.length,consumedFrames:Atomics.load(c,AUDIO.CONSUMED)>>>0,renderedFrames:Atomics.load(c,AUDIO.RENDERED)>>>0,underrunFrames:Atomics.load(c,AUDIO.UNDERRUN)>>>0};
  const read=Atomics.load(c,resetDone?AUDIO.RESET_READ:AUDIO.READ)>>>0;
  while(this.jobs.length&&passed(read,this.jobs[0].end)){const job=this.jobs.shift();this.ids.delete(job.id);result.push({...job,status:'played'});}
  if(resetDone){for(const job of this.jobs){this.ids.delete(job.id);result.push({...job,status:'reset'});}this.jobs=[];this.resetRequest=null;}
  return {buffers:result,resetDone,queuedBuffers:this.jobs.length,consumedFrames:Atomics.load(c,AUDIO.CONSUMED)>>>0,renderedFrames:Atomics.load(c,AUDIO.RENDERED)>>>0,underrunFrames:Atomics.load(c,AUDIO.UNDERRUN)>>>0};
 }
 close(){if(this.closed)return [];const completed=this.poll().buffers;this.closed=true;Atomics.store(this.ring.control,AUDIO.STATE,AUDIO.CLOSED);for(const job of this.jobs)completed.push({...job,status:'closed'});this.jobs=[];this.ids.clear();return completed;}
}
/* DirectSound "hardware": looping secondary buffers living in guest memory (shared
 * Wasm memory). Each slot is 8 Int32: state (1 playing), ring byte address, ring
 * bytes, channels, play cursor frame (rendered here), heartbeat, volume
 * (0..10000 linear), reserved. The guest reads the cursor like a DMA position. */
export const DSOUND_SLOT=Object.freeze({STATE:0,ADDRESS:1,BYTES:2,CHANNELS:3,CURSOR:4,HEARTBEAT:5,VOLUME:6});
export function mixDirectSound(ds,outputs){
 const frames=outputs[0]?.length||0;if(!frames)return;
 for(let slot=0;slot<ds.slots;slot++){
  const base=slot*8,c=ds.control;Atomics.add(c,base+DSOUND_SLOT.HEARTBEAT,1);
  if(Atomics.load(c,base+DSOUND_SLOT.STATE)!==1)continue;
  const address=Atomics.load(c,base+DSOUND_SLOT.ADDRESS)>>>0,bytes=Atomics.load(c,base+DSOUND_SLOT.BYTES)>>>0,channels=Atomics.load(c,base+DSOUND_SLOT.CHANNELS);
  if(!address||(address&1)||![1,2].includes(channels))continue;
  const total=Math.floor(bytes/(2*channels));if(!total||address+bytes>ds.samples.byteLength)continue;
  const gain=Atomics.load(c,base+DSOUND_SLOT.VOLUME)/10000/32768,first=address>>>1;
  let cursor=(Atomics.load(c,base+DSOUND_SLOT.CURSOR)>>>0)%total;
  for(let f=0;f<frames;f++){
   const index=first+cursor*channels,left=ds.samples[index],right=channels===2?ds.samples[index+1]:left;
   outputs[0][f]+=left*gain;if(outputs[1])outputs[1][f]+=right*gain;
   if(++cursor===total)cursor=0;
  }
  Atomics.store(c,base+DSOUND_SLOT.CURSOR,cursor);
 }
}
