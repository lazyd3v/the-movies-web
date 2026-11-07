#!/usr/bin/env python3
"""Relink a completed, verified Node/Wasm bootstrap as a browser diagnostic."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = '48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a'


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_exe', type=Path)
    parser.add_argument('--optimization', type=int, choices=(0, 1, 2), default=0,
                        help='Stage the matching isolated bootstrap profile')
    parser.add_argument('--threads', action='store_true', help='Stage the isolated guest-pthread profile')
    args = parser.parse_args()
    profile_suffix = f'-o{args.optimization}' if args.optimization else ''
    if args.threads:
        profile_suffix += '-threads'
    build = ROOT / '.cache/recomp-full' / ('bootstrap-wasm' + profile_suffix)
    result_path = build / 'result.json'
    result = json.loads(result_path.read_text())
    if (result.get('target') != 'wasm' or result.get('gameExeSha256') != EXE_SHA
            or not isinstance(result.get('returnCode'), int) or not result.get('startupApisEnabled')):
        parser.error('A completed startup-API Wasm execution result is required')
    if result.get('optimization', 0) != args.optimization:
        parser.error('Completed build optimization profile mismatch')
    if bool(result.get('threaded', False)) != args.threads:
        parser.error('Completed build threading profile mismatch')
    if args.threads and (not {'-pthread', '-DMOVIES_GUEST_THREADS=1'}.issubset(result.get('compileFlags', []))
                        or not {'-pthread', '-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2'}.issubset(result.get('linkFlags', []))
                        or not result.get('threadSafetyPatchSha256')):
        parser.error('Threaded build lacks recorded shared-memory/pool/runtime settings')
    if args.optimization:
        required = {f'-O{args.optimization}', '-frounding-math', '-ffp-contract=off', '-fno-strict-aliasing', '-fwrapv'}
        if not required.issubset(result.get('compileFlags', [])) or f'-O{args.optimization}' not in result.get('linkFlags', []):
            parser.error('Optimized build lacks matching recorded compiler/link settings')
    for name, key in [('bootstrap.cjs', 'executableSha256'), ('bootstrap.wasm', 'wasmSha256')]:
        if digest(build / name) != result.get(key):
            parser.error(f'Completed build artifact mismatch: {name}')
    if digest(args.game_exe) != EXE_SHA:
        parser.error('Original executable checksum mismatch')
    sources = {}
    for name, expected in result.get('hostInputSha256', {}).items():
        if Path(name).name != name or digest(build / 'native' / name) != expected:
            parser.error(f'Build host input mismatch: {name}')
        sources[str((build / 'native' / name).relative_to(ROOT))] = expected
    for name, expected in result['hostSourceSha256'].items():
        path = build / ('runtime' if name == 'runtime.c' else 'native') / name
        if digest(path) != expected:
            parser.error(f'Build source snapshot mismatch: {name}')
        sources[str(path.relative_to(ROOT))] = expected
    for directory in ('native', 'runtime'):
        for path in sorted((build / directory).glob('*.h')):
            sources[str(path.relative_to(ROOT))] = digest(path)
    objects = sorted(build.glob('*.o'))
    expected_objects = {Path(name).stem + '.o' for name in result['hostSourceSha256']}
    translation = json.loads((ROOT / 'docs/research/full-translation-result.json').read_text())
    expected_objects.update(Path(item['file']).stem + '.o' for item in translation['generatedFiles'])
    if {p.name for p in objects} != expected_objects:
        parser.error('Object set differs from completed translation and host sources')
    provenance = {p.name: {'sha256': digest(p), 'compileCacheKey': p.with_suffix('.sha256').read_text()} for p in objects}
    output = ROOT / 'public/runtime' / ('static-probe' + profile_suffix)
    output.parent.mkdir(parents=True, exist_ok=True)
    flags = [f'-O{args.optimization}', '-lm', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=33554432',
             '-sMAXIMUM_MEMORY=2147483648', '-sSTACK_SIZE=16777216',
             '-sENVIRONMENT=worker', '-sMODULARIZE=1', '-sEXPORT_NAME=createMoviesStaticProbe',
             '-sEXIT_RUNTIME=1', '-sFORCE_FILESYSTEM=1', '-sEXPORTED_RUNTIME_METHODS=FS,ENV,callMain']
    if args.threads:
        flags += ['-pthread', '-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2']
    compiler = ROOT / '.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'
    env = dict(os.environ, EM_CACHE=str(ROOT / '.cache/recomp-full/emscripten-cache'))
    with tempfile.TemporaryDirectory(prefix='static-probe-', dir=output.parent) as temporary:
        stage = Path(temporary)
        subprocess.run([str(compiler), *map(str, objects), *flags, '-o', str(stage / 'probe.js')], env=env, check=True)
        if any(digest(p) != provenance[p.name]['sha256'] for p in objects) or json.loads(result_path.read_text()) != result:
            parser.error('Input build changed during link; refusing to publish')
        shutil.copyfile(args.game_exe, stage / 'MoviesSE.exe')
        if digest(stage / 'MoviesSE.exe') != EXE_SHA:
            parser.error('Staged executable checksum mismatch')
        manifest = {'scope': 'Actual original-entry static Wasm diagnostic; not a playable game',
                    'optimization': args.optimization,
                    'threaded': args.threads,
                    'gameExeSha256': EXE_SHA, 'nodeResult': result, 'hostSources': sources,
                    'objects': provenance, 'linkFlags': flags,
                    'compilerSha256': digest(compiler),
                    'artifacts': {p.name: {'sha256': digest(p), 'bytes': p.stat().st_size} for p in stage.iterdir()}}
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        output.mkdir(exist_ok=True)
        for path in stage.iterdir():
            os.replace(path, output / path.name)
    print(json.dumps({'output': str(output), 'objects': len(objects), 'artifacts': manifest['artifacts']}, indent=2))


if __name__ == '__main__':
    main()
