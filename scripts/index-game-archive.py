#!/usr/bin/env python3
"""Index original ZIP_STORED game files without extracting or modifying them."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GENERATED = {'launch.bat', 'movies-web.reg'}

def path_checked(value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value or any(ord(c) < 32 for c in value):
        raise ValueError('Invalid archive path')
    if any(part in ('', '.', '..') for part in value.split('/')):
        raise ValueError('Archive path must be normalized and relative')
    return value

def digest(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        result.update(block)
    return result.hexdigest()

def index_archive(archive, manifest):
    archive = Path(archive)
    size = archive.stat().st_size
    wanted = {}
    seen = set()
    for record in manifest['files']:
        name = path_checked(record['path'])
        length = record['size']
        if type(length) is not int or length < 0 or name.casefold() in seen:
            raise ValueError('Duplicate path or invalid file size in manifest')
        seen.add(name.casefold())
        wanted[name] = length
    if manifest.get('fileCount') != len(wanted) or manifest.get('archiveBytes') != size:
        raise ValueError('Manifest count/archive size mismatch')
    for name in wanted:
        parts = name.split('/')
        if any('/'.join(parts[:i]).casefold() in seen for i in range(1, len(parts))):
            raise ValueError('File/directory collision')
    executable = manifest['executable']
    exe = path_checked(executable['path'])
    if exe not in wanted:
        raise ValueError('Executable absent from manifest')
    entries, ranges, archive_seen = [], [], set()
    with archive.open('rb') as raw, zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = path_checked(info.filename)
            if name.casefold() in archive_seen:
                raise ValueError('Duplicate ZIP entry')
            archive_seen.add(name.casefold())
            if info.compress_type != zipfile.ZIP_STORED or info.flag_bits & (1 | 64):
                raise ValueError('Only unencrypted ZIP_STORED entries supported')
            if info.compress_size != info.file_size:
                raise ValueError('Stored entry size mismatch')
            start = info.header_offset
            if start < 0 or start + 30 > z.start_dir:
                raise ValueError('Local header outside archive payload')
            raw.seek(start)
            header = raw.read(30)
            fields = struct.unpack('<I5H3I2H', header)
            signature, _, flags, compression, _, _, crc, packed, unpacked, name_len, extra_len = fields
            if signature != 0x04034b50 or flags != info.flag_bits or compression != 0:
                raise ValueError('Local header inconsistent with directory')
            encoded = raw.read(name_len)
            extra = raw.read(extra_len)
            if len(encoded) != name_len or len(extra) != extra_len:
                raise ValueError('Truncated local header')
            decoded = encoded.decode('utf-8' if flags & 0x800 else 'cp437')
            if decoded != name:
                raise ValueError('Local and directory filenames differ')
            if not flags & 8:
                cursor = 0
                while cursor < len(extra):
                    if cursor + 4 > len(extra):
                        raise ValueError('Truncated ZIP extra field')
                    tag, length = struct.unpack_from('<HH', extra, cursor)
                    cursor += 4
                    payload = extra[cursor:cursor + length]
                    if len(payload) != length:
                        raise ValueError('Truncated ZIP extra payload')
                    cursor += length
                    if tag == 1:
                        values = iter(struct.unpack('<' + 'Q' * (len(payload) // 8), payload)) if len(payload) % 8 == 0 else iter(())
                        if unpacked == 0xffffffff:
                            unpacked = next(values, -1)
                        if packed == 0xffffffff:
                            packed = next(values, -1)
                if packed != info.compress_size or unpacked != info.file_size or crc != info.CRC:
                    raise ValueError('Local sizes/CRC inconsistent with directory')
            offset = start + 30 + name_len + extra_len
            end = offset + info.file_size
            if end > z.start_dir or end > size:
                raise ValueError('Entry extends beyond archive payload')
            ranges.append((start, end))
            if name in wanted:
                if wanted[name] != info.file_size:
                    raise ValueError('Manifest file size mismatch')
                entries.append({'path': name, 'size': info.file_size, 'offset': offset})
            elif name not in GENERATED:
                raise ValueError('Unexpected entry absent from original-file manifest')
        if {entry['path'] for entry in entries} != set(wanted):
            raise ValueError('Manifest files missing from archive')
        for left, right in zip(sorted(ranges), sorted(ranges)[1:]):
            if left[1] > right[0]:
                raise ValueError('Overlapping ZIP entries')
        with z.open(exe) as stream:
            exe_hash = digest(stream)
        if exe_hash != executable['sha256']:
            raise ValueError('Executable hash does not match manifest')
        raw.seek(0)
        archive_hash = digest(raw)
    return {'schemaVersion': 1, 'archive': archive.name, 'archiveBytes': size,
            'archiveSha256': archive_hash, 'executable': {'path': exe, 'sha256': exe_hash},
            'fileCount': len(entries), 'totalBytes': sum(e['size'] for e in entries),
            'files': sorted(entries, key=lambda e: e['path'])}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=ROOT / 'public/game/movies.zip')
    parser.add_argument('--manifest', type=Path, default=ROOT / 'public/game/manifest.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'public/game/file-index.json')
    args = parser.parse_args()
    result = index_archive(args.archive, json.loads(args.manifest.read_text()))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=args.output.parent, prefix='.index-')
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
        os.replace(temporary, args.output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f"Indexed {result['fileCount']} files ({result['totalBytes']} bytes)")

if __name__ == '__main__':
    main()
