from pathlib import Path
import subprocess,shutil
root=Path(__file__).resolve().parents[2]
out=root/'public/runtime/pointer-lock-probe';out.mkdir(parents=True,exist_ok=True)
for name in ('index.html','probe.js','worker.js'):shutil.copyfile(Path(__file__).parent/name,out/name)
subprocess.run([str(root/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'),'-O2','-Wall','-Wextra','-Werror','-DMOVIES_WINDOW_SPACE=0x10000','-I'+str(root/'native'),'-I'+str(root/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'),str(Path(__file__).parent/'native.c'),str(root/'native/window-api.c'),str(root/'native/text-api.c'),'--no-entry','-sMODULARIZE=1','-sEXPORT_ES6=1','-sENVIRONMENT=worker','-sEXPORT_NAME=createPointerProbe','-o',str(out/'native.js')],check=True)
print('/runtime/pointer-lock-probe/index.html')
