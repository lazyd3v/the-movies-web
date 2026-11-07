#!/usr/bin/env python3
"""Build the synthetic native COM→EM_JS→real GPU browser fixture."""
import os
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from stage import main as stage

def main():
    stage()
    compiler = Path(os.environ.get('EMCC', ROOT / '.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'))
    runtime = ROOT / '.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
    output = ROOT / 'public/runtime/graphics-probe/native-abi-module.js'
    command = [str(compiler), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
        '-DMOVIES_HEAP_BASE=0x05040000', '-DMOVIES_HEAP_HI=0x05840000', '-DMOVIES_D3D9_SPACE=0x05900000',
        f'-I{ROOT / "native"}', f'-I{runtime}', str(ROOT / 'native/d3d9-api.c'), str(ROOT / 'native/heap-api.c'),
        str(Path(__file__).parent / 'native-abi.c'), '-sALLOW_MEMORY_GROWTH=1', '-sENVIRONMENT=worker',
        '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=createNativeD3D9Probe', '-sINVOKE_RUN=0',
        '-sEXPORTED_RUNTIME_METHODS=callMain', '-o', str(output)]
    subprocess.run(command, check=True, cwd=ROOT)
    print('Built /runtime/graphics-probe/native-abi.html (synthetic ABI fixture, not the game)')
if __name__ == '__main__':
    main()
