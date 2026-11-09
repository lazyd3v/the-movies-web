#!/usr/bin/env python3
"""Build/run a strict native diagnostic from translated game code (not a port).

Original game and generated files remain in ignored directories. The native
diagnostic helps isolate translation errors before adding browser integration.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import re
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


EXCHANGE_PATTERNS = [
    (re.compile(r'\{ guest_lock\(\); \{ uint32_t t0_ = R32\(\(uint32_t\)\((?P<a>[^;]+?)\)\), t1_ = (?P<s>[^;]+?); '
                r'W32\(\(uint32_t\)\((?P=a)\), \(uint32_t\)\(t1_\)\); (?P<d>[^{}]+?) \} guest_unlock\(\); \}'),
     r'{ uint32_t t1_ = \g<s>; uint32_t t0_ = guest_xchg32((uint32_t)(\g<a>), t1_); \g<d> }'),
    (re.compile(r'\{ guest_lock\(\); \{ uint8_t t0_ = R8\(\(uint32_t\)\((?P<a>[^;]+?)\)\), t1_ = (?P<s>[^;]+?); '
                r'W8\(\(uint32_t\)\((?P=a)\), \(uint8_t\)\(t1_\)\); (?P<d>[^{}]+?) \} guest_unlock\(\); \}'),
     r'{ uint8_t t1_ = \g<s>; uint8_t t0_ = guest_xchg8((uint32_t)(\g<a>), t1_); \g<d> }'),
]


ENTRY_HEADER = re.compile(r'^void f_(?P<entry>[0-9a-f]{8})\(CPU \*c\) \{')
TERMINATOR = re.compile(r'/\* (?:ret|retn|jmp|int3|hlt|ud2)\b')


def fix_entry_order(sources, directory):
    """The translator emits a function's instructions in address order. When Ghidra
    gives a function a tail-jump chunk below its entry (e.g. 009dd0d0 ending in
    `jmp 0x9db140`), that chunk comes first and the C function starts there instead
    of at the entry. Jump to the entry first. In recursive-descent output, code after
    a terminator is either labelled (a jump target) or the unlabelled entry block."""
    directory.mkdir(parents=True, exist_ok=True)
    result, fixed = [], []
    for source in sources:
        lines = source.read_text().split('\n')
        changed = False
        i = 0
        while i < len(lines):
            m = ENTRY_HEADER.match(lines[i])
            if not m:
                i += 1; continue
            entry = m.group('entry')
            first = i + 1
            while lines[first].strip().startswith('PREEMPT_POLL') or not lines[first].strip():
                first += 1
            label = re.fullmatch(r'L_([0-9a-f]{8}):;', lines[first].strip())
            if not label or label.group(1) == entry:
                i += 1; continue
            end = first
            while lines[end] != '}':
                end += 1
            body = lines[first:end]
            if f'L_{entry}:;' not in (line.strip() for line in body):
                starts = [k for k in range(1, len(body))
                          if (TERMINATOR.search(body[k - 1]) or (body[k - 1].strip().startswith('goto ') and '/*' not in body[k - 1]))
                          and not body[k].strip().startswith('L_')]
                if len(starts) != 1:
                    raise SystemExit(f'Cannot locate entry block of f_{entry}: {len(starts)} candidates')
                body.insert(starts[0], f'L_{entry}:;')
            lines[first:end] = [f'    goto L_{entry}; /* entry {entry} follows lower tail-jump chunk */', *body]
            fixed.append(entry); changed = True
            i = first + len(body) + 1
        if changed:
            text = '\n'.join(lines)
            target = directory / source.name
            if not target.exists() or target.read_text() != text:
                target.write_text(text)
            result.append(target)
        else:
            result.append(source)
    expected = {'00510440', '0058f440', '005b3d10', '0070f640', '008600e0', '0087feb0', '009dd0d0'}
    if set(fixed) != expected:
        raise SystemExit(f'Entry-order fixes changed: {sorted(set(fixed) ^ expected)}')
    return result


INTRO_UPDATE = 'void f_0045e3f0(CPU *c) { /* FUN_0045e3f0 */\n    PREEMPT_POLL(c);\n'


def skip_intro(sources, directory):
    """Skip the startup logo sequence (Lionhead logo, Activision video, Intel and ATI
    splashes, legal screen). 0045e3f0 is the sequence's update, called once per
    message-loop pass from 0045e720 with ecx = the sequence object; [this+4] is the
    state (0 start ... 5 legal, 6 finished) and `mov al, 1; ret` reports completion.
    From state 0 nothing has been loaded yet, so finishing there needs no cleanup.
    The main menu's own intro (frontend_intro.wmv) is not part of this sequence."""
    directory.mkdir(parents=True, exist_ok=True)
    result, patched = [], 0
    for source in sources:
        text = source.read_text()
        if INTRO_UPDATE in text:
            text = text.replace(INTRO_UPDATE, INTRO_UPDATE +
                '    if (R32(c->ecx + 4u) == 0u) { W32(c->ecx + 4u, 6u); c->eax = (c->eax & ~0xffu) | 1u; c->esp += 4; return; } /* port: skip startup logos */\n', 1)
            target = directory / source.name
            if not target.exists() or target.read_text() != text:
                target.write_text(text)
            result.append(target); patched += 1
        else:
            result.append(source)
    if patched != 1:
        raise SystemExit(f'Intro-skip patch matched {patched} files, expected 1')
    return result


def rewrite_exchanges(sources, directory):
    """Translated `xchg mem, reg` groups become real atomic exchanges, so plain guest
    accesses need no global lock (patches/retro-port-lockfree-access.patch). Any other
    lock group left behind must be a translator trap that never completes."""
    directory.mkdir(parents=True, exist_ok=True)
    result, rewritten = [], 0
    for source in sources:
        text = source.read_text()
        if 'guest_lock();' not in text:
            result.append(source); continue
        for pattern, replacement in EXCHANGE_PATTERNS:
            text, n = pattern.subn(replacement, text); rewritten += n
        for line in text.splitlines():
            if 'guest_lock();' in line and 'guest_trap(' not in line:
                raise SystemExit(f'Unrewritten translated lock group in {source.name}: {line.strip()[:160]}')
        target = directory / source.name
        if not target.exists() or target.read_text() != text:
            target.write_text(text)
        result.append(target)
    if rewritten != 36:
        raise SystemExit(f'Expected 36 translated exchange groups, rewrote {rewritten}')
    return result


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
    simd_patch = ROOT / 'patches/retro-port-simd-sign.patch'
    subprocess.run(['patch', '-p1', '-i', str(simd_patch)], cwd=build, check=True,
                   stdout=subprocess.DEVNULL)
    thread_patch = ROOT / 'patches/retro-port-thread-safety.patch'
    lockfree_patch = ROOT / 'patches/retro-port-lockfree-access.patch'
    if args.threads:
        if not thread_patch.is_file():
            parser.error('Thread runtime patch has not been published; use the verified nonthreaded profile')
        subprocess.run(['patch', '-p1', '-i', str(thread_patch)], cwd=build, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(['patch', '-p1', '-i', str(lockfree_patch)], cwd=build, check=True,
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
    translated = sorted(generated.glob('recomp_*.c'))
    translated = fix_entry_order(translated, build / 'translated-fixed')
    translated = skip_intro(translated, build / 'translated-fixed')
    if args.threads:
        translated = rewrite_exchanges(translated, build / 'translated-lockfree')
    sources = translated + [runtime / 'runtime.c', *[host / p.name for p in host_inputs if p.suffix == '.c']]
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
        digest = hashlib.sha256(source_bytes + headers + extra_headers + repr(flags).encode() + (b'host-O3' if args.optimization and source.parent == host else b'')).hexdigest()
        if obj.exists() and stamp.exists() and legacy_headers is not None and source.parent != host:
            old_digest = hashlib.sha256(source_bytes + legacy_headers + repr(flags).encode()).hexdigest()
            if stamp.read_text() == old_digest:
                stamp.write_text(digest)
        if not obj.exists() or not stamp.exists() or stamp.read_text() != digest:
            print(f'[compile O{args.optimization}] {source.name}', flush=True)
            # Host API layer (small, hot: vertex conversion/lighting, allocator) at -O3;
            # the large recompiled guest code keeps the requested level.
            unit_flags = [('-O3' if f.startswith('-O') and args.optimization and source.parent == host else f) for f in flags]
            command = [compiler, *unit_flags, '-c', str(source), '-o', str(obj)]
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
            link_flags += ['-sPTHREAD_POOL_SIZE=8', '-sPTHREAD_POOL_SIZE_STRICT=2']
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
              'simdSignPatchSha256': hashlib.sha256(simd_patch.read_bytes()).hexdigest(),
              'lockfreeAccessPatchSha256': hashlib.sha256(lockfree_patch.read_bytes()).hexdigest() if args.threads else None,
              'threaded': args.threads,
              'threadSafetyPatchSha256': hashlib.sha256(thread_patch.read_bytes()).hexdigest() if args.threads else None,
              'registeredFunctions': registration_count, 'startupApisEnabled': args.startup_apis,
              'importTracing': args.trace_imports, 'returnCode': status, 'output': output,
              'optimization': args.optimization, 'compileFlags': flags, 'linkFlags': link_flags}
    result['hostSourceSha256'] = {source.name: hashlib.sha256(source.read_bytes()).hexdigest()
                                for source in sources if source.parent not in (generated, build / 'translated-lockfree', build / 'translated-fixed')}
    result['rewrittenTranslationSha256'] = {source.name: hashlib.sha256(source.read_bytes()).hexdigest()
                                            for source in sources if source.parent in (build / 'translated-lockfree', build / 'translated-fixed')}
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
