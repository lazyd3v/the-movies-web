#!/usr/bin/env python3
"""Build a synthetic Windows Media COM ABI→real codec-service browser test."""
from pathlib import Path
import os
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'public/runtime/media-probe'

def main():
    if not (DEST / 'manifest.json').is_file():
        raise SystemExit('Stage the verified codec service first: scripts/media-probe/stage.py')
    compiler = Path(os.environ.get('EMCC', ROOT / '.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'))
    runtime = ROOT / '.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
    subprocess.run([str(compiler), '-std=c11', '-O1', '-Wall', '-Werror', '-pthread',
        '-DMOVIES_MEDIA_SPACE=0x04010000', '-DMOVIES_MEDIA_REAL_SERVICE=1',
        f'-I{ROOT / "native"}', f'-I{runtime}', str(ROOT / 'native/media-com.c'),
        str(ROOT / 'tests/test_media_com.c'), '-sINITIAL_MEMORY=134217728',
        '-sPTHREAD_POOL_SIZE=1', '-sENVIRONMENT=worker', '-sMODULARIZE=1',
        '-sEXPORT_NAME=createNativeMediaProbe', '-sINVOKE_RUN=0',
        '-sEXPORTED_RUNTIME_METHODS=callMain', '-o', str(DEST / 'native-abi-module.js')], check=True, cwd=ROOT)
    for name in ['native-abi.html', 'native-abi.js', 'native-abi-worker.js']:
        shutil.copyfile(Path(__file__).parent / name, DEST / name)
    print('Built /runtime/media-probe/native-abi.html (synthetic COM fixture, not game playback)')
if __name__ == '__main__': main()
