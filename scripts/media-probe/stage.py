#!/usr/bin/env python3
"""Stage an already integrity-verified official core package and bounded fixture."""
import array, base64, hashlib, json, shutil, re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CACHE=ROOT/'.cache/media-probe'
DEST=ROOT/'public/runtime/media-probe'
INTEGRITY='dzNplnn2Nxle2c2i2rrDhqcB19q9cglCkWnoMTDN9Q9l3PvdjZWd1HfSPjCNWc/p8Q3CT+Es9fWOR0UhAeYQZA=='
assert base64.b64encode(hashlib.sha512((CACHE/'core-0.12.10.tgz').read_bytes()).digest()).decode()==INTEGRITY

def record(path):
 data=path.read_bytes();return {'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}

video=(CACHE/'frame.yuv').read_bytes();ref=(CACHE/'reference.yuv').read_bytes()
a=array.array('h',(CACHE/'audio.pcm').read_bytes());b=array.array('h',(CACHE/'reference.pcm').read_bytes())
assert len(video)==640*480*3//2 and video==ref
assert len(a)==len(b) and len(a)<=44100*2*2
maxdiff=max(abs(x-y) for x,y in zip(a,b));assert maxdiff<=1
comparison={'videoBytesEqual':video==ref,'audioSamples':len(a),'audioDurationSeconds':len(a)/2/44100,'audioMaxAbsoluteS16Difference':maxdiff,'audioRmsDifference':(sum((x-y)**2 for x,y in zip(a,b))/len(a))**.5,'nativeVersion':re.search(r'ffmpeg version ([^\n]+)',(CACHE/'reference.log').read_text()).group(0),'wasmVersion':'FFmpeg 5.1.4 / @ffmpeg/core 0.12.10'}
manifest={'package':{'name':'@ffmpeg/core','version':'0.12.10','license':'GPL-2.0-or-later','url':'https://registry.npmjs.org/@ffmpeg/core/-/core-0.12.10.tgz','sha512Base64':INTEGRITY,**record(CACHE/'core-0.12.10.tgz')},'input':record(CACHE/'input.wmv'),'wasm':json.loads((CACHE/'node-result.json').read_text()),'comparison':comparison,'artifacts':{}}
if (CACHE/'encode-result.json').exists(): manifest['writer']=json.loads((CACHE/'encode-result.json').read_text())
DEST.mkdir(parents=True,exist_ok=True)
for name in ['ffmpeg-core.js','ffmpeg-core.wasm']:
 src=CACHE/'core/package/dist/umd'/name;manifest['artifacts'][name]=record(src);shutil.copyfile(src,DEST/name)
for name in ['index.html','probe.js','worker.js']:shutil.copyfile(ROOT/'scripts/media-probe'/name,DEST/name)
shutil.copyfile(CACHE/'input.wmv',DEST/'input.wmv')
(DEST/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(CACHE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(DEST);print(json.dumps(comparison,indent=2))

if (CACHE/'stream-comparison.json').exists():
 for name in ['stream-adapter.js','stream-worker.js','stream.js','stream.html','timed.js','timed.html']:shutil.copyfile(ROOT/'scripts/media-probe'/name,DEST/name)
 for name in ['stream-wasm.json','stream-reference.pcm','stream-comparison.json']:shutil.copyfile(CACHE/name,DEST/name)

if (CACHE/'service-result.json').exists():
 for name in ['service.html','service-fixture.js','service-fixture-worker.js']:shutil.copyfile(ROOT/'scripts/media-probe'/name,DEST/name)
 for name in ['service-frame.bgr','service-result.json']:shutil.copyfile(CACHE/name,DEST/name)
 (DEST/'service-audio.pcm').write_bytes((CACHE/'stream-wasm.pcm').read_bytes()[:44100*4])

if (CACHE/'reader-result.json').exists():
 for name in ['reader.html','reader-fixture.js','reader-fixture-worker.js']:shutil.copyfile(ROOT/'scripts/media-probe'/name,DEST/name)
 shutil.copyfile(CACHE/'reader-result.json',DEST/'reader-result.json')

if (CACHE/'reader-audio-only.asf').exists():
 for name in ['reader-audio-only.asf','reader-audio-only.json']:shutil.copyfile(CACHE/name,DEST/name)
