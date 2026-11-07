import {audioViews,consumeAudioQuantum} from './audio-ring.js';
class MoviesPCMProcessor extends AudioWorkletProcessor{
 constructor(options){super();this.ring=audioViews(options.processorOptions);}
 process(inputs,outputs){return consumeAudioQuantum(this.ring,outputs[0]);}
}
registerProcessor('movies-pcm-sink',MoviesPCMProcessor);
