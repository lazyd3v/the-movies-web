#!/usr/bin/env python3
"""Stage the synthetic diagnostic, never original-game rendering claims."""
from pathlib import Path
import shutil
import re
ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / 'public/runtime/graphics-probe'
def main():
    DEST.mkdir(parents=True, exist_ok=True)
    for name in ('index.html', 'probe.js', 'worker.js', 'native-abi.html', 'native-abi-worker.js'):
        shutil.copyfile(Path(__file__).parent / name, DEST / name)
    for name in ('graphics-blend.js', 'graphics-metrics.js', 'd3d9-gpu.js', 'graphics-client.js', 'graphics-worker.js', 'graphics-protocol.js', 'graphics-host.js', 'graphics-presentation.js', 'd3d9-host.js'):
        shutil.copyfile(ROOT / 'public' / name, DEST / name)
    # Fail staging if a newly introduced relative module dependency was omitted.
    for module in DEST.glob('*.js'):
        for relative in re.findall(r"(?:from\s*|import\s*)['\"](\./[^'\"]+)['\"]", module.read_text()):
            # Compiled immediately after staging by build-native-abi.py.
            if relative == './native-abi-module.js':
                continue
            if not (module.parent / relative).is_file():
                raise RuntimeError(f'Missing fixture dependency: {module.name} -> {relative}')
    print('Staged /runtime/graphics-probe/ (synthetic diagnostic, not the game)')
if __name__ == '__main__':
    main()
