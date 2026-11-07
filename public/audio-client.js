import {AUDIO,MoviesAudioProducer} from './audio-ring.js';
// Runs only on the owning guest worker. No timers or Promise-valued API results.
export function createMoviesAudioClient(descriptor,{timeoutMs=3000}={}){
 const producer=new MoviesAudioProducer(descriptor),control=producer.ring.control;
 let token=0,nextToken=0,channels=0;
 const available=()=>Atomics.load(control,AUDIO.STATE)!==AUDIO.CLOSED;
 const valid=id=>{if(!id||id!==token||!available())throw Error('Audio device unavailable');};
 return {
  moviesAudioAvailable:()=>available()?1:0,
  moviesAudioOpen(ch){if(token||!available()||![1,2].includes(ch))return 0;channels=ch;token=++nextToken;producer.resume();return token;},
  moviesAudioWrite(id,header,bytes){valid(id);if(!(bytes instanceof Uint8Array)||bytes.length%(channels*2))return 0;const input=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);const frames=bytes.length/(channels*2);if(frames>descriptor.capacityFrames)return 0;const pcm=new Int16Array(frames*2);for(let i=0;i<frames;i++){pcm[i*2]=input.getInt16(i*channels*2,true);pcm[i*2+1]=channels===1?pcm[i*2]:input.getInt16(i*4+2,true);}try{producer.write(header,pcm);return 1;}catch(error){if(error.message==='Audio queue full; no samples accepted')return 0;throw error;}},
  moviesAudioPoll(id){valid(id);const result=producer.poll();if(result.buffers.some(b=>b.status!=='played'))throw Error('Unexpected cancelled audio completion');return result.buffers.map(b=>b.id);},
  moviesAudioReset(id){valid(id);const request=producer.requestReset(),deadline=performance.now()+timeoutMs;while(Atomics.load(control,AUDIO.RESET_ACK)!==request){if(!available())throw Error('Audio renderer stopped during reset');const remaining=deadline-performance.now();if(remaining<=0)throw Error('Audio reset acknowledgement timed out');const observed=Atomics.load(control,AUDIO.RESET_ACK);if(observed===request)break;Atomics.wait(control,AUDIO.RESET_ACK,observed,Math.min(remaining,100));}const result=producer.poll();if(!result.resetDone)throw Error('Missing audio reset acknowledgement');return 1;},
  moviesAudioClose(id){valid(id);if(producer.jobs.length)return 0;producer.pause();token=0;return 1;},
  dispose(){producer.close();token=0;}
 };
}
