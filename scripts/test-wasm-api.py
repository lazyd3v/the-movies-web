#!/usr/bin/env python3
"""Compile and run existing host-API contract tests as Wasm in Node.

This validates test fixtures only, not execution of the original game. Uses the
cached Emscripten SDK; all build artifacts/logs stay in .cache/wasm-api-tests.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emcc", type=Path, default=ROOT / ".cache/runtime-build/emsdk-main/upstream/emscripten/emcc")
    parser.add_argument("--suite", choices=("startup", "heap", "thread", "process", "text"), action="append")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    cache = ROOT / ".cache/wasm-api-tests"
    cache.mkdir(parents=True, exist_ok=True)
    (cache / "package.json").write_text('{"type":"commonjs"}\n')
    report_path = cache / "report.json"
    report_path.unlink(missing_ok=True)
    runtime = ROOT / ".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime"
    environment = dict(os.environ, EM_CACHE=str(cache / "emscripten-cache"))
    suites = args.suite or ("startup", "heap", "thread", "process", "text")
    report = {"scope": "Wasm host-API contract fixtures only; not original game execution", "requestedSuites": suites, "results": []}
    for suite in suites:
        source = ROOT / f"native/{suite}-api.c"
        fixture = ROOT / f"tests/test_{suite}_api.c"
        inputs = [source, fixture, *sorted((ROOT / "native").glob("*-api.h")), runtime / "runtime.h", runtime / "cpu.h"]
        inputs += sorted((ROOT / 'native').glob('*.inc'))
        dependencies = [ROOT / 'native/text-api.c'] if suite == 'process' else []
        inputs += dependencies
        if suite == 'text':
            inputs.append(ROOT / 'tests/data/bestfit1252.txt')
        hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
        output = cache / (suite + ".js")
        flags = ["-O2", "-sASSERTIONS=1", "-sEXIT_RUNTIME=1", "-sENVIRONMENT=node",
                 "-I" + str(ROOT / "native"), "-I" + str(runtime)]
        if suite in ("heap", "thread"):
            flags += ["-pthread", "-sPTHREAD_POOL_SIZE=2"]
        if suite == "heap":
            flags += ["-DMOVIES_HEAP_BASE=0x1000", "-DMOVIES_HEAP_HI=0x11000", "-DMOVIES_HEAP_PROFILE=1"]
        if suite == "text":
            flags += ["-DMOVIES_TEXT_SPACE=0x10000"]
            flags += ['--embed-file', str(ROOT / 'tests/data/bestfit1252.txt') + '@/tests/data/bestfit1252.txt']
        if suite == "process":
            flags += ["-sALLOW_MEMORY_GROWTH=1", "-DMOVIES_PROCESS_SPACE=0x2100010"]
        result = {"suite": suite, "sourceHashes": hashes,
                  "compilerFlags": [flag for flag in flags if not flag.startswith("-I")], "status": "failed"}
        try:
            with (cache / (suite + "-build.log")).open("w") as log:
                subprocess.run([str(args.emcc.resolve()), *flags, str(fixture), str(source), *map(str, dependencies), "-o", str(output)],
                               check=True, stdout=log, stderr=subprocess.STDOUT, env=environment, timeout=300)
            if any(sha(p) != hashes[str(p.relative_to(ROOT))] for p in inputs):
                raise RuntimeError("Source changed during compilation; rerun this suite")
            run = subprocess.run(["node", str(output)], capture_output=True, text=True, timeout=args.timeout)
            (cache / (suite + "-run.log")).write_text(run.stdout + run.stderr)
            marker = f"{suite} ABI tests passed" if suite in ("startup", "process") else f"{suite} API tests passed"
            if run.returncode != 0 or marker not in run.stdout:
                raise RuntimeError(f"Contract test failed (exit {run.returncode}); see run log")
            result.update({"status": "passed", "wasmBytes": output.with_suffix(".wasm").stat().st_size,
                           "wasmSha256": sha(output.with_suffix(".wasm")), "testOutput": run.stdout.strip(),
                           "pthreadPoolSize": 2 if suite in ("heap", "thread") else 0})
        except (subprocess.SubprocessError, RuntimeError) as error:
            result["error"] = str(error)
        report["results"].append(result)
        print(f"{suite}: {result['status']}", flush=True)
        report["allPassed"] = len(report["results"]) == len(suites) and all(item["status"] == "passed" for item in report["results"])
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    return 0 if report["allPassed"] else 1


if __name__ == "__main__":
    sys.exit(main())
