#!/usr/bin/env python3
"""Real runtime guest-thread foundation and browser pthread fixture; not gameplay."""
from pathlib import Path
import argparse,os,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]
def run(args,**kw):subprocess.run([str(a) for a in args],check=True,**kw)
def main():
 p=argparse.ArgumentParser();p.add_argument('--browser',action='store_true');args=p.parse_args()
 cache=ROOT/'.cache/guest-thread-tests';rt=cache/'runtime';rt.mkdir(parents=True,exist_ok=True)
 upstream=ROOT/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
 for name in ['runtime.c','runtime.h','cpu.h','simd.h']:shutil.copyfile(upstream/name,rt/name)
 for name in ['retro-port-function-table.patch','retro-port-import-boundary.patch','retro-port-thread-safety.patch']:run(['patch','-p1','-i',ROOT/'patches'/name],cwd=cache)
 flags=['-O1','-g','-pthread','-DMOVIES_GUEST_THREADS=1','-DMOVIES_SYNC_SPACE=0x2000000','-DMOVIES_THREAD_SPACE=0x2000000','-I'+str(rt),'-I'+str(ROOT/'native')]
 sources=[ROOT/'tests/test_guest_thread.c',*[ROOT/'native'/f for f in ['guest-thread-api.c','thread-api.c','sync-api.c']],rt/'runtime.c']
 run(['clang',*flags,'-fsanitize=address,undefined',*sources,'-o',cache/'native']);run([cache/'native'])
 shared=[ROOT/'tests/test_shared_guest_memory.c',ROOT/'native/atomic-api.c',rt/'runtime.c']
 run(['clang',*flags,'-fsanitize=address,undefined',*shared,'-o',cache/'shared-native']);run([cache/'shared-native'])
 env=os.environ.copy();env['EM_CACHE']=str(ROOT/'.cache/wasm-api-tests/emscripten-cache');emcc=ROOT/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'
 wasm=['-sASSERTIONS=1','-sEXIT_RUNTIME=1','-sPTHREAD_POOL_SIZE=2','-sPTHREAD_POOL_SIZE_STRICT=2','-sINITIAL_MEMORY=134217728']
 (cache/'package.json').write_text('{"type":"commonjs"}\n')
 for label,src in [('guest',sources),('shared',shared)]:run([emcc,*flags,*wasm,'-sENVIRONMENT=node',*src,'-o',cache/(label+'.js')],env=env);run(['node',cache/(label+'.js')])
 if args.browser:
  dest=ROOT/'public/runtime/thread-probe';dest.mkdir(parents=True,exist_ok=True)
  run([emcc,*flags,*wasm,'-sENVIRONMENT=worker','-sMODULARIZE=1','-sEXPORT_NAME=createGuestThreadProbe','-sINVOKE_RUN=0','-sEXPORTED_RUNTIME_METHODS=callMain',*sources,'-o',dest/'thread-module.js'],env=env)
  for name in ['index.html','worker.js']:shutil.copyfile(ROOT/'scripts/thread-probe'/name,dest/name)
  print('Browser acceptance: /runtime/thread-probe/index.html')
if __name__=='__main__':main()
