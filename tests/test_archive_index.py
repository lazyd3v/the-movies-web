import hashlib
import importlib.util
import struct
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location('archive_index', Path(__file__).resolve().parents[1] / 'scripts/index-game-archive.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ArchiveIndexTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.archive = Path(self.tmp.name) / 'movies.zip'

    def make(self, entries=None, compression=zipfile.ZIP_STORED):
        entries = entries or [('MoviesSE.exe', b'MZoriginal'), ('Data/a.pak', b'asset')]
        with zipfile.ZipFile(self.archive, 'w', compression=compression) as z:
            for name, data in entries:
                with z.open(name, 'w', force_zip64=True) as stream:
                    stream.write(data)
            z.writestr('launch.bat', b'generated')
            z.writestr('movies-web.reg', b'generated')
        originals = [{'path': name, 'size': len(data)} for name, data in entries]
        return {'files': originals, 'fileCount': len(originals), 'archiveBytes': self.archive.stat().st_size,
                'executable': {'path': 'MoviesSE.exe', 'sha256': hashlib.sha256(b'MZoriginal').hexdigest()}}

    def test_zip64_local_offsets_and_hash(self):
        manifest = self.make()
        result = module.index_archive(self.archive, manifest)
        data = self.archive.read_bytes()
        self.assertEqual(result['fileCount'], 2)
        self.assertEqual(result['archiveSha256'], hashlib.sha256(data).hexdigest())
        expected = {'MoviesSE.exe': b'MZoriginal', 'Data/a.pak': b'asset'}
        for entry in result['files']:
            self.assertEqual(data[entry['offset']:entry['offset']+entry['size']], expected[entry['path']])

    def test_paths_and_duplicates(self):
        for name in ['../bad', '/bad', 'Data\\bad', 'C:bad', 'a//bad', './bad', 'a/../bad']:
            with self.subTest(name=name):
                manifest = self.make([('MoviesSE.exe', b'MZoriginal'), (name, b'x')])
                with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            manifest = self.make([('MoviesSE.exe', b'MZoriginal'), ('moviesse.EXE', b'x')])
        with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)

    def test_reject_compression_and_bad_executable(self):
        manifest = self.make(compression=zipfile.ZIP_DEFLATED)
        with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)
        manifest = self.make()
        manifest['executable']['sha256'] = '0'*64
        with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)

    def test_local_header_mismatch_and_encryption(self):
        for offset, value in [(8, 8), (6, 1)]:
            manifest = self.make()
            data = bytearray(self.archive.read_bytes())
            struct.pack_into('<H', data, offset, value)
            self.archive.write_bytes(data)
            with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)

    def test_out_of_bounds_size(self):
        manifest = self.make()
        data = bytearray(self.archive.read_bytes())
        at = data.index(b'PK\x01\x02')
        struct.pack_into('<I', data, at+20, 0x70000000)
        struct.pack_into('<I', data, at+24, 0x70000000)
        self.archive.write_bytes(data)
        with self.assertRaises(ValueError): module.index_archive(self.archive, manifest)

if __name__ == '__main__': unittest.main()
