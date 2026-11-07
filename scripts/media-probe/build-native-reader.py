#!/usr/bin/env python3
"""Build native IWMSyncReader acceptance fixture against the real codec service."""
from pathlib import Path
import os
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[2]
DEST=ROOT/'public/runtime/media-probe'

def main():
    if not (DEST/'reader-audio-only.asf').is_file():
        raise SystemExit('Stage the verified audio-only ASF fixture first')
    compiler=Path(os.environ.get('EMCC',ROOT/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'))
    runtime=ROOT/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
    subprocess.run([str(compiler),'-std=c11','-O1','-Wall','-Werror','-pthread',
        '-DMOVIES_MEDIA_SPACE=0x04010000',f'-I{ROOT/"native"}',f'-I{runtime}',
        str(ROOT/'native/media-com.c'),str(ROOT/'tests/test_media_reader_real.c'),
        '-sINITIAL_MEMORY=134217728','-sENVIRONMENT=worker','-sMODULARIZE=1',
        '-sEXPORT_NAME=createNativeMediaReaderProbe','-sINVOKE_RUN=0',
        '-sEXPORTED_RUNTIME_METHODS=callMain','-o',str(DEST/'native-reader-module.js')],check=True,cwd=ROOT)
    for name in ['native-reader.html','native-reader.js','native-reader-worker.js']:
        shutil.copyfile(Path(__file__).parent/name,DEST/name)
    print('Built /runtime/media-probe/native-reader.html (native reader fixture, not game playback)')
if __name__=='__main__':main()
