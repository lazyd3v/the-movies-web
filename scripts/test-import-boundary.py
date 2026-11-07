#!/usr/bin/env python3
"""Exercise the actual pinned runtime plus production patches, native and Wasm."""
from pathlib import Path
import os, shutil, subprocess
ROOT=Path(__file__).resolve().parents[1]
CACHE=ROOT/'.cache/import-boundary'
SOURCE=ROOT/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
def run(args,**kwargs): subprocess.run([str(x) for x in args],check=True,**kwargs)
def main():
    runtime=CACHE/'runtime';runtime.mkdir(parents=True,exist_ok=True)
    for name in ('runtime.c','runtime.h','cpu.h','simd.h'):shutil.copyfile(SOURCE/name,runtime/name)
    for name in ('retro-port-function-table.patch','retro-port-import-boundary.patch'):
        run(['patch','-p1','-i',ROOT/'patches'/name],cwd=CACHE)
    inputs=[ROOT/'tests/test_import_boundary.c',runtime/'runtime.c']
    run(['clang','-O1','-g','-fsanitize=address,undefined','-pthread','-I'+str(runtime),*inputs,'-o',CACHE/'native-test'])
    run([CACHE/'native-test'])
    (CACHE/'package.json').write_text('{"type":"commonjs"}\n')
    env=os.environ.copy();env['EM_CACHE']=str(ROOT/'.cache/wasm-api-tests/emscripten-cache')
    emcc=ROOT/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'
    run([emcc,'-O1','-sASSERTIONS=1','-sEXIT_RUNTIME=1','-sENVIRONMENT=node','-I'+str(runtime),*inputs,'-o',CACHE/'wasm-test.js'],env=env)
    run(['node',CACHE/'wasm-test.js'])
    run(['clang','-O1','-g','-fsanitize=address,undefined','-pthread','-DTEST_DEFAULT_DISABLED','-I'+str(runtime),*inputs,'-o',CACHE/'native-disabled'])
    run([CACHE/'native-disabled'])
    run([emcc,'-O1','-sASSERTIONS=1','-sEXIT_RUNTIME=1','-sENVIRONMENT=node','-DTEST_DEFAULT_DISABLED','-I'+str(runtime),*inputs,'-o',CACHE/'wasm-disabled.js'],env=env)
    run(['node',CACHE/'wasm-disabled.js'])
if __name__=='__main__':main()
