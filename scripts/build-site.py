#!/usr/bin/env python3
"""Assemble the public website in dist/: the play page and only what it needs.

Usage: python3 scripts/build-site.py [--output dist] [--report-endpoint https://.../report]

--report-endpoint (or MOVIES_REPORT_ENDPOINT) turns on error reports for this
build: the page sends a report to that URL when the game stops with an error
(public/crash-report.js; collector in workers/crash-reports). Without it, and on
local hosts, nothing is ever sent. The git commit is stamped as the site version.

Copied: play.html (as index.html), the scripts it loads (followed transitively
through import/importScripts/Worker/URL string literals), the O1 threaded game
build and the ffmpeg.wasm decoder. Never copied: game files (public/game), the
original MoviesSE.exe that staging puts next to the build, the diagnostic pages
(static-probe.html, Boxedwine launcher) and probe fixtures. Preview with
`node scripts/serve.mjs --root dist`.
"""

import argparse
import gzip
import hashlib
import html
import os
import subprocess
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / 'public'
BUILD = 'runtime/static-probe-o1-threads'
RUNTIME_FILES = [f'{BUILD}/manifest.json', f'{BUILD}/probe.js', f'{BUILD}/probe.wasm',
                 'runtime/media-probe/ffmpeg-core.js', 'runtime/media-probe/ffmpeg-core.wasm']
FORBIDDEN = re.compile(r'(^|/)(game/|MoviesSE\.exe$|static-probe\.html$|.*\.wmv$|.*\.asf$)', re.I)
# String literals naming a sibling script: './x.js', '/x.js', 'x.js'.
SCRIPT = re.compile(r"""['"`]\.?/?([A-Za-z0-9_-]+\.(?:js|css))['"`]""")
HEADERS = """/v/*
  Cache-Control: public, max-age=31536000, immutable
/
  Cache-Control: no-cache
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Content-Type-Options: nosniff
"""
LIMIT = 25 * 1024 * 1024  # Cloudflare Pages / Workers static asset per-file limit


def page_files():
    """play.html, play.css and every script reachable from them."""
    pending, found = ['play.html'], set()
    while pending:
        name = pending.pop()
        if name in found or not (PUBLIC / name).is_file():
            continue
        found.add(name)
        if not name.endswith(('.html', '.js', '.css')):
            continue
        text = (PUBLIC / name).read_text(encoding='utf-8')
        if name.endswith('.html'):
            pending += re.findall(r'(?:src|href|content)="((?:img/)?[A-Za-z0-9_-]+\.(?:js|css|jpg|png|webp|svg))"', text)
        pending += SCRIPT.findall(text)
    return sorted(found)


def site_version():
    try:
        commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(['git', 'status', '--porcelain'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        return commit + ('-dirty' if dirty else '')
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def stamp(page, endpoint, version=''):
    attributes = f' data-site-version="{html.escape(version + "+" + site_version())}"'
    if endpoint:
        if not endpoint.startswith('https://'):
            raise SystemExit('--report-endpoint must be an https:// URL')
        attributes += f' data-report-endpoint="{html.escape(endpoint)}"'
    marker = '<body data-mode="play">'
    if marker not in page:
        raise SystemExit('play.html body marker missing')
    return page.replace(marker, f'<body data-mode="play"{attributes}>', 1)


KEEP_VERSIONS = 3
COMPRESS = {'.wasm', '.js', '.css', '.html', '.json', '.svg'}
ROOT_PAGE = None  # the root index is the play page with a <base> pointing at the current release


def build(output, endpoint=''):
    """dist/index.html is the play page with <base href="/v/<content hash>/">; that folder never changes once
    written: a CDN and browsers may cache everything under v/ forever, and only the
    small root page must be revalidated. Each compressible file gets a .gz twin that
    scripts/serve.mjs sends to clients accepting gzip (126 MB of Wasm becomes ~16 MB)."""
    if output.resolve() in (ROOT, PUBLIC) or PUBLIC in output.resolve().parents:
        raise SystemExit('Output must be outside public/')
    files = page_files() + RUNTIME_FILES
    contents = {}
    for name in files:
        if FORBIDDEN.search(name):
            raise SystemExit(f'Refusing to publish {name}')
        source = PUBLIC / name
        if not source.is_file():
            raise SystemExit(f'Missing {name}; run the build and stage-static-browser.py first')
        contents[name] = source.read_bytes()
    digest = hashlib.sha256(endpoint.encode())
    for name in sorted(contents):
        digest.update(name.encode() + b'\0' + hashlib.sha256(contents[name]).digest())
    version = digest.hexdigest()[:12]
    contents['play.html'] = stamp(contents['play.html'].decode('utf-8'), endpoint, version).encode('utf-8')
    versions = output / 'v'
    target_root = versions / version
    if not target_root.exists():
        staging = versions / f'.{version}.tmp'
        if staging.exists():
            shutil.rmtree(staging)
        for name, data in contents.items():
            target = staging / ('index.html' if name == 'play.html' else name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if target.suffix in COMPRESS and len(data) > 1024:
                with open(str(target) + '.gz', 'wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', compresslevel=9, mtime=0) as packed:
                    packed.write(data)
        staging.rename(target_root)  # complete versions only; never a half-written one
    # The root URL serves the play page itself; <base> makes every relative URL
    # (scripts, workers, images, runtime) load from the immutable release folder.
    page = contents['play.html'].decode('utf-8')
    if '<head>' not in page:
        raise SystemExit('play.html <head> missing')
    (output / 'index.html').write_text(page.replace('<head>', f'<head><base href="/v/{version}/">', 1), encoding='utf-8')
    (output / '_headers').write_text(HEADERS)
    old = sorted((d for d in versions.iterdir() if d.is_dir() and not d.name.startswith('.') and d != target_root), key=lambda d: d.stat().st_mtime)
    for stale in old[:max(0, len(old) - (KEEP_VERSIONS - 1))]:
        shutil.rmtree(stale)
    raw_total = sum(p.stat().st_size for p in target_root.rglob('*') if p.is_file() and p.suffix != '.gz')
    gz_total = sum((p.with_name(p.name + '.gz') if p.with_name(p.name + '.gz').exists() else p).stat().st_size for p in target_root.rglob('*') if p.is_file() and p.suffix != '.gz')
    print(f'Site version {version}: {len(files)} files, {raw_total / 1e6:.1f} MB ({gz_total / 1e6:.1f} MB gzip) in {target_root}; error reports {"to " + endpoint if endpoint else "off"}')
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--report-endpoint', default=os.environ.get('MOVIES_REPORT_ENDPOINT', ''))
    args = parser.parse_args()
    build(args.output, args.report_endpoint)


if __name__ == '__main__':
    main()
