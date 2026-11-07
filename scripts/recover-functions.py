#!/usr/bin/env python3
"""Recover MoviesSE function boundaries/switches with portable Ghidra; original stays read-only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = Path(__file__).resolve().parents[1] / ".cache/game-files" / "MoviesSE.exe"


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def recover(exe, tools, output, retro):
    manifest = json.loads((tools / "manifest.json").read_text())
    ghidra = tools / manifest["ghidra"]
    java_home = tools / manifest["javaHome"]
    scripts = retro / "plugins/retro-port/skills/port-pc-game-to-browser/template/re/scripts"
    exporter = scripts / "ExportFunctions.java"
    if not exporter.is_file():
        raise ValueError(f"Function exporter missing: {exporter}")
    before = digest(exe)
    output.mkdir(parents=True, exist_ok=True)
    (output / "ghidra").mkdir(exist_ok=True)
    # No source-game modification, and functions.json is published only after a
    # successful headless run and schema validation, never a partial export.
    temporary = output / "functions.pending.json"
    temporary.unlink(missing_ok=True)
    # Ghidra rejects ANY dot-prefixed project path component, including .cache.
    # Analyze in a private temporary directory, then retain the closed project
    # beneath our cache along with the export and log.
    project_temp = Path(tempfile.mkdtemp(prefix="movies-ghidra-"))
    retained_project = output / "ghidra" / project_temp.name
    command = [str(ghidra / "support/analyzeHeadless"), str(project_temp), "MoviesSE",
               "-import", str(exe), "-overwrite", "-max-cpu", "3",
               "-scriptPath", str(scripts), "-postScript", "ExportFunctions.java", str(temporary)]
    env = os.environ.copy()
    env["JAVA_HOME"] = str(java_home)
    env["JAVA_HOME_OVERRIDE"] = str(java_home)
    env["PATH"] = str(java_home / "bin") + os.pathsep + env.get("PATH", "")
    # LaunchSupport saves preferences before headless starts. Keep all Java
    # preferences, Ghidra state and temporary files inside the private cache.
    java_user = output / "java-home"
    java_temp = output / "java-tmp"
    java_user.mkdir(exist_ok=True)
    java_temp.mkdir(exist_ok=True)
    env["JAVA_TOOL_OPTIONS"] = shlex.join([f"-Duser.home={java_user}", f"-Djava.io.tmpdir={java_temp}"])
    env["GHIDRA_HEADLESS_MAXMEM"] = "3G"
    started = time.monotonic()
    print(f"Analyzing {exe.name}; log: {output / 'analysis.log'}", flush=True)
    try:
        with (output / "analysis.log").open("w") as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    finally:
        shutil.move(str(project_temp), retained_project)
    if digest(exe) != before:
        raise ValueError("Original executable changed during analysis")
    if result.returncode:
        raise ValueError(f"Ghidra exited {result.returncode}; inspect {output / 'analysis.log'}")
    data = json.loads(temporary.read_text())
    if not isinstance(data.get("functions"), list) or not data["functions"] or not isinstance(data.get("jumptables"), list):
        raise ValueError("Incomplete function export")
    if any(not isinstance(f.get("entry"), int) or not isinstance(f.get("ranges"), list) for f in data["functions"]):
        raise ValueError("Malformed function entry")
    temporary.replace(output / "functions.json")
    report = {"schemaVersion": 1, "executable": exe.name, "executableSha256": before,
              "executableBytes": exe.stat().st_size, "exporterSha256": digest(exporter),
              "ghidra": manifest["ghidra"], "elapsedSeconds": round(time.monotonic() - started, 2),
              "projectDirectory": str(retained_project.relative_to(output)),
              "functionCount": len(data["functions"]), "jumpTableCount": len(data["jumptables"]),
              "unresolvedJumpCount": sum(not item.get("targets") for item in data["jumptables"]),
              "analysisDiagnostics": [line for line in (output / "analysis.log").read_text(errors="replace").splitlines()
                                      if line.startswith(("WARN", "ERROR"))],
              "note": "Recovered functions are analysis results, not a proof of complete code coverage or playable translation."}
    (output / "recovery-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", nargs="?", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--tools", type=Path, default=ROOT / ".cache/analysis-tools")
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/recomp-full")
    parser.add_argument("--retro-port", type=Path, default=ROOT / ".cache/retro-port")
    args = parser.parse_args()
    try:
        recover(args.exe.resolve(), args.tools.resolve(), args.output.resolve(), args.retro_port.resolve())
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Recovery failed: {error}\n")
