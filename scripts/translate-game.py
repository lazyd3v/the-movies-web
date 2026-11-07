#!/usr/bin/env python3
"""Generate full static-translation candidates and report remaining blockers.

Exit 0 means generation without explicit translation traps; it never means the
game runs. Exit 2 means generation completed with explicit traps/translation
failures. Exit 1 means preparation or generation failed. Outputs containing game
code stay beneath .cache; published reports contain metadata only.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(__file__).resolve().parents[1] / ".cache/game-files" / "MoviesSE.exe"
EXE_SHA = "48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a"
TRANSLATOR_SHA = "3d42b5d26b9ac803375da2f888e959505490d99f4e5e1d8bbb09c87565fa4dea"
TOOL_COMMIT = "ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf"
BOUNDARY_PATCH_SHA = "d1ae8456bccb9cb694bf49fd8f26a9b0f8a3981b1bd2f6bd49a49c54bab474fe"
BOUNDARY_TRANSLATOR_SHA = "472cc89414f69cf5f410ef498c89a37b8aee0ecc5451f72009367081b1a490d4"
LOCAL_SIMD_PATCH_SHA = "8f9f53782ba595e8ccdfe0b1d374879e1a240a7d230d7f87cda79ca652d52907"
PATCHED_TRANSLATOR_SHA = "3daba930925cbd6a9d1ddc6a39314bbf19bb76d7a98bab46b30decbe5b738590"
FUNCTION = re.compile(r'^void f_([0-9a-fA-F]{8})\(CPU \*c\)\s*\{', re.M)
TRAP = re.compile(r'guest_trap\(c,\s*0x[0-9a-fA-F]+u,\s*"([^"\n]*)"\)')
REGISTRATION = re.compile(r'    register_function\(0x([0-9a-f]{8})u, f_([0-9a-f]{8})\);')


def prepare_translator(toolkit, destination):
    """Patch a verified copy; leave the cached upstream translator unchanged."""
    original = toolkit / "translator/x86toc.py"
    patch = ROOT / "patches/retro-port-final-code-boundary.patch"
    if digest(original) != TRANSLATOR_SHA or digest(patch) != BOUNDARY_PATCH_SHA:
        raise ValueError("Translator or boundary patch checksum mismatch")
    output = destination / "translator/x86toc.py"
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(original, output)
    subprocess.run(["patch", "-s", "-p1", "-i", str(patch)], cwd=destination, check=True)
    if digest(output) != BOUNDARY_TRANSLATOR_SHA:
        raise ValueError("Boundary-patched translator checksum mismatch")
    local_patch = ROOT / "patches/translator-local-simd-copy.patch"
    if digest(local_patch) != LOCAL_SIMD_PATCH_SHA:
        raise ValueError("Local SIMD patch checksum mismatch")
    subprocess.run(["patch", "-s", "-p1", "-i", str(local_patch)], cwd=destination, check=True)
    if digest(output) != PATCHED_TRANSLATOR_SHA:
        raise ValueError("Patched translator checksum mismatch")
    return output


def batch_registration(source, batch_size=256):
    """Split only the exact generated registration sequence, preserving order."""
    if type(batch_size) is not int or not 1 <= batch_size <= 1024:
        raise ValueError("Invalid registration batch size")
    marker = "void register_all_functions(void) {\n"
    if source.count(marker) != 1:
        raise ValueError("Expected one generated registration function")
    prefix, body = source.split(marker)
    if not body.endswith("}\n"):
        raise ValueError("Unexpected registration suffix")
    lines = body[:-2].splitlines()
    if not lines:
        raise ValueError("Empty registration function")
    for line in lines:
        match = REGISTRATION.fullmatch(line)
        if not match or match.group(1) != match.group(2):
            raise ValueError("Unexpected registration statement; refusing rewrite")
    batches = [lines[i:i + batch_size] for i in range(0, len(lines), batch_size)]
    output = [prefix]
    for index, batch in enumerate(batches):
        output.append(f"static void register_batch_{index:05d}(void) {{\n" + "\n".join(batch) + "\n}\n\n")
    output.append(marker)
    output.extend(f"    register_batch_{index:05d}();\n" for index in range(len(batches)))
    output.append("}\n")
    return "".join(output), {"batchSize": batch_size, "batches": len(batches), "registrations": len(lines)}


def rewrite_registration(out):
    path = out / "recomp_stubs.c"
    original_hash = digest(path)
    rewritten, details = batch_registration(path.read_text())
    temporary = path.with_suffix(".tmp")
    temporary.write_text(rewritten)
    os.replace(temporary, path)
    return {**details, "originalSha256": original_hash, "sha256": digest(path)}


def split_existing(out, metadata):
    """Reprocess only checksum-verified existing output; no translator rerun."""
    report = json.loads(metadata.read_text())
    if report.get("status") not in ("generated_with_blockers", "generated_unvalidated") or report.get("gameExeSha256") != EXE_SHA:
        raise ValueError("No completed pinned translation to reprocess")
    records = report["generatedFiles"] + [report["generatedHeader"]]
    for record in records:
        if Path(record["file"]).name != record["file"]:
            raise ValueError("Invalid generated filename")
        path = out / record["file"]
        if path.stat().st_size != record["bytes"] or digest(path) != record["sha256"]:
            raise ValueError("Existing generated output checksum mismatch")
    if report.get("registrationBatching"):
        return report  # Already normalized and all bytes verified above.
    details = rewrite_registration(out)
    for record in report["generatedFiles"]:
        if record["file"] == "recomp_stubs.c":
            record.update({"bytes": (out / record["file"]).stat().st_size, "sha256": details["sha256"]})
    report["registrationBatching"] = details
    report["generatedCode"]["generatedCBytes"] = sum(record["bytes"] for record in report["generatedFiles"])
    temporary = metadata.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n")
    os.replace(temporary, metadata)
    return report


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def supplement_functions(data, config, exe):
    """Add only evidenced, byte-verified entries; never alter the Ghidra export."""
    validate_functions(data)
    if config.get("schemaVersion") != 1 or config.get("gameExeSha256") != EXE_SHA or digest(exe) != EXE_SHA:
        raise ValueError("Supplemental entry executable identity mismatch")
    functions = list(data["functions"])
    entries = set(f["entry"] for f in functions)
    for item in config["entries"]:
        start, end, offset = item["entry"], item["end"], item["fileOffset"]
        if any(type(n) is not int for n in (start, end, offset)) or not 0 <= start < end < 2**32 or offset < 0:
            raise ValueError("Invalid supplemental entry bounds")
        if start in entries or any(low < end and start < high for f in functions for low, high in f["ranges"]):
            raise ValueError("Supplemental entry overlaps recovered code")
        with exe.open("rb") as stream:
            stream.seek(offset)
            code = stream.read(end - start)
        if len(code) != end - start or hashlib.sha256(code).hexdigest() != item["sha256"]:
            raise ValueError("Supplemental entry machine-code checksum mismatch")
        functions.append({"entry": start, "name": f"EVIDENCED_{start:08x}", "ranges": [[start, end]]})
        entries.add(start)
    return {**data, "functions": sorted(functions, key=lambda f: f["entry"])}


def validate_functions(data):
    if not isinstance(data, dict) or not isinstance(data.get("functions"), list) or not isinstance(data.get("jumptables"), list):
        raise ValueError("Expected Ghidra functions and jumptables arrays")
    entries = set()
    ranges = []
    for function in data["functions"]:
        entry = function["entry"]
        if type(entry) is not int or not 0 <= entry < 2**32 or entry in entries:
            raise ValueError("Invalid or duplicate function entry")
        entries.add(entry)
        if not isinstance(function.get("name"), str) or not isinstance(function.get("ranges"), list):
            raise ValueError("Invalid function metadata")
        for bounds in function["ranges"]:
            if not isinstance(bounds, list) or len(bounds) != 2 or any(type(n) is not int for n in bounds):
                raise ValueError("Invalid function range")
            low, high = bounds
            if not 0 <= low < high <= 2**32:
                raise ValueError("Invalid function range bounds")
            ranges.append((low, high))
    if not entries or not ranges:
        raise ValueError("Empty function recovery")
    for table in data["jumptables"]:
        if type(table.get("at")) is not int or not 0 <= table["at"] < 2**32:
            raise ValueError("Invalid jump-table address")
        if not isinstance(table.get("targets"), list) or any(type(n) is not int or not 0 <= n < 2**32 for n in table["targets"]):
            raise ValueError("Invalid jump-table targets")
    covered, end = 0, -1
    for low, high in sorted(ranges):
        covered += max(0, high - max(low, end))
        end = max(end, high)
    return {"exportedFunctions": len(entries), "functionsWithRanges": sum(bool(f["ranges"]) for f in data["functions"]),
            "recoveredRangeUnionBytes": covered, "jumpTables": len(data["jumptables"]),
            "jumpTablesWithoutTargets": sum(not t["targets"] for t in data["jumptables"])}


def scan_generated(paths):
    reasons = Counter()
    functions = set()
    trap_functions = set()
    dispatch_functions = set()
    call_sites = jump_sites = fault_checks = generated_bytes = 0
    for path in paths:
        text = path.read_text()
        generated_bytes += path.stat().st_size
        starts = list(FUNCTION.finditer(text))
        for i, match in enumerate(starts):
            address = "0x" + match.group(1).lower()
            if address in functions:
                raise ValueError("Duplicate generated function definition")
            functions.add(address)
            body = text[match.end():starts[i + 1].start() if i + 1 < len(starts) else len(text)]
            for reason in TRAP.findall(body):
                if reason == "divide by zero":
                    fault_checks += 1
                else:
                    # Never publish original instruction text embedded in a trap.
                    category = reason.split(":", 1)[0]
                    reasons[category] += 1
                    trap_functions.add(address)
            calls = body.count("call_indirect(")
            jumps = body.count("jump_indirect(")
            call_sites += calls
            jump_sites += jumps
            if calls or jumps:
                dispatch_functions.add(address)
    return {"generatedFunctionDefinitions": len(functions), "generatedCBytes": generated_bytes,
            "explicitTrapSitesByCategory": dict(sorted(reasons.items())),
            "functionsWithExplicitTraps": len(trap_functions), "trapFunctionAddressSample": sorted(trap_functions)[:100],
            "trapFunctionSampleTruncated": len(trap_functions) > 100,
            "architecturalDivideByZeroChecks": fault_checks,
            "indirectDispatcherCallSites": call_sites, "indirectDispatcherJumpSites": jump_sites,
            "functionsUsingIndirectDispatch": len(dispatch_functions)}


def verify_supplemental_output(out, paths, entries):
    """Recovery must emit a registered implementation, never an import/gap stub."""
    registration = (out / "recomp_stubs.c").read_text()
    header = (out / "recomp.h").read_text()
    remaining = set(entries)
    for path in paths:
        text = path.read_text()
        starts = list(FUNCTION.finditer(text))
        for index, match in enumerate(starts):
            entry = int(match.group(1), 16)
            if entry not in remaining:
                continue
            body = text[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(text)]
            if TRAP.search(body) or "ret 8" not in body:
                raise ValueError("Supplemental raster callback did not translate completely")
            name = f"{entry:08x}"
            if f"register_function(0x{name}u, f_{name});" not in registration or f"void f_{name}(CPU *c);" not in header:
                raise ValueError("Supplemental callback is not declared and registered")
            remaining.remove(entry)
    if remaining:
        raise ValueError("Supplemental callback implementation missing")


def summarize(upstream, scanned, recovered):
    stats = upstream.get("stats", {})
    unsupported = upstream.get("unsupported", [])
    # Upstream includes exception messages and operands in some strings. Keep
    # only mnemonic/category tokens in the repository metadata.
    categories = Counter()
    for name, count in unsupported:
        categories[name.split()[0] if name else "unknown"] += count
    blocked = bool(scanned["functionsWithExplicitTraps"] or stats.get("unsupported_instructions", 0))
    return {"status": "generated_with_blockers" if blocked else "generated_unvalidated",
            "playable": False, "compiled": False, "executionValidated": False,
            "coverageMeaning": "Recovered/heuristic candidates emitted as C; not verified whole-executable coverage",
            "recovery": recovered, "translatorStats": stats, "generatedCode": scanned,
            "unsupportedMnemonicOrCategoryCounts": dict(sorted(categories.items())),
            "limitations": ["Unrecovered code and heuristic false positives remain possible",
                            "Indirect dispatch includes imports, callbacks and unresolved targets; none validated here",
                            "No Win32, COM, D3D9, threading, exception or floating-point equivalence established",
                            "No performance measurement or gameplay test"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-exe", type=Path, default=GAME)
    parser.add_argument("--functions", type=Path, default=ROOT / ".cache/recomp-full/functions.json")
    parser.add_argument("--toolkit", type=Path, default=ROOT / ".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp")
    parser.add_argument("--python", type=Path, default=ROOT / ".cache/recomp-probe/venv/bin/python")
    parser.add_argument("--extra-entries", type=Path, default=ROOT / "scripts/translation-extra-entries.json")
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/recomp-full", help="Isolated generation directory; do not share with a running build")
    parser.add_argument("--metadata", type=Path, default=ROOT / "docs/research/full-translation-result.json")
    parser.add_argument("--split-registration-only", action="store_true", help="Verify existing output and split its registration function; run only with compilation stopped")
    args = parser.parse_args()
    cache = args.output.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    metadata = args.metadata.resolve()
    metadata.parent.mkdir(parents=True, exist_ok=True)
    if args.split_registration_only:
        report = split_existing(cache / "out", metadata)
        print(json.dumps({"status": report["status"], "registrationBatching": report["registrationBatching"]}))
        return 2 if report["status"] == "generated_with_blockers" else 0
    # Replace stale results up front; a failed run must not retain success.
    metadata.write_text(json.dumps({"status": "generation_pending", "playable": False}) + "\n")
    try:
        translator = args.toolkit / "translator/x86toc.py"
        if digest(args.game_exe) != EXE_SHA or digest(translator) != TRANSLATOR_SHA:
            raise ValueError("Pinned executable or translator SHA-256 mismatch")
        supplemented = supplement_functions(json.loads(args.functions.read_text()), json.loads(args.extra_entries.read_text()), args.game_exe)
        effective_functions = cache / "functions-supplemented.json"
        effective_functions.write_text(json.dumps(supplemented) + "\n")
        recovered = validate_functions(supplemented)
        translator = prepare_translator(args.toolkit, cache / "patched-tool")
        out = cache / "out"
        out.mkdir(exist_ok=True)
        # The translator itself removes only its generated recomp_*.c outputs.
        with (cache / "translation.log").open("w") as log:
            # Preserve the venv executable path; resolving its symlink selects
            # the base interpreter and silently loses isolated dependencies.
            subprocess.run([str(args.python.absolute()), str(translator.resolve()), str(args.game_exe.resolve()),
                            str(effective_functions), str(out), "--all", "--per-file", "400"],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
        raw = json.loads((out / "translate_report.json").read_text())
        batching = rewrite_registration(out)
        paths = sorted(out.glob("recomp_*.c"))
        verify_supplemental_output(out, paths, [i["entry"] for i in json.loads(args.extra_entries.read_text())["entries"]])
        scanned = scan_generated(paths)
        if not scanned["generatedFunctionDefinitions"]:
            raise ValueError("No generated functions")
        report = summarize(raw, scanned, recovered)
        report.update({"schemaVersion": 1, "gameExeSha256": EXE_SHA, "toolCommit": TOOL_COMMIT,
                       "translatorSha256": TRANSLATOR_SHA, "functionExportSha256": digest(args.functions),
                       "effectiveFunctionExportSha256": digest(effective_functions),
                       "supplementalEntries": {"sha256": digest(args.extra_entries), "entries": [i["entry"] for i in json.loads(args.extra_entries.read_text())["entries"]]},
                       "effectiveTranslatorSha256": PATCHED_TRANSLATOR_SHA,
                       "translatorPatches": [{"file": "patches/retro-port-final-code-boundary.patch", "sha256": BOUNDARY_PATCH_SHA},
                                             {"file": "patches/translator-local-simd-copy.patch", "sha256": LOCAL_SIMD_PATCH_SHA}],
                       "registrationBatching": batching,
                       "generatedHeader": {"file": "recomp.h", "bytes": (out / "recomp.h").stat().st_size, "sha256": digest(out / "recomp.h")},
                       "generatedFiles": [{"file": p.name, "bytes": p.stat().st_size, "sha256": digest(p)} for p in paths]})
        temporary = metadata.with_suffix(".tmp")
        temporary.write_text(json.dumps(report, indent=2) + "\n")
        os.replace(temporary, metadata)
        print(json.dumps({"status": report["status"], "generated": scanned["generatedFunctionDefinitions"],
                          "withExplicitTraps": scanned["functionsWithExplicitTraps"], "report": str(metadata)}))
        return 2 if report["status"] == "generated_with_blockers" else 0
    except Exception as error:
        metadata.write_text(json.dumps({"status": "generation_failed", "playable": False,
                                       "errorType": type(error).__name__}) + "\n")
        raise


if __name__ == "__main__":
    sys.exit(main())
