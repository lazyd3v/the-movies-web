#!/usr/bin/env python3
"""Prepare the original Microsoft WMP10 media DLLs as a local Wine overlay.

The pinned installer is only extracted, never executed. Its original EULA is
preserved; this script does not accept license terms or publish the binaries.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE_URL = (
    "https://web.archive.org/web/20200803205216/"
    "https://download.microsoft.com/download/1/2/a/"
    "12a31f29-2fa9-4f50-b95d-e45ef7013f87/MP10Setup.exe"
)
SOURCE_SHA256 = "c1e71784c530035916aad5b09fa002abfbb7569b75208dd79351f29c6d197e03"
PROFILE_SHA256 = "95f1edbc3cb9aa70a374a61b43bd16818636b0190aa841504130bb718c39c2c7"
DRIVE = "home/username/.wine/drive_c/"
DLLS = (
    "qasf", "wmvcore", "wmasf", "wmadmod", "wmadmoe", "wmvdmod",
    "wmvdmoe2", "wmsdmod", "wmsdmoe2", "wmspdmod", "wmspdmoe",
    "wmvadvd", "wmvadve", "wmnetmgr", "laprxy",
    # Original WM runtime dependencies, preserved byte-for-byte. In particular,
    # wmvcore loads these through delay imports / LoadLibrary, so inspecting
    # only its ordinary PE import table misses them.
    "drmclien", "drmstor", "drmv2clt", "wmidx", "asferror", "wmerror",
)
# Registration lists in the installer's WMFSDK10.inf and codecs10.inf.
REGISTER = (
    "drmclien", "drmstor", "drmv2clt",
    "wmadmod", "wmsdmod", "wmspdmod", "wmvdmod", "wmvadvd",
    "wmadmoe", "wmsdmoe2", "wmspdmoe", "wmvdmoe2", "wmvadve",
    "laprxy", "wmnetmgr", "wmvcore", "qasf",
)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def verify(path):
    if digest(path) != SOURCE_SHA256:
        raise ValueError(f"Installer SHA-256 mismatch: {path}")
    return path


def installer_file(args, cache):
    if args.installer:
        return verify(args.installer.resolve())
    target = cache / "MP10Setup.exe"
    if target.exists():
        return verify(target)
    temporary = cache / "MP10Setup.exe.part"
    request = urllib.request.Request(SOURCE_URL, headers={"User-Agent": "TheMoviesWeb-media-setup/1"})
    try:
        with urllib.request.urlopen(request, timeout=90) as reader, temporary.open("wb") as writer:
            shutil.copyfileobj(reader, writer, 1024 * 1024)
        verify(temporary)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def bootstrap():
    lines = ["@echo off", "if exist C:\\media-setup-v2.done goto media_setup_ready", "echo MEDIA_SETUP_BEGIN"]
    for name in DLLS:
        lines += [
            f'reg add "HKCU\\Software\\Wine\\DllOverrides" /v {name} /t REG_SZ /d native,builtin /f',
            f"echo MEDIA_OVERRIDE {name} %ERRORLEVEL%",
            "if errorlevel 1 goto media_setup_failed",
        ]
    for name in REGISTER:
        lines += [
            f"regsvr32 /s C:\\windows\\system32\\{name}.dll",
            f"echo MEDIA_REGISTER {name} %ERRORLEVEL%",
            "if errorlevel 1 goto media_setup_failed",
        ]
    lines += [
        "echo Registered WMP10 media components>C:\\media-setup-v2.done",
        ":media_setup_ready",
        "if exist C:\\media-codec-reg-v1.done goto media_codecs_ready",
        "regedit /S C:\\media-codecs.reg",
        "echo MEDIA_CODEC_REGISTRY %ERRORLEVEL%",
        "if errorlevel 1 goto media_setup_failed",
        "echo Installed original WMFSDK10 codec registry>C:\\media-codec-reg-v1.done",
        ":media_codecs_ready", "echo MEDIA_SETUP_READY",
        'if "%MOVIES_TRACE%"=="1" set WINEDEBUG=+ole,+loaddll,+quartz,+wmvcore',
        'if "%MOVIES_TRACE%"=="graphics" set WINEDEBUG=+d3d,+wgl,+loaddll',
        "call C:\\files\\launch.bat", "goto :eof",
        ":media_setup_failed", "echo MEDIA_SETUP_FAILED", "exit /b 1", "",
    ]
    return "\r\n".join(lines).encode("ascii")


def system_profiles(data):
    """Read the original PRXFILE/200 PE resource without running vendor code."""
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid WMVCORE PE header")
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x10b:
        raise ValueError("Expected 32-bit WMVCORE")
    section_table = optional + struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<IIII", data, section_table + i * 40 + 8)
                for i in range(struct.unpack_from("<H", data, pe + 6)[0])]

    def offset(rva, size=1):
        for _, virtual, length, raw in sections:
            if virtual <= rva and rva + size <= virtual + length:
                result = raw + rva - virtual
                if result + size <= len(data):
                    return result
        raise ValueError("PE resource outside file-backed sections")

    resource = offset(struct.unpack_from("<I", data, optional + 112)[0])

    def children(relative):
        directory = resource + relative
        named, numbered = struct.unpack_from("<HH", data, directory + 12)
        for i in range(named + numbered):
            name, target = struct.unpack_from("<II", data, directory + 16 + i * 8)
            if name & 0x80000000:
                start = resource + (name & 0x7fffffff)
                length = struct.unpack_from("<H", data, start)[0]
                name = data[start + 2:start + 2 + length * 2].decode("utf-16le")
            yield name, target

    target = next(value for name, value in children(0) if name == "PRXFILE")
    target = next(value for name, value in children(target & 0x7fffffff) if name == 200)
    _, target = next(children(target & 0x7fffffff))
    if target & 0x80000000:
        raise ValueError("Unexpected nested profile resource")
    rva, size = struct.unpack_from("<II", data, resource + target)
    start = offset(rva, size)
    profile = data[start:start + size]
    if hashlib.sha256(profile).hexdigest() != PROFILE_SHA256:
        raise ValueError("Original system-profile resource SHA-256 mismatch")
    return profile


def codec_registry(inf):
    """Preserve exactly WMFSDK10.inf's Reg.Codecs entries, without setup side effects."""
    sections = {}
    section = None
    for line in inf.decode("cp1252").splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            sections[section] = []
        elif line and not line.startswith(";") and section:
            sections[section].append(line)
    if 'REGKEY_WMSDK="WMSDK"' not in sections["Strings"]:
        raise ValueError("Unexpected WMFSDK registry key")
    keys = {}
    for line in sections["Reg.Codecs"]:
        row = next(csv.reader([line], skipinitialspace=True))
        if len(row) != 5 or row[0] != "HKCR" or row[3:] != ["", ""]:
            raise ValueError("Unexpected WMFSDK codec registry entry")
        key = row[1].replace("%REGKEY_WMSDK%", "WMSDK")
        if key not in (r"Windows Media\WMSDK\AudioEncode", r"Windows Media\WMSDK\VideoEncode"):
            raise ValueError("Unexpected WMFSDK codec registry path")
        keys.setdefault(key, []).append(row[2])
    lines = ["Windows Registry Editor Version 5.00", ""]
    for key, values in keys.items():
        lines.append("[HKEY_CLASSES_ROOT\\" + key + "]")
        lines.extend('"' + value + '"=""' for value in values)
        lines.append("")
    return ("\r\n".join(lines) + "\r\n").encode("utf-16"), keys


def write_zip(path, entries):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, content in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (2005, 1, 28, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, content, compresslevel=9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", type=Path, help="Existing original MP10Setup.exe (verified before extraction)")
    parser.add_argument("--cache-dir", type=Path, default=ROOT / ".cache" / "media")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "public" / "media")
    args = parser.parse_args()
    cabextract = shutil.which("cabextract")
    if not cabextract:
        parser.error("cabextract is required to extract the Microsoft installer")
    cache, output = args.cache_dir.resolve(), args.output_dir.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    source = installer_file(args, cache)
    with tempfile.TemporaryDirectory(prefix="extract-", dir=cache) as temporary:
        extracted = Path(temporary)
        subprocess.run([cabextract, "-q", "-d", str(extracted), str(source)], check=True)
        names = {path.name.lower(): path for path in extracted.iterdir() if path.is_file()}
        entries = {}
        components = []
        for name in DLLS:
            filename = name + ".dll"
            if filename not in names:
                raise ValueError(f"Missing component: {filename}")
            data = names[filename].read_bytes()
            if not data.startswith(b"MZ"):
                raise ValueError(f"Invalid PE component: {filename}")
            entries[DRIVE + "windows/system32/" + filename] = data
            components.append({"file": filename, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
        eula = names["eula.txt"].read_bytes()
        profile = system_profiles(names["wmvcore.dll"].read_bytes())
        entries[DRIVE + "windows/wmsyspr9.prx"] = profile
        registry, codec_keys = codec_registry(names["wmfsdk10.inf"].read_bytes())
        entries[DRIVE + "media-codecs.reg"] = registry
        entries[DRIVE + "media-setup.bat"] = bootstrap()
        entries[DRIVE + "media-license.txt"] = eula
        staged = extracted / "media.zip"
        write_zip(staged, entries)
        manifest = {
            "schemaVersion": 1,
            "description": "Original WMP10 media components for Wine; game compatibility remains under test",
            "source": {"url": SOURCE_URL, "sha256": SOURCE_SHA256, "file": "MP10Setup.exe"},
            "archive": {"file": "media.zip", "sha256": digest(staged), "bytes": staged.stat().st_size},
            "components": components,
            "systemProfiles": {"file": "C:/windows/wmsyspr9.prx", "sourceResource": "WMVCORE.DLL/PRXFILE/200", "sha256": PROFILE_SHA256, "bytes": len(profile)},
            "codecRegistry": {"source": "WMFSDK10.inf/Reg.Codecs", "keys": codec_keys, "sha256": hashlib.sha256(registry).hexdigest()},
            "bootstrap": "C:/media-setup.bat",
            "dllOverrides": ",".join(DLLS) + "=n,b",
            "license": {"file": "LICENSE-WMP10.txt", "sha256": hashlib.sha256(eula).hexdigest(), "acceptedByScript": False},
        }
        # Only generated outputs are replaced; source installer/game remain untouched.
        shutil.copyfile(staged, output / "media.zip.tmp")
        os.replace(output / "media.zip.tmp", output / "media.zip")
        (output / "LICENSE-WMP10.txt").write_bytes(eula)
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(DLLS)} native media DLLs: {output / 'media.zip'}")
    print(f"SHA-256: {manifest['archive']['sha256']}")


if __name__ == "__main__":
    main()
