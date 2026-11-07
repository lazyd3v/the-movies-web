#!/usr/bin/env python3
"""Install pinned portable Ghidra/JDK tools in the project cache, without Homebrew."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [
    {"name": "ghidra_12.1.4_PUBLIC_20260921.zip", "size": 569649598,
     "sha256": "ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db",
     "url": "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip"},
    {"name": "OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.13_9.tar.gz", "size": 199754598,
     "sha256": "bf036b35b071bfb2f54f2c0af8ee8d88afa95ee6eb3dce02c0f94cfc2524d04a",
     "url": "https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_aarch64_mac_hotspot_21.0.13_9.tar.gz"},
]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def obtain(source, directory):
    target = directory / source["name"]
    if not target.exists():
        partial = target.with_name(target.name + ".partial")
        subprocess.run(["curl", "-fL", "--retry", "2", "--continue-at", "-", source["url"], "-o", str(partial)], check=True)
        if partial.stat().st_size != source["size"] or digest(partial) != source["sha256"]:
            raise ValueError(f"Downloaded archive failed verification: {partial}")
        partial.replace(target)
    if target.stat().st_size != source["size"] or digest(target) != source["sha256"]:
        raise ValueError(f"Archive failed verification; refusing to overwrite: {target}")
    return target


def setup(output):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise ValueError("These pinned tools target macOS ARM64")
    output.mkdir(parents=True, exist_ok=True)
    downloads = output / "downloads"
    downloads.mkdir(exist_ok=True)
    archives = [obtain(source, downloads) for source in SOURCES]
    ghidra = output / "ghidra_12.1.4_PUBLIC"
    jdk = output / "jdk-21.0.13+9"
    if not ghidra.exists():
        with tempfile.TemporaryDirectory(prefix=".ghidra-", dir=output) as temp:
            staging = Path(temp)
            with zipfile.ZipFile(archives[0]) as archive:
                for item in archive.infolist():
                    destination = (staging / item.filename).resolve()
                    if not destination.is_relative_to(staging.resolve()):
                        raise ValueError("Archive path escapes extraction directory")
                    archive.extract(item, staging)
                    mode = item.external_attr >> 16
                    if mode and destination.is_file():
                        destination.chmod(mode & 0o777)
            (staging / ghidra.name).replace(ghidra)
    if not jdk.exists():
        with tempfile.TemporaryDirectory(prefix=".jdk-", dir=output) as temp:
            with tarfile.open(archives[1]) as archive:
                archive.extractall(temp, filter="data")
            (Path(temp) / jdk.name).replace(jdk)
    java_home = jdk / "Contents/Home"
    version = subprocess.run([str(java_home / "bin/java"), "-version"], check=True, capture_output=True, text=True)
    print(version.stderr.strip(), flush=True)
    native_dir = ghidra / "Ghidra/Features/Decompiler/os/mac_arm_64"
    if not (native_dir / "decompile").is_file():
        cpp = ghidra / "Ghidra/Features/Decompiler/src/decompile/cpp"
        for name in ["com_opt", "com_dbg", "ghi_opt", "ghi_dbg", "sla_opt", "sla_dbg"]:
            (cpp / name).mkdir(exist_ok=True)
            (cpp / name / "depend").touch(exist_ok=True)
        print("Building ARM64 Ghidra decompiler with three jobs; see decompiler-build.log", flush=True)
        with (output / "decompiler-build.log").open("w") as log:
            subprocess.run(["make", "-j3", "ghidra_opt", "sleigh_opt", "ARCH_TYPE=-arch arm64", "CXX=clang++ -std=c++11"], cwd=cpp, stdout=log, stderr=subprocess.STDOUT, check=True)
        native_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cpp / "ghidra_opt", native_dir / "decompile")
        shutil.copy2(cpp / "sleigh_opt", native_dir / "sleigh")
    if not os.access(native_dir / "decompile", os.X_OK):
        raise ValueError("Decompiler is not executable")
    manifest = {"schemaVersion": 1, "sources": SOURCES,
                "javaHome": str(java_home.relative_to(output)), "ghidra": ghidra.name,
                "javaVersion": version.stderr.strip(),
                "nativeDecompilerSha256": digest(native_dir / "decompile")}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Analysis tools ready: {output}", flush=True)
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/analysis-tools")
    args = parser.parse_args()
    try:
        setup(args.output.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Analysis tool setup failed: {error}\n")
