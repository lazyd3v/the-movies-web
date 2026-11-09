#!/usr/bin/env python3
"""Run compatibility-layer contract tests with cached runtime headers."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / '.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
OUTPUT = ROOT / '.cache/native-tests'


def main():
    if not (RUNTIME / 'runtime.h').is_file():
        raise SystemExit('Cached retro-port headers missing; see docs/research/recompilation-probe.md')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name in ('startup', 'heap', 'thread', 'process', 'text', 'atomic', 'shell', 'registry', 'file', 'sync', 'time', 'dialog', 'resource', 'window', 'directinput', 'media', 'd3d9', 'audio', 'acm', 'ole', 'dsound', 'movie'):
        binary = OUTPUT / (name + '-api')
        extra = ['-DMOVIES_HEAP_BASE=0x1000', '-DMOVIES_HEAP_HI=0x11000', '-DMOVIES_HEAP_PROFILE=1'] if name == 'heap' else []
        if name == 'audio':
            extra = ['-DMOVIES_AUDIO_SPACE=0x10000']
        if name == 'text':
            extra = ['-DMOVIES_TEXT_SPACE=0x10000']
        if name == 'registry':
            extra = ['-DMOVIES_REGISTRY_SPACE=0x20000']
        if name == 'sync':
            extra = ['-DMOVIES_SYNC_SPACE=0x10000']
        if name == 'process':
            extra = ['-DMOVIES_PROCESS_SPACE=0x2100010']
        if name == 'time':
            extra = ['-DMOVIES_TIME_SPACE=0x10000']
        if name == 'dialog':
            extra = ['-DMOVIES_DIALOG_SPACE=0x20000']
        if name == 'shell':
            extra = ['-DMOVIES_SHELL_SPACE=0x10000']
        if name == 'resource':
            extra = ['-DMOVIES_RESOURCE_SPACE=0x20000']
        if name == 'window':
            extra = ['-DMOVIES_WINDOW_SPACE=0x10000']
        if name == 'directinput':
            extra = ['-DMOVIES_DI_SPACE=0x3010000']
        if name == 'media':
            extra = ['-DMOVIES_MEDIA_SPACE=0x04010000']
        if name == 'ole':
            extra = ['-DMOVIES_OLE_SPACE=0x10000']
        dependencies = [str(ROOT / 'native/text-api.c')] if name in ('registry', 'process', 'dialog', 'window') else []
        if name == 'media':
            dependencies = [str(ROOT / 'native/dsound-api.c'), str(ROOT / 'native/movie-api.c')]
        if name == 'dsound':
            extra = ['-DMOVIES_DSOUND_SPACE=0x04020000']
        if name == 'movie':
            extra = ['-DMOVIES_MOVIE_SPACE=0x05000000']
        if name == 'acm':
            extra = ['-DMOVIES_ACM_SPACE=0x100000', '-DFIXTURE="' + str(ROOT / 'tests/fixtures/sine-440-22k.mp2') + '"']
            dependencies = [str(ROOT / 'native/mp2-decode.c')]
        if name == 'd3d9':
            extra = ['-DMOVIES_HEAP_BASE=0x05040000', '-DMOVIES_HEAP_HI=0x05440000', '-DMOVIES_D3D9_SPACE=0x05500000']
            dependencies = [str(ROOT / 'native/heap-api.c')]
        fixture = ROOT / ('tests/test_media_com.c' if name == 'media' else f'tests/test_{name}_api.c')
        source = ROOT / ('native/media-com.c' if name == 'media' else f'native/{name}-api.c')
        subprocess.run(['clang', '-O2', '-Wall', '-Werror', '-pthread', *extra,
                        '-I' + str(ROOT / 'native'), '-I' + str(RUNTIME),
                        str(fixture), str(source), *dependencies,
                        '-o', str(binary), '-lm'], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == '__main__':
    main()
