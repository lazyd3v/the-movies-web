#!/usr/bin/env python3
import array,hashlib,json,subprocess,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];P=ROOT/'.cache/media-probe'
frames=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=pts','-of','json',str(P/'input.wmv')]))['frames']
proc=subprocess.Popen(['ffmpeg','-v','error','-nostdin','-i',str(P/'input.wmv'),'-map','0:v:0','-vsync','0','-pix_fmt','bgr24','-f','rawvideo','pipe:1'],stdout=subprocess.PIPE)
hashes=[]
while data:=proc.stdout.read(921600):
 assert len(data)==921600;hashes.append(hashlib.sha256(data).hexdigest())
assert proc.wait()==0
audio_log=subprocess.run(['ffmpeg','-nostdin','-y','-i',str(P/'input.wmv'),'-map','0:a:0','-af','ashowinfo','-c:a','pcm_s16le','-f','s16le',str(P/'stream-reference.pcm')],check=True,capture_output=True,text=True).stderr
wasm=json.loads((P/'stream-wasm.json').read_text());assert len(frames)==len(hashes)==len(wasm['frames'])
assert all(h==w['sha256'] and f['pts']==w['pts'] for h,f,w in zip(hashes,frames,wasm['frames']))
audio_frames=[{'pts':int(m[0]),'rate':int(m[1]),'samples':int(m[2]),'used':0} for m in re.findall(r'Parsed_ashowinfo_.*? n:\d+ pts:(-?\d+).*?rate:(\d+) nb_samples:(\d+)',audio_log)]
audio_pts_differences=[]
for packet in wasm['audioPackets']:
 for span in packet['spans']:
  frame=audio_frames[0];assert span['timeBase']==[1,frame['rate']]
  audio_pts_differences.append(span['pts']-frame['pts']-frame['used'])
  frame['used']+=span['samples'];assert frame['used']<=frame['samples']
  if frame['used']==frame['samples']:audio_frames.pop(0)
assert not audio_frames
(P/'stream-reference-audio.log').write_text(audio_log)
a=array.array('h',(P/'stream-wasm.pcm').read_bytes());b=array.array('h',(P/'stream-reference.pcm').read_bytes());assert len(a)==len(b)
result={'frames':len(frames),'allFrameHashesEqual':True,'allFramePtsEqual':True,'audioPtsMaxDifferenceSamples':max(abs(x) for x in audio_pts_differences),'audioPtsDifferenceSamples':sorted(set(audio_pts_differences)),'firstFrame':wasm['frames'][0],'lastFrame':wasm['frames'][-1],'audioBytes':len(a)*2,'audioSamplesPerChannel':len(a)//2,'audioMaxAbsoluteS16Difference':max(abs(x-y)for x,y in zip(a,b)),'audioRmsDifference':(sum((x-y)**2 for x,y in zip(a,b))/len(a))**.5}
assert result['audioMaxAbsoluteS16Difference']<=1
(P/'stream-comparison.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
