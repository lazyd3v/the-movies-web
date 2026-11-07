from pathlib import Path
import hashlib,json,subprocess,shutil
root=Path(__file__).resolve().parents[2];src=Path(__file__).parent;cache=root/'.cache/thread-fs-probe';out=root/'public/runtime/thread-fs-probe'
cache.mkdir(parents=True,exist_ok=True);out.mkdir(parents=True,exist_ok=True)
index=json.loads((root/'public/game/file-index.json').read_text());entry=next(e for e in index['files'] if e['path']=='Data/Audio/Atmosphere/fac_catering0.lug')
with (root/'public/game'/index['archive']).open('rb') as f:f.seek(entry['offset']);data=f.read(entry['size'])
h=2166136261
for byte in data:h=((h^byte)*16777619)&0xffffffff
em=root/'.cache/runtime-build/emsdk-main/upstream/emscripten'
command=[str(em/'emcc'),str(src/'probe.c'),'-O1','-pthread','-sPTHREAD_POOL_SIZE=1','-sPTHREAD_POOL_SIZE_STRICT=2','-sMODULARIZE=1','-sEXPORT_NAME=createThreadFSProbe','-sENVIRONMENT=worker','-sFORCE_FILESYSTEM=1','-sEXPORTED_RUNTIME_METHODS=["FS","callMain"]','-sINITIAL_MEMORY=33554432','-sSTACK_SIZE=1048576','-sASSERTIONS=1','-sEXIT_RUNTIME=0',f'-DEXPECTED_BYTES={len(data)}',f'-DEXPECTED_FNV={h}u','-o',str(out/'probe.js')]
subprocess.run(command,check=True)
for name in ('worker.js','page.js','index.html'):shutil.copyfile(src/name,out/name)
paths=[src/'probe.c',src/'worker.js',src/'page.js',src/'build.py',out/'worker.js',out/'page.js',root/'public/owned-worker-host.js',root/'public/owned-worker-client.js',root/'public/range-file.js',root/'public/stored-archive-fs.js',em/'system/lib/pthread/library_pthread.c',em/'system/lib/pthread/emscripten_yield.c',em/'src/lib/libcore.js',out/'probe.js',out/'probe.wasm']
manifest={'emscriptenVersion':json.loads((em/'emscripten-version.txt').read_text()),'asset':entry,'assetSHA256':hashlib.sha256(data).hexdigest(),'assetFNV':h,'command':command,'hashes':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
(cache/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');shutil.copyfile(cache/'manifest.json',out/'manifest.json')
print('http://127.0.0.1:4173/runtime/thread-fs-probe/')
