#!/usr/bin/env python3
"""Statically audit original PE32 executables; never execute or modify them.

Usage: python3 scripts/audit-game.py [GAME_FILES] [--output audit.json]
Optional --media-directory inspects DLLs listed by scripts/setup-media.py.
"""
import argparse
import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path(__file__).resolve().parents[1] / ".cache/game-files"


class PEError(ValueError):
    pass


class PE32:
    def __init__(self, data):
        self.data = data
        if self.span(0, 2) != b'MZ':
            raise PEError('Missing DOS MZ signature')
        pe = self.unpack('<I', 60)[0]
        if self.span(pe, 4) != b'PE\0\0':
            raise PEError('Missing PE signature')
        self.machine, count, _, _, _, optional_size, self.characteristics = self.unpack('<HHIIIHH', pe + 4)
        optional = pe + 24
        self.span(optional, optional_size)
        if optional_size < 96 or self.unpack('<H', optional)[0] != 0x10b:
            raise PEError('Expected PE32 optional header')
        self.entry = self.unpack('<I', optional + 16)[0]
        self.base = self.unpack('<I', optional + 28)[0]
        self.image_size, self.header_size = self.unpack('<II', optional + 56)
        self.dll_characteristics = self.unpack('<H', optional + 70)[0]
        directory_count = self.unpack('<I', optional + 92)[0]
        if directory_count > (optional_size - 96) // 8:
            raise PEError('Data directories exceed optional header')
        self.directories = [self.unpack('<II', optional + 96 + i * 8) for i in range(directory_count)]
        self.sections = []
        self.span(optional + optional_size, count * 40)
        for i in range(count):
            at = optional + optional_size + i * 40
            virtual_size, virtual_address, raw_size, raw_offset = self.unpack('<IIII', at + 8)
            if raw_size:
                self.span(raw_offset, raw_size)
            self.sections.append({'name': self.span(at, 8).split(b'\0')[0].decode('ascii', 'replace'),
                                  'virtualAddress': virtual_address, 'virtualSize': virtual_size,
                                  'rawOffset': raw_offset, 'rawSize': raw_size,
                                  'characteristics': self.unpack('<I', at + 36)[0]})

    def span(self, at, size):
        if at < 0 or size < 0 or at > len(self.data) or size > len(self.data) - at:
            raise PEError(f'File range out of bounds: {at}+{size}')
        return self.data[at:at + size]

    def unpack(self, fmt, at):
        return struct.unpack(fmt, self.span(at, struct.calcsize(fmt)))

    def directory(self, index):
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def offset(self, rva, size=1):
        if rva < 0 or size < 0 or rva + size > 0x100000000:
            raise PEError('Invalid RVA')
        if rva < self.header_size and size <= self.header_size - rva:
            self.span(rva, size)
            return rva
        for s in self.sections:
            delta = rva - s['virtualAddress']
            # Reject zero-filled virtual tails; they have no bytes in the file.
            if 0 <= delta <= s['rawSize'] and size <= s['rawSize'] - delta:
                at = s['rawOffset'] + delta
                self.span(at, size)
                return at
        raise PEError(f'RVA not backed by file: {rva:#x}')

    def string(self, rva):
        value = bytearray()
        # DLL/API names are small; cap work on malformed or hostile input.
        for i in range(4096):
            char = self.data[self.offset(rva + i)]
            if not char:
                try:
                    return value.decode('ascii')
                except UnicodeDecodeError as error:
                    raise PEError('Non-ASCII import name') from error
            value.append(char)
        raise PEError('Unterminated or oversized import name')

    def imports(self, delayed=False):
        rva, size = self.directory(13 if delayed else 1)
        if not rva and not size:
            return {}
        stride = 32 if delayed else 20
        if not rva or size < stride:
            raise PEError('Invalid import directory')
        result = {}
        for displacement in range(0, size - stride + 1, stride):
            fields = self.unpack('<8I' if delayed else '<5I', self.offset(rva + displacement, stride))
            if not any(fields):
                return result
            if delayed:
                attributes, name, _, iat, table, _, _, _ = fields
                if attributes & ~1:
                    raise PEError('Unsupported delay-import attributes')
                convert = (lambda address: address) if attributes & 1 else (lambda address: address - self.base)
                name, table = convert(name), convert(table or iat)
            else:
                table, _, _, name, iat = fields
                table = table or iat
                convert = lambda address: address
            if not table:
                raise PEError('Missing import lookup table')
            dll = self.string(name)
            entries = []
            for index in range(65536):
                value = self.unpack('<I', self.offset(table + index * 4, 4))[0]
                if not value:
                    break
                if value & 0x80000000:
                    entries.append({'ordinal': value & 0xffff})
                else:
                    address = convert(value)
                    hint = self.unpack('<H', self.offset(address, 2))[0]
                    entry = {'name': self.string(address + 2)}
                    if not delayed:
                        entry['hint'] = hint
                    entries.append(entry)
            else:
                raise PEError('Unterminated or oversized import lookup table')
            result.setdefault(dll, []).extend(entries)
        raise PEError('Import directory has no terminator')

    def audit(self, filename):
        imports, delayed = self.imports(), self.imports(True)
        strings = [s.decode('ascii') for s in re.findall(rb'[ -~]{4,}', self.data)]
        dlls = sorted({m.group(0) for s in strings for m in re.finditer(r'(?i)[a-z0-9_+.-]+\.dll\b', s)})
        def directory(index):
            rva, size = self.directory(index)
            return {'rva': hex(rva), 'size': size}
        return {'file': filename, 'sha256': hashlib.sha256(self.data).hexdigest(), 'size': len(self.data),
                'format': 'PE32', 'machine': hex(self.machine), 'entryRva': hex(self.entry),
                'imageBase': hex(self.base), 'imageSize': self.image_size,
                'characteristics': hex(self.characteristics), 'dllCharacteristics': hex(self.dll_characteristics),
                'sections': self.sections, 'imports': imports, 'importCount': sum(map(len, imports.values())),
                'importDllCount': len(imports), 'delayImportDirectory': directory(13),
                'tlsDirectory': directory(9), 'relocationDirectory': directory(5),
                'dllStringsNotNecessarilyLoaded': dlls,
                'dynamicApiEvidence': [s for s in strings if s in {'Direct3DCreate9', 'DirectInput8Create',
                    'DirectInputCreateA', 'WMCreateWriter', 'WMCreateProfileManager', 'WMCreateReader',
                    'WMCreateSyncReader', 'DllGetClassObject'}],
                'sourcePathStringCount': sum('.cpp' in s.lower() for s in strings),
                'delayImports': delayed, 'delayImportCount': sum(map(len, delayed.values()))}


def audit_game(source, media_directory=None):
    source = Path(source)
    files = sorted(p for p in source.rglob('*') if p.is_file())
    result = {'method': 'Static PE32 import table and printable ASCII string inspection only; DLL/API strings do not establish executed dependency or exhaustive dynamic calls. No game code executed.',
              'files': [PE32((source / name).read_bytes()).audit(name) for name in ('MoviesSE.exe', 'StarMaker.exe')],
              'installation': {'fileCount': len(files), 'bytes': sum(p.stat().st_size for p in files),
                'extensions': dict(sorted(Counter(p.suffix.lower() for p in files).items())),
                'dllFiles': [p.relative_to(source).as_posix() for p in files if p.suffix.lower() == '.dll'],
                'sourceFiles': [p.relative_to(source).as_posix() for p in files if p.suffix.lower() in {'.c', '.cpp', '.h', '.hpp', '.sln', '.vcxproj', '.vcproj'}]}}
    if media_directory is not None:
        # Read the literal DLL allowlist, without importing/executing setup code.
        module = ast.parse((ROOT / 'scripts/setup-media.py').read_text())
        names = next(ast.literal_eval(n.value) for n in module.body if isinstance(n, ast.Assign)
                     and any(isinstance(t, ast.Name) and t.id == 'DLLS' for t in n.targets))
        available = {p.name.lower(): p for p in Path(media_directory).iterdir() if p.is_file()}
        records = []
        for name in names:
            path = available.get(name + '.dll')
            if path is None:
                raise ValueError(f'Media directory missing {name}.dll')
            pe = PE32(path.read_bytes())
            records.append({'file': path.name, 'machine': hex(pe.machine),
                            'importsByDllCount': {k: len(v) for k, v in pe.imports().items()},
                            'delayImportsByDllCount': {k: len(v) for k, v in pe.imports(True).items()}})
        result['existingNativeMediaOverlay'] = {'origin': 'scripts/setup-media.py DLLS; original Microsoft installer extracted under .cache/wmp10, not part of supplied game folder',
            'dlls': records, 'dllCount': len(records),
            'totalOrdinaryImportEntries': sum(sum(r['importsByDllCount'].values()) for r in records)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path, default=DEFAULT_SOURCE)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/research/pe-import-audit.json')
    parser.add_argument('--media-directory', type=Path)
    args = parser.parse_args()
    try:
        result = audit_game(args.source, args.media_directory)
        # Refuse to write inside the original game installation.
        if args.output.resolve().is_relative_to(args.source.resolve()):
            raise ValueError('Audit output must be outside original game directory')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    except (OSError, ValueError) as error:
        parser.exit(1, f'Audit failed: {error}\n')
    for file in result['files']:
        print(f"{file['file']}: {file['importCount']} ordinary imports, {file['delayImportCount']} delayed imports")
    print(args.output)


if __name__ == '__main__':
    main()
