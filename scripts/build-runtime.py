#!/usr/bin/env python3
"""Build the isolated read-ahead runtime. Requires Python 3.12+, Git, make and clang++.

Downloads are workspace-local. The original public/runtime files are untouched;
only public/runtime/read-ahead is staged after compilation and the cache test.
SDK 5.0.7 is selected from a pinned emsdk bootstrap. SDK/SDL/zlib downloads are
managed by Emscripten; their release identifiers are recorded in build metadata.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
COMMIT = '26bf258ab0dc00aa1ec6da81d1ca263f922f8947'
SDK_COMMIT = 'd7b4130688bfa348392b921ab04015aa15c4f154'
SDK_VERSION = '5.0.7'
SDK_RELEASE = '6cd98e86d7749ff98b82b7f2ae78eb4f01942788'
DOWNLOADS = [
    ('boxedwine-source.tar.gz', f'https://codeload.github.com/danoon2/Boxedwine/tar.gz/{COMMIT}', '85574341623a9c77d5e6c8dbfd58c41ff5a34e369d7c0e8792c15ef1656c63f8'),
    ('emsdk-pinned.tar.gz', f'https://codeload.github.com/emscripten-core/emsdk/tar.gz/{SDK_COMMIT}', '91a27c37ee08094a7bb4322d968526d2106104e74a62ef329c50d5d725edbfb4'),
]


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while data := f.read(1024 * 1024):
            h.update(data)
    return h.hexdigest()


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--chain-limit', type=int, choices=(32, 128), default=32,
                        help='MT JIT chain bound; 128 stages a separate experimental variant')
    parser.add_argument('--warmup-threshold', type=int, choices=(50, 200), default=200,
                        help='JIT promotion threshold; 50 requires chain128 and stages performance')
    parser.add_argument('--stage-only', action='store_true', help='Verify/test and stage an already completed build')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    if args.warmup_threshold == 50 and args.chain_limit != 128:
        parser.error('--warmup-threshold 50 requires --chain-limit 128')
    compile_defines = []
    if args.chain_limit == 128:
        compile_defines.append('-DWASM_JIT_MT_CHAIN_BLOCK_LIMIT=128')
    if args.warmup_threshold == 50:
        compile_defines.append('-DJIT_RUN_COUNT=50')
    cache = ROOT / '.cache/runtime-build'
    cache.mkdir(parents=True, exist_ok=True)
    for name, url, expected in DOWNLOADS:
        path = cache / name
        if not path.exists():
            temporary = path.with_suffix('.download')
            with urllib.request.urlopen(url, timeout=90) as source, temporary.open('wb') as target:
                shutil.copyfileobj(source, target)
            if sha(temporary) != expected:
                temporary.unlink()
                raise ValueError(f'Integrity mismatch for {url}')
            temporary.replace(path)
        if sha(path) != expected:
            raise ValueError(f'Integrity mismatch for {path}')
    source = cache / f'Boxedwine-{COMMIT}'
    sdk = cache / 'emsdk-main'
    if not source.exists():
        with tarfile.open(cache / DOWNLOADS[0][0]) as archive:
            archive.extractall(cache, filter='data')
    if not sdk.exists():
        with tarfile.open(cache / DOWNLOADS[1][0]) as archive:
            archive.extractall(cache, filter='data')
        (cache / f'emsdk-{SDK_COMMIT}').rename(sdk)
    patch = ROOT / 'patches/boxedwine-stored-read-ahead.patch'
    # Upstream mixes CRLF and LF. Normalize just the two patched text files.
    for relative in ('source/io/fszipopennode.h', 'source/io/fszipopennode.cpp'):
        path = source / relative
        content = path.read_text()
        path.write_text(content)
    if not (source / 'source/io/fsstoredreadcache.h').exists():
        run('git', 'apply', str(patch), cwd=source)
    # Check the entire patch, even when resuming a previous build.
    run('git', 'apply', '--reverse', '--check', str(patch), cwd=source)
    test = cache / 'test-stored-read-cache'
    run('clang++', '-std=c++17', '-O2', '-I' + str(source / 'source/io'),
        str(ROOT / 'tests/test_stored_read_cache.cpp'), '-o', str(test))
    run(str(test))
    if not args.stage_only:
        run('python3', str(sdk / 'emsdk.py'), 'install', SDK_VERSION)
        run('python3', str(sdk / 'emsdk.py'), 'activate', SDK_VERSION)
        run('bash', '-c', 'source "$1" >/dev/null 2>&1; cd "$2"; exec make -j"$3" "make_jobs=$3" multiThreadedJit "$4"',
            'build-runtime', str(sdk / 'emsdk_env.sh'), str(source / 'project/emscripten'), str(args.jobs),
            'GCC_EXTRA_FLAGS=' + ' '.join(compile_defines))
    build = source / 'project/emscripten/Build/MultiThreadedJit'
    compile_config = json.loads((build / '.compile-config.json').read_text())
    has_chain128 = '-DWASM_JIT_MT_CHAIN_BLOCK_LIMIT=128' in json.dumps(compile_config)
    if has_chain128 != (args.chain_limit == 128):
        raise ValueError('Built artifact configuration does not match requested chain limit')
    has_warmup50 = '-DJIT_RUN_COUNT=50' in json.dumps(compile_config)
    if has_warmup50 != (args.warmup_threshold == 50):
        raise ValueError('Built artifact configuration does not match requested warmup threshold')
    files = ['boxedwine.js', 'boxedwine.wasm']
    files += [p.name for p in sorted(build.glob('*.worker.js'))]
    for name in files:
        if not (build / name).is_file():
            raise ValueError(f'Missing build artifact: {name}')
        if (build / name).stat().st_mtime < (build / '.compile-config.json').stat().st_mtime:
            raise ValueError(f'Build artifact predates current compiler configuration: {name}')
    with (build / 'boxedwine.wasm').open('rb') as wasm:
        if wasm.read(4) != b'\0asm':
            raise ValueError('Invalid WebAssembly output')
    variant = 'performance' if args.warmup_threshold == 50 else ('chain128' if args.chain_limit == 128 else 'read-ahead')
    output = ROOT / 'public/runtime' / variant
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for name in files:
        target = output / name
        temporary = target.with_suffix(target.suffix + '.tmp')
        shutil.copyfile(build / name, temporary)
        temporary.replace(target)
        records.append({'path': name, 'size': target.stat().st_size, 'sha256': sha(target)})
    manifest = {'sourceCommit': COMMIT, 'sdkVersion': SDK_VERSION,
                'sdkBootstrapCommit': SDK_COMMIT, 'sdkRelease': SDK_RELEASE,
                'patch': patch.relative_to(ROOT).as_posix(), 'patchSha256': sha(patch),
                'target': 'multiThreadedJit', 'readAheadBytes': 262144,
                'chainLimit': args.chain_limit, 'jitWarmup': args.warmup_threshold, 'optimization': '-O2',
                'downloads': [{'url': url, 'sha256': digest} for _, url, digest in DOWNLOADS],
                'files': records}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built and staged isolated runtime: {output}')
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
