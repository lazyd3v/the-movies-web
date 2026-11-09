import {createAudioRing,MoviesAudioProducer} from './audio-ring.js';
export async function createMoviesAudioSink({channels=2,capacityFrames=32768,gain=1,onError=()=>{}}={}){
 if(!crossOriginIsolated)throw Error('Audio shared ring requires cross-origin isolation');
 if(navigator.userActivation&&!navigator.userActivation.isActive)throw Error('Start audio through an explicit user gesture');
 if(!Number.isFinite(gain)||gain<0||gain>1)throw Error('Invalid output gain');
 const context=new AudioContext({sampleRate:44100,latencyHint:'interactive'});let node;
 try{
  if(context.sampleRate!==44100)throw Error('Browser did not provide the required44100Hz context');
  const running=context.resume();await Promise.all([running,context.audioWorklet.addModule(new URL('./audio-worklet.js',import.meta.url))]);
  if(context.state!=='running')throw Error('Audio context not running after user activation');
  const descriptor=createAudioRing({channels,capacityFrames}),producer=new MoviesAudioProducer(descriptor);
  node=new AudioWorkletNode(context,'movies-pcm-sink',{numberOfInputs:0,numberOfOutputs:1,outputChannelCount:[channels],processorOptions:descriptor});
  const volume=context.createGain();volume.gain.value=gain;node.connect(volume);volume.connect(context.destination);
  let error=null,closed=false;node.onprocessorerror=()=>{error=Error('AudioWorklet processing failed');producer.close();onError(error);};
  return {context,descriptor,producer,attachDirectSound(memory,control,slots){node.port.postMessage({type:'dsound',memory,control,slots});},get error(){return error;},async close(){if(closed)return [];closed=true;const cancelled=producer.close();node.disconnect();volume.disconnect();await context.close();return cancelled;}};
 }catch(error){node?.disconnect();await context.close();throw error;}
}
