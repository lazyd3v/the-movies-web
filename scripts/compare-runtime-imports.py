#!/usr/bin/env python3
"""Compare static imports with the pinned retro-port registration tables.

This measures name overlap only, NOT API correctness or game compatibility.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

PIN = "ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf"
ROOT = Path(__file__).resolve().parents[1]


def registrations(host):
    found = set()
    for filename in ("kernel32.c", "user32.c", "misc.c"):
        source = (host / filename).read_text()
        macros = dict(re.findall(r'#define\s+(\w+)\(name\)\s+IMPORT\("([^"]+)",\s*name\)', source))
        for body in re.findall(r'static const ImportHandler\s+\w+\[\]\s*=\s*\{(.*?)\n?\};', source, re.S):
            for macro, name in re.findall(r'\b(\w+)\((\w+)\)', body):
                if macro in macros:
                    found.add((macros[macro].lower(), name))
            for dll, name in re.findall(r'"([^"!]+)!([^"!]+)"', body):
                found.add((dll.lower(), name))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolkit", type=Path, default=ROOT / ".cache/retro-port")
    parser.add_argument("--audit", type=Path, default=ROOT / "docs/research/pe-import-audit.json")
    args = parser.parse_args()
    revision = subprocess.check_output(["git", "-C", str(args.toolkit), "rev-parse", "HEAD"], text=True).strip()
    if revision != PIN:
        parser.error(f"Expected toolkit revision {PIN}, got {revision}")
    host = args.toolkit / "plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/host"
    registered = registrations(host)
    audit = json.loads(args.audit.read_text())
    game = next(item for item in audit["files"] if item["file"] == "MoviesSE.exe")
    rows = []
    for kind in ("imports", "delayImports"):
        for dll, entries in game[kind].items():
            names = [entry.get("name") or f'#{entry["ordinal"]}' for entry in entries]
            rows.append({"dll": dll, "kind": kind,
                         "registeredNames": [name for name in names if (dll.lower(), name) in registered],
                         "missingNames": [name for name in names if (dll.lower(), name) not in registered]})
    print(json.dumps({"toolkitRevision": revision, "gameSha256": game["sha256"],
                      "method": "Source registration-name intersection only. Registered handlers may be stubs or incomplete. Does not measure COM methods, dynamic imports, correctness or performance.",
                      "registeredCount": sum(len(row["registeredNames"]) for row in rows),
                      "missingCount": sum(len(row["missingNames"]) for row in rows),
                      "rows": rows}, indent=2))


if __name__ == "__main__":
    main()
