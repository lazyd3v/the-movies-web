#!/usr/bin/env python3
"""Build/run a strict native diagnostic from translated game code (not a port).

Original game and generated files remain in ignored directories. The native
diagnostic helps isolate translation errors before adding browser integration.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = '48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a'
RUNTIME_HASHES = {
    'runtime.c': '4f7dc0c579a486b9a8c3ac87a271c28ff03dcfd311588838c6929da807ef6cbb',
    'runtime.h': 'd72c72b8691b6fd6ec0df7bf610a7203ab41694f65fb7a3d573e849c0f68ad22',
    'cpu.h': '0ffb3dfda1c641a5341fcb01687893e00a002928bb338745881abb0cf3fcb36c',
    'simd.h': '6482743d4944b51d117fa61ce18b9e28117bab95baeaa46e05b9ba2d78457de4',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_exe', type=Path)
    parser.add_argument('--generated', type=Path, default=ROOT / '.cache/recomp-full/out')
    parser.add_argument('--manifest', type=Path, default=ROOT / 'docs/research/full-translation-result.json')
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--optimization', type=int, choices=(0, 1, 2), default=0,
                        help='Isolated compiler/link profile; 0 preserves existing cache paths')
    parser.add_argument('--timeout', type=int, default=30)
    parser.add_argument('--startup-apis', action='store_true', help='Enable the tested, narrow startup API implementations')
    parser.add_argument('--trace-imports', action='store_true', help='Log every Windows API call (very verbose and slow)')
    parser.add_argument('--threads', action='store_true', help='Isolated experimental guest-pthread profile')
    parser.add_argument('--target', choices=('native', 'wasm'), default='native')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 4 or args.timeout < 1:
        parser.error('jobs must be 1..4; timeout must be positive')
    if hashlib.sha256(args.game_exe.read_bytes()).hexdigest() != EXE_SHA:
        parser.error('Original executable SHA-256 does not match audited version')
    generated = args.generated.resolve()
    manifest = json.loads(args.manifest.read_text())
    if manifest.get('gameExeSha256') != EXE_SHA or manifest.get('status') not in ('generated_with_blockers', 'generated_unvalidated'):
        parser.error('A completed translation manifest for this executable is required')
    records = manifest['generatedFiles'] + [manifest['generatedHeader']]
    if {record['file'] for record in manifest['generatedFiles']} != {p.name for p in generated.glob('recomp_*.c')}:
        parser.error('Generated source set differs from the translation manifest')
    for record in records:
        if Path(record['file']).name != record['file']:
            parser.error('Invalid generated source path in manifest')
        if hashlib.sha256((generated / record['file']).read_bytes()).hexdigest() != record['sha256']:
            parser.error(f'Generated source checksum mismatch: {record["file"]}')
    report = json.loads((generated / 'translate_report.json').read_text())
    registration_count = (generated / 'recomp_stubs.c').read_text().count('register_function(')
    runtime = ROOT / '.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime'
    for name, digest in RUNTIME_HASHES.items():
        if hashlib.sha256((runtime / name).read_bytes()).hexdigest() != digest:
            parser.error(f'Pinned runtime checksum mismatch: {name}')
    profile_suffix = f'-o{args.optimization}' if args.optimization else ''
    if args.threads:
        profile_suffix += '-threads'
    build = ROOT / '.cache/recomp-full' / (('bootstrap' if args.target == 'native' else 'bootstrap-wasm') + profile_suffix)
    build.mkdir(parents=True, exist_ok=True)
    (build / 'result.json').unlink(missing_ok=True)
    # Migrate existing core objects from the old overly broad dependency hash.
    # Their C only includes runtime/generated headers, never host API headers.
    try:
        legacy_headers = b''.join((build / 'runtime' / name).read_bytes() for name in ('runtime.h', 'cpu.h', 'simd.h'))
        legacy_headers += (generated / 'recomp.h').read_bytes()
        legacy_headers += b''.join((build / 'native' / name).read_bytes() for name in ('startup-api.h', 'heap-api.h', 'thread-api.h'))
    except FileNotFoundError:
        legacy_headers = None
    # Patch a private copy; the pinned toolkit checkout remains unchanged.
    patched_runtime = build / 'runtime'
    patched_runtime.mkdir(exist_ok=True)
    for name in RUNTIME_HASHES:
        shutil.copyfile(runtime / name, patched_runtime / name)
    patch = ROOT / 'patches/retro-port-function-table.patch'
    subprocess.run(['patch', '-p1', '-i', str(patch)], cwd=build, check=True,
                   stdout=subprocess.DEVNULL)
    boundary_patch = ROOT / 'patches/retro-port-import-boundary.patch'
    subprocess.run(['patch', '-p1', '-i', str(boundary_patch)], cwd=build, check=True,
                   stdout=subprocess.DEVNULL)
    thread_patch = ROOT / 'patches/retro-port-thread-safety.patch'
    if args.threads:
        if not thread_patch.is_file():
            parser.error('Thread runtime patch has not been published; use the verified nonthreaded profile')
        subprocess.run(['patch', '-p1', '-i', str(thread_patch)], cwd=build, check=True,
                       stdout=subprocess.DEVNULL)
    runtime = patched_runtime
    host = build / 'native'
    host.mkdir(exist_ok=True)
    host_inputs = [*sorted((ROOT / 'native').glob('*.c')), *sorted((ROOT / 'native').glob('*.h')), *sorted((ROOT / 'native').glob('*.inc'))]
    input_names = {p.name for p in host_inputs}
    for old in host.iterdir():
        if old.suffix in ('.c', '.h', '.inc') and old.name not in input_names:
            old.unlink()
    for source in host_inputs:
        shutil.copyfile(source, host / source.name)
    # A Wasm compilation may overlap later API work. Compile a fixed snapshot.
    sources = sorted(generated.glob('recomp_*.c')) + [runtime / 'runtime.c', *[host / p.name for p in host_inputs if p.suffix == '.c']]
    flags = [f'-O{args.optimization}', '-frounding-math', '-I' + str(runtime), '-I' + str(generated)]
    if args.optimization:
        flags += ['-ffp-contract=off', '-fno-strict-aliasing', '-fwrapv']
    if args.threads:
        flags += ['-pthread', '-DMOVIES_GUEST_THREADS=1']
    compiler = 'clang'
    compile_env = os.environ.copy()
    if args.target == 'wasm':
        compiler = str(ROOT / '.cache/runtime-build/emsdk-main/upstream/emscripten/emcc')
        compile_env['EM_CACHE'] = str(ROOT / '.cache/recomp-full/emscripten-cache')
    headers = b''.join((runtime / name).read_bytes() for name in ('runtime.h', 'cpu.h', 'simd.h'))
    headers += (generated / 'recomp.h').read_bytes()
    compile_logs = build / 'compile-logs'
    compile_logs.mkdir(exist_ok=True)

    def compile_one(source):
        obj = build / (source.stem + '.o')
        stamp = obj.with_suffix('.sha256')
        extra_headers = b''
        if source.parent == host:
            extra_headers = b''.join(p.read_bytes() for p in sorted(host.glob('*.h')))
            extra_headers += b''.join(p.read_bytes() for p in sorted(host.glob('*.inc')))
        source_bytes = source.read_bytes()
        digest = hashlib.sha256(source_bytes + headers + extra_headers + repr(flags).encode()).hexdigest()
        if obj.exists() and stamp.exists() and legacy_headers is not None and source.parent != host:
            old_digest = hashlib.sha256(source_bytes + legacy_headers + repr(flags).encode()).hexdigest()
            if stamp.read_text() == old_digest:
                stamp.write_text(digest)
        if not obj.exists() or not stamp.exists() or stamp.read_text() != digest:
            print(f'[compile O{args.optimization}] {source.name}', flush=True)
            command = [compiler, *flags, '-c', str(source), '-o', str(obj)]
            log_path = compile_logs / (source.name + '.log')
            with log_path.open('w') as log_file:
                compiled = subprocess.run(command, env=compile_env, stdout=log_file, stderr=subprocess.STDOUT)
            if compiled.returncode:
                print(f'Compiler failed; complete output: {log_path}', flush=True)
                print('\n'.join(log_path.read_text(errors='replace').splitlines()[-30:]), flush=True)
                raise subprocess.CalledProcessError(compiled.returncode, command)
            stamp.write_text(digest)
        return obj

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        objects = list(pool.map(compile_one, sources))
    executable = build / ('bootstrap' if args.target == 'native' else 'bootstrap.cjs')
    link_flags = [] if args.target == 'native' else [
        '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=33554432', '-sMAXIMUM_MEMORY=2147483648', '-sSTACK_SIZE=16777216',
        '-sENVIRONMENT=node', '-sNODERAWFS=1', '-sEXIT_RUNTIME=1',
    ]
    link_flags = [f'-O{args.optimization}', *link_flags]
    if args.threads:
        link_flags += ['-pthread']
        if args.target == 'wasm':
            link_flags += ['-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2']
    print(f'[link O{args.optimization}] {executable.name}', flush=True)
    subprocess.run([compiler, *map(str, objects), '-lm', *link_flags, '-o', str(executable)],
                   check=True, env=compile_env)
    env = dict(os.environ, PORT_LOG_FILES='1')
    env.pop('PORT_LOG_IMPORTS', None)
    if args.trace_imports:
        env['PORT_LOG_IMPORTS'] = '1'
    env['MOVIES_USERDATA_ROOT'] = str(ROOT / '.cache/recomp-full/userdata')
    env.pop('MOVIES_STARTUP_APIS', None)
    if args.startup_apis:
        env['MOVIES_STARTUP_APIS'] = '1'
    try:
        command = ([str(executable)] if args.target == 'native' else ['node', str(executable)])
        run = subprocess.run([*command, str(args.game_exe.resolve())], env=env,
                             capture_output=True, text=True, timeout=args.timeout)
        output, status = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as exc:
        output = ((exc.stdout or b'') + (exc.stderr or b'')).decode(errors='replace')
        status = 'timeout'
    (build / 'run.log').write_text(output)
    result = {'scope': 'Strict original-entry bootstrap; not gameplay', 'target': args.target,
              'gameExeSha256': EXE_SHA, 'translationStats': report['stats'],
              'functionTablePatchSha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
              'importBoundaryPatchSha256': hashlib.sha256(boundary_patch.read_bytes()).hexdigest(),
              'threaded': args.threads,
              'threadSafetyPatchSha256': hashlib.sha256(thread_patch.read_bytes()).hexdigest() if args.threads else None,
              'registeredFunctions': registration_count, 'startupApisEnabled': args.startup_apis,
              'importTracing': args.trace_imports, 'returnCode': status, 'output': output,
              'optimization': args.optimization, 'compileFlags': flags, 'linkFlags': link_flags}
    result['hostSourceSha256'] = {source.name: hashlib.sha256(source.read_bytes()).hexdigest()
                                for source in sources if source.parent != generated}
    result['hostInputSha256'] = {p.name: hashlib.sha256((host / p.name).read_bytes()).hexdigest() for p in host_inputs}
    result['executableSha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
    if args.target == 'wasm':
        result['wasmSha256'] = hashlib.sha256(executable.with_suffix('.wasm').read_bytes()).hexdigest()
    (build / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    label = 'startup-apis' if args.startup_apis else 'no-apis'
    (build / f'result-{label}.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
