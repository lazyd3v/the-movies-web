#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,shutil,hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'public/runtime/audio-probe';out.mkdir(parents=True,exist_ok=True)
compiler=root/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'
runtime=root/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
source=root/'scripts/audio-probe/native-fixture.c';api=root/'native/audio-api.c'
subprocess.run([str(compiler),'-O1','-Wall','-Werror','-DMOVIES_AUDIO_SPACE=0x10000','-I'+str(root/'native'),'-I'+str(runtime),str(source),str(api),'-sMODULARIZE=1','-sEXPORT_ES6=1','-sENVIRONMENT=web','-sALLOW_MEMORY_GROWTH=1','-sASSERTIONS=1','-sNO_EXIT_RUNTIME=1','-o',str(out/'native-fixture.js')],check=True)
generated=(out/'native-fixture.js').read_text()
for signature in ('function write_host(token,header,pcm,bytes)', 'function poll_host(token,ids,capacity,count)'):
    if signature not in generated: raise RuntimeError('EM_JS pointer ABI mismatch: '+signature)
for name in ('native.html','native.js'):shutil.copyfile(source.parent/name,out/name)
files=[source,api,root/'native/audio-api.h',out/'native-fixture.js',out/'native-fixture.wasm']
(out/'native-manifest.json').write_text(json.dumps({str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
print('http://127.0.0.1:4173/runtime/audio-probe/native.html')

subprocess.run([str(compiler),'-O1','-Wall','-Werror','-DMOVIES_AUDIO_SPACE=0x10000','-DMOVIES_AUDIO_PRODUCTION=1','-I'+str(root/'native'),'-I'+str(runtime),str(source),str(api),'-sMODULARIZE=1','-sEXPORT_ES6=1','-sENVIRONMENT=worker','-sALLOW_MEMORY_GROWTH=1','-sASSERTIONS=1','-sNO_EXIT_RUNTIME=1','-o',str(out/'production-fixture.js')],check=True)
for name in ('production.html','production.js','production-worker.js'):shutil.copyfile(source.parent/name,out/name)
files += [out/'production-fixture.js',out/'production-fixture.wasm',root/'public/audio-client.js']
(out/'native-manifest.json').write_text(json.dumps({str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n')
print('http://127.0.0.1:4173/runtime/audio-probe/production.html')
