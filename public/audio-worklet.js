import {audioViews,consumeAudioQuantum,mixDirectSound} from './audio-ring.js';
class MoviesPCMProcessor extends AudioWorkletProcessor{
 constructor(options){super();this.ring=audioViews(options.processorOptions);this.dsound=null;
  this.port.onmessage=({data})=>{if(data?.type==='dsound'&&data.memory instanceof SharedArrayBuffer)this.dsound={samples:new Int16Array(data.memory),control:new Int32Array(data.memory,data.control,data.slots*8),slots:data.slots};};}
 process(inputs,outputs){const alive=consumeAudioQuantum(this.ring,outputs[0]);if(alive&&this.dsound)mixDirectSound(this.dsound,outputs[0]);return alive;}
}
registerProcessor('movies-pcm-sink',MoviesPCMProcessor);
