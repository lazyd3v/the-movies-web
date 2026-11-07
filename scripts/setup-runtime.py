#!/usr/bin/env python3
"""Fetch the pinned Boxedwine web runtime; no third-party Python packages needed.

Use --cache-dir /tmp to reuse previously downloaded archives. Every downloaded,
cached, or existing file must match its pinned SHA-256. Modified runtime files
are never silently overwritten. The source game is not accessed by this script.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
BASE = "https://www.boxedwine.org/builds/demos/build/master/121/mt-jit/"
COMMIT = "26bf258ab0dc00aa1ec6da81d1ca263f922f8947"
SOURCES = [
    {"name": "build-121.zip", "cacheName": "movies-boxedwine-build121.zip",
     "url": "https://www.boxedwine.org/builds/branches/master/builds/121/artifacts/build-121.zip",
     "sha256": "bf5183c04e287097ec563cd761c9b4bc02f48123fcb90843cb6021cd39f771d6"},
    {"name": "TinyCore15Wine11.0-web.zip", "cacheName": "TinyCore15Wine11.0-web.zip",
     "url": BASE + "TinyCore15Wine11.0-web.zip",
     "sha256": "ae2ed179324bde6e3605c2ddeb27406654790077f1885d670d1f402800dfbdf4"},
    {"name": "fsfs.js", "cacheName": "movies-bw-fsfs.js",
     "url": f"https://raw.githubusercontent.com/danoon2/Boxedwine/{COMMIT}/project/emscripten/fsfs.js",
     "sha256": "45b4658ca0666203c08ce1c55022702a03a7355a4134b8d23bc95cee5beb8e1e"},
    {"name": "LICENSE-Boxedwine.txt", "cacheName": "movies-boxedwine-license.txt",
     "url": f"https://raw.githubusercontent.com/danoon2/Boxedwine/{COMMIT}/license.txt",
     "sha256": "32b1062f7da84967e7019d01ab805935caa7ab7321a7ced0e30ebe75e5df1670"},
]
WEB_FILES = ("boxedwine-shell.js", "boxedwine.html", "boxedwine.wasm", "boxedwine.css", "boxedwine.js")
ALTERNATE_VARIANTS = {"interpreter": "MultiThreaded", "single-jit": "SingleThreadedJit"}


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as reader:
        while chunk := reader.read(1024 * 1024):
            result.update(chunk)
    return result.hexdigest()


def verify(path, expected):
    if digest(path) != expected:
        raise ValueError(f"SHA-256 mismatch: {path}; refusing to use or overwrite it")
    return path


def obtain(source, cache, output):
    candidates = [output / source["name"], cache / source["cacheName"], cache / source["name"]]
    for path in candidates:
        if path.is_file():
            return verify(path, source["sha256"])
    target = cache / source["cacheName"]
    fd, temporary = tempfile.mkstemp(prefix=".download-", dir=cache)
    try:
        request = urllib.request.Request(source["url"], headers={"User-Agent": "TheMoviesWeb-runtime-setup/1"})
        with os.fdopen(fd, "wb") as writer, urllib.request.urlopen(request, timeout=90) as reader:
            shutil.copyfileobj(reader, writer, length=1024 * 1024)
        verify(Path(temporary), source["sha256"])
        os.replace(temporary, target)
        return target
    finally:
        Path(temporary).unlink(missing_ok=True)


def install(staged, target, output_root=None):
    expected = digest(staged)
    if target.exists():
        verify(target, expected)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        os.replace(staged, target)
    relative = target.relative_to(output_root).as_posix() if output_root is not None else target.name
    return {"path": relative, "size": target.stat().st_size, "sha256": expected}


def setup(cache, output):
    cache.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    downloaded = [obtain(source, cache, output) for source in SOURCES]
    records = []
    with tempfile.TemporaryDirectory(prefix=".runtime-", dir=output) as directory:
        staging = Path(directory)
        with zipfile.ZipFile(downloaded[0]) as archive:
            members = [("Web/MultiThreadedJit/" + name, name) for name in WEB_FILES]
            members.extend((f"Web/{variant}/{name}", f"{folder}/{name}")
                           for folder, variant in ALTERNATE_VARIANTS.items()
                           for name in ("boxedwine.js", "boxedwine.wasm"))
            for member, relative in members:
                # Explicit member allowlist prevents archive path traversal.
                target = staging / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(member) as reader, target.open("wb") as writer:
                    shutil.copyfileobj(reader, writer, length=1024 * 1024)
        for source, path in zip(SOURCES[1:], downloaded[1:]):
            shutil.copyfile(path, staging / source["name"])
        # Validate all existing outputs before making any new runtime files visible.
        staged_files = sorted(path for path in staging.rglob("*") if path.is_file())
        for path in staged_files:
            target = output / path.relative_to(staging)
            if target.exists():
                verify(target, digest(path))
        for path in staged_files:
            records.append(install(path, output / path.relative_to(staging), output))
        manifest = {"schemaVersion": 1, "runtime": "Boxedwine", "build": 121,
                    "variant": "MultiThreadedJit", "licenseSourceCommit": COMMIT,
                    "alternateVariants": ALTERNATE_VARIANTS,
                    "sources": [{k: v for k, v in source.items() if k != "cacheName"} for source in SOURCES],
                    "files": records}
        manifest_temp = staging / "manifest.json"
        manifest_temp.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        os.replace(manifest_temp, output / "manifest.json")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=ROOT / ".cache/runtime")
    parser.add_argument("--output", type=Path, default=ROOT / "public/runtime")
    args = parser.parse_args()
    try:
        manifest = setup(args.cache_dir, args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Runtime setup failed: {error}\n")
    print(f"Verified and installed {len(manifest['files'])} runtime files in {args.output}")


if __name__ == "__main__":
    main()
