#!/usr/bin/env python3
"""Bounded wait aggregation and real Sleep/condvar native/Wasm fixtures."""
from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1]
out=root/'.cache/wait-profile-tests';out.mkdir(parents=True,exist_ok=True)
runtime=root/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
env=os.environ.copy();env['EM_CACHE']=str(root/'.cache/recomp-full/emscripten-cache')
for name in ['wait_profile','wait_profile_sync']:
 sources=[root/f'tests/test_{name}.c',root/'native/wait-profile.c']
 if name.endswith('_sync'):sources.append(root/'native/sync-api.c')
 flags=['-O1','-pthread','-DMOVIES_SYNC_SPACE=0x10000','-I'+str(root/'native'),'-I'+str(runtime)]
 binary=out/name
 subprocess.run(['clang',*flags,'-g','-Wall','-Werror','-fsanitize=address,undefined',*map(str,sources),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
 if '--wasm' in sys.argv:
  wasm=out/(name+'.cjs')
  subprocess.run([str(root/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'),*flags,*map(str,sources),'-sPTHREAD_POOL_SIZE=2','-sPTHREAD_POOL_SIZE_STRICT=2','-sEXIT_RUNTIME=1','-o',str(wasm)],env=env,check=True)
  subprocess.run(['node',str(wasm)],check=True,timeout=30)
