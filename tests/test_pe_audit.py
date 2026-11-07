import importlib.util
from pathlib import Path
import struct
import unittest

SPEC = importlib.util.spec_from_file_location('audit_game', Path(__file__).resolve().parents[1] / 'scripts/audit-game.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def fixture():
    data = bytearray(0x800)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x14c, 1, 0, 0, 0, 224, 0x102)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x10b)
    struct.pack_into('<I', data, optional + 16, 0x1000)
    struct.pack_into('<I', data, optional + 28, 0x400000)
    struct.pack_into('<II', data, optional + 56, 0x2000, 0x200)
    struct.pack_into('<I', data, optional + 92, 16)
    struct.pack_into('<II', data, optional + 104, 0x1100, 40)
    struct.pack_into('<II', data, optional + 96 + 13 * 8, 0x1200, 64)
    section = optional + 224
    data[section:section + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, section + 8, 0x1000, 0x1000, 0x600, 0x200)
    struct.pack_into('<5I', data, 0x300, 0x1180, 0, 0, 0x11a0, 0x1180)
    struct.pack_into('<3I', data, 0x380, 0x11c0, 0x80000042, 0)
    data[0x3a0:0x3a0 + 13] = b'KERNEL32.dll\0'
    struct.pack_into('<H', data, 0x3c0, 7)
    data[0x3c2:0x3c2 + 12] = b'ExitProcess\0'
    return data


def valid_fixture():
    data = fixture()
    struct.pack_into('<8I', data, 0x400, 1, 0x1280, 0, 0x1290, 0x1290, 0, 0, 0)
    data[0x480:0x480 + 9] = b'd3d9.dll\0'
    struct.pack_into('<2I', data, 0x490, 0x12b0, 0)
    struct.pack_into('<H', data, 0x4b0, 0)
    value = b'Direct3DCreate9\0'
    data[0x4b2:0x4b2 + len(value)] = value
    return data


class PEAuditTests(unittest.TestCase):
    def test_named_ordinal_and_delay_imports(self):
        pe = audit.PE32(bytes(valid_fixture()))
        self.assertEqual(pe.imports()['KERNEL32.dll'], [{'name': 'ExitProcess', 'hint': 7}, {'ordinal': 66}])
        self.assertEqual(pe.imports(True), {'d3d9.dll': [{'name': 'Direct3DCreate9'}]})
        result = pe.audit('fixture.exe')
        self.assertEqual(result['importCount'], 2)
        self.assertEqual(result['delayImportCount'], 1)
        self.assertEqual(result['imageBase'], '0x400000')

    def test_delay_import_virtual_address_form(self):
        data = valid_fixture()
        struct.pack_into('<8I', data, 0x400, 0, 0x401280, 0, 0x401290, 0x401290, 0, 0, 0)
        struct.pack_into('<I', data, 0x490, 0x4012b0)
        self.assertEqual(audit.PE32(data).imports(True)['d3d9.dll'][0]['name'], 'Direct3DCreate9')

    def test_rejects_truncation(self):
        for length in (0, 2, 63, 0x90, 0x180, 0x7ff):
            with self.subTest(length=length), self.assertRaises(audit.PEError):
                audit.PE32(valid_fixture()[:length])

    def test_rejects_non_pe_and_pe64(self):
        data = valid_fixture()
        data[0x80:0x84] = b'NOPE'
        with self.assertRaises(audit.PEError):
            audit.PE32(data)
        data = valid_fixture()
        struct.pack_into('<H', data, 0x98, 0x20b)
        with self.assertRaises(audit.PEError):
            audit.PE32(data)

    def test_rejects_directory_overflow_and_virtual_tail(self):
        data = valid_fixture()
        struct.pack_into('<I', data, 0x98 + 92, 17)
        with self.assertRaisesRegex(audit.PEError, 'directories'):
            audit.PE32(data)
        pe = audit.PE32(valid_fixture())
        with self.assertRaisesRegex(audit.PEError, 'backed'):
            pe.offset(0x1700)

    def test_rejects_unterminated_directory(self):
        data = valid_fixture()
        struct.pack_into('<I', data, 0x98 + 108, 20)
        with self.assertRaisesRegex(audit.PEError, 'terminator'):
            audit.PE32(data).imports()

    def test_rejects_unmapped_lookup(self):
        data = valid_fixture()
        struct.pack_into('<I', data, 0x300, 0xfffffff0)
        with self.assertRaises(audit.PEError):
            audit.PE32(data).imports()

    def test_absent_directories(self):
        data = valid_fixture()
        struct.pack_into('<II', data, 0x98 + 104, 0, 0)
        struct.pack_into('<II', data, 0x98 + 96 + 13 * 8, 0, 0)
        pe = audit.PE32(data)
        self.assertEqual(pe.imports(), {})
        self.assertEqual(pe.imports(True), {})


if __name__ == '__main__':
    unittest.main()
