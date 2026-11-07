#!/usr/bin/env python3
"""Stage the explicit-gesture PCM sink diagnostic on the local static server."""
from pathlib import Path
import shutil
root = Path(__file__).resolve().parents[2]
dest = root / 'public/runtime/audio-probe'
dest.mkdir(parents=True, exist_ok=True)
for name in ('index.html', 'probe.js'):
    shutil.copyfile(Path(__file__).parent / name, dest / name)
print('http://127.0.0.1:4173/runtime/audio-probe/')
