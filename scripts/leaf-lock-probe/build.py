from pathlib import Path
import hashlib,json,subprocess,shutil,os
root=Path(__file__).resolve().parents[2];src=Path(__file__).parent;cache=root/'.cache/leaf-lock-probe';out=root/'public/runtime/leaf-lock-probe'
cache.mkdir(parents=True,exist_ok=True);out.mkdir(parents=True,exist_ok=True)
s=(root/'.cache/recomp-full/out/recomp_142.c').read_text();a=s.index('void f_009e9ab0(');b=s.index('\n}',a)+2;body=s[a:b]
expected='5822fa61b9ace4b26931c115b5a6d0a03ded7e52960716fcae4e3e62a09ce8d4'
assert hashlib.sha256(body.encode()).hexdigest()==expected
assert body.count('PREEMPT_POLL(c);')==1 and body.count('return;')==1
body=body.replace('PREEMPT_POLL(c);','PREEMPT_POLL(c);\n    guest_lock();').replace('return;','guest_unlock(); return;')
(cache/'candidate.inc').write_text(body+'\n')
shutil.copyfile(root/'.cache/recomp-full/bootstrap-wasm-o1-threads/runtime/cpu.h',cache/'cpu.h')
em=root/'.cache/runtime-build/emsdk-main/upstream/emscripten';env=os.environ.copy();env['EM_CACHE']=str(root/'.cache/recomp-full/emscripten-cache')
command=[str(em/'emcc'),str(src/'probe.c'),'-I'+str(cache),'-O1','-pthread','-DMOVIES_GUEST_THREADS=1','-DPORT_BOUNDS','-sPTHREAD_POOL_SIZE=2','-sPTHREAD_POOL_SIZE_STRICT=2','-sMODULARIZE=1','-sEXPORT_NAME=createLeafLockProbe','-sENVIRONMENT=worker','-sEXPORTED_RUNTIME_METHODS=["callMain"]','-sINITIAL_MEMORY=33554432','-sSTACK_SIZE=1048576','-sASSERTIONS=1','-sEXIT_RUNTIME=0','-o',str(out/'probe.js')]
subprocess.run(command,env=env,check=True)
js=(out/"probe.js").read_text()
assert "function publish(ptr)" in js and "function block_parent(ptr)" in js, "EM_JS pointer parameter binding changed"
for n in ['page.js','worker.js','index.html']:shutil.copyfile(src/n,out/n)
paths=[src/n for n in ['probe.c','page.js','worker.js','build.py']]+[cache/'cpu.h',cache/'candidate.inc',root/'public/owned-worker-host.js',root/'public/owned-worker-client.js',out/'probe.js',out/'probe.wasm']
manifest={'auditedBodySHA256':expected,'command':command,'hashes':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('http://127.0.0.1:4173/runtime/leaf-lock-probe/')
