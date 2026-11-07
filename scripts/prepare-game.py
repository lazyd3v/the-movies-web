#!/usr/bin/env python3
"""Package a locally installed copy for the browser without modifying it.

Usage: python3 scripts/prepare-game.py [SOURCE] [--output public/game/movies.zip]

The registry names below were observed in executable strings and install.log.
Their types, values and effectiveness have NOT been confirmed by running the
game. In particular GD: Level=0 and Fullscreen=0 are conservative guesses, not
a verified compatibility fix. The original executable and assets are unchanged.
ZIP_STORED is deliberate: assets are predominantly compressed already. ZIP64
permits large packages; the browser runtime must support their size separately.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile


DEFAULT_SOURCE = Path(__file__).resolve().parents[1] / ".cache/game-files"
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "public/game/movies.zip"
CHUNK_SIZE = 1024 * 1024


def launch_script():
    lines = [
        "@echo off",
        "rem Generated compatibility bootstrap; originals are unchanged.",
        "rem Registry setting types and values are inferred, not verified.",
        'cd /d "%~dp0"',
        'echo The Movies: applying compatibility settings',
        'regedit /s "%~dp0movies-web.reg"',
        'echo The Movies: starting MoviesSE.exe',
        '"%~dp0MoviesSE.exe"',
        'set "MOVIES_EXIT_CODE=%errorlevel%"',
        'echo The Movies: exited with code %MOVIES_EXIT_CODE%',
        'exit /b %MOVIES_EXIT_CODE%',
        '',
    ]
    return "\r\n".join(lines).encode("ascii")


def registry_script():
    lines = ["REGEDIT4", "", "; Inferred compatibility settings; not verified.",
             "; Game directory is mounted as C:\\files by the browser runtime.", ""]
    for hive in ("HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE"):
        key = hive + r"\Software\Lionhead Studios Ltd\TheMovies"
        lines.append(f"[{key}]")
        for name, value in (("Fullscreen", 0), ("GD: Level", 0),
                            ("GD: Screen Width", 800), ("GD: Screen Height", 600),
                            ("GD: Screen Depth", 32)):
            lines.append(f'"{name}"=dword:{value:08x}')
        lines.append("")
    key = r"HKEY_LOCAL_MACHINE\Software\Lionhead Studios Ltd\TheMovies Stunts & Effects"
    lines.append(f"[{key}]")
    for name, value in (("InstallPath", "C:\\files\\"), ("InstallDrive", "C:"),
                        ("EXEString", "C:\\files\\MoviesSE.exe"), ("Version", "1.2"),
                        ("IntVersion", "17.0"), ("Language", "English"),
                        ("LanguageCode", "EN-UK")):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'"{name}"="{escaped}"')
    lines.append("")
    return "\r\n".join(lines).encode("ascii")


def validate_source(source, output):
    source = source.resolve(strict=True)
    if not source.is_dir():
        raise ValueError("Source must be the Game Files directory")
    executable = source / "MoviesSE.exe"
    if not executable.is_file():
        raise ValueError("Source is missing MoviesSE.exe")
    with executable.open("rb") as handle:
        if handle.read(2) != b"MZ":
            raise ValueError("MoviesSE.exe does not have a Windows MZ header")
    pak = source / "Data/pak"
    if not pak.is_dir() or not any(p.is_file() and p.stat().st_size for p in pak.glob("*.pak")):
        raise ValueError("Source requires a nonempty Data/pak/*.pak archive")
    if output.resolve().is_relative_to(source):
        raise ValueError("Output must be outside the original game directory")
    paths = sorted(source.rglob("*"), key=lambda p: p.relative_to(source).as_posix())
    if any(p.is_symlink() for p in paths):
        raise ValueError("Source contains symbolic links; refusing to follow them")
    files = [p for p in paths if p.is_file()]
    if any(p.relative_to(source).as_posix().lower() in {"launch.bat", "movies-web.reg"} for p in files):
        raise ValueError("Source file conflicts with generated launch.bat or movies-web.reg")
    return source, files


def zip_info(name):
    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    entry.compress_type = zipfile.ZIP_STORED
    entry.create_system = 3
    entry.external_attr = 0o100644 << 16
    return entry


def read_version(source):
    path = source / "version.inf"
    result = {}
    if path.is_file():
        # Only small version metadata is read at once; assets are streamed.
        with path.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = re.match(r"\s*(ExtVersion|IntVersion|Language)\s*=\s*(.*?)\s*$", line)
                if match:
                    result[match[1]] = match[2]
    return result


def prepare_game(source=DEFAULT_SOURCE, output=DEFAULT_OUTPUT):
    output = Path(output)
    source, files = validate_source(Path(source), output)
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = output.parent / "manifest.json"
    if output.resolve() == manifest_path.resolve():
        raise ValueError("Archive output cannot be named manifest.json")
    temporary = []
    try:
        fd, archive_temp = tempfile.mkstemp(prefix=".movies-", suffix=".zip", dir=output.parent)
        os.close(fd)
        temporary.append(archive_temp)
        file_records = []
        executable_digest = hashlib.sha256()
        with zipfile.ZipFile(archive_temp, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
            for path in files:
                name = path.relative_to(source).as_posix()
                size = 0
                with path.open("rb") as reader, archive.open(zip_info(name), "w", force_zip64=True) as writer:
                    while chunk := reader.read(CHUNK_SIZE):
                        writer.write(chunk)
                        size += len(chunk)
                        if name == "MoviesSE.exe":
                            executable_digest.update(chunk)
                file_records.append({"path": name, "size": size})
            bootstrap = launch_script()
            registry = registry_script()
            archive.writestr(zip_info("launch.bat"), bootstrap)
            archive.writestr(zip_info("movies-web.reg"), registry)
        manifest = {
            "schemaVersion": 1,
            "archive": output.name,
            "compression": "stored",
            "fileCount": len(file_records),
            "totalBytes": sum(item["size"] for item in file_records),
            "archiveBytes": Path(archive_temp).stat().st_size,
            "executable": {"path": "MoviesSE.exe", "sha256": executable_digest.hexdigest()},
            "version": read_version(source),
            "files": file_records,
            "generatedFiles": [{"path": "launch.bat", "size": len(bootstrap)},
                               {"path": "movies-web.reg", "size": len(registry)}],
            "registrySettingsVerified": False,
        }
        fd, manifest_temp = tempfile.mkstemp(prefix=".manifest-", suffix=".json", dir=output.parent)
        temporary.append(manifest_temp)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
        # Both files are fully built before publishing. Each replacement is atomic;
        # consumers should avoid reading the pair while this command is running.
        os.replace(archive_temp, output)
        os.replace(manifest_temp, manifest_path)
        return manifest
    finally:
        for path in temporary:
            Path(path).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    try:
        manifest = prepare_game(args.source, args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Packaging failed: {error}\n")
    print(f"Packaged {manifest['fileCount']} original files ({manifest['totalBytes']:,} bytes) plus launch.bat and movies-web.reg")
    print(f"Archive: {args.output}")
    print(f"Manifest: {args.output.parent / 'manifest.json'}")


if __name__ == "__main__":
    main()
