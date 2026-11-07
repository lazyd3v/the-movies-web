import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "setup_media", Path(__file__).resolve().parents[1] / "scripts/setup-media.py"
)
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


class CodecRegistryTests(unittest.TestCase):
    INF = b'''[DefaultInstall]
AddReg=Reg.Codecs,Unrelated
[Reg.Codecs]
HKCR, "Windows Media\\%REGKEY_WMSDK%\\AudioEncode", "353",,""
HKCR, "Windows Media\\%REGKEY_WMSDK%\\VideoEncode", "WMV3",,""
[Unrelated]
HKLM,"Software\\Unrelated","DoNotImport",,"yes"
[Strings]
REGKEY_WMSDK="WMSDK"
'''

    def test_imports_only_original_codec_section_with_empty_string_values(self):
        data, keys = media.codec_registry(self.INF)
        self.assertEqual(data[:2], b"\xff\xfe")
        self.assertEqual(data.decode("utf-16"),
            'Windows Registry Editor Version 5.00\r\n\r\n'
            '[HKEY_CLASSES_ROOT\\Windows Media\\WMSDK\\AudioEncode]\r\n'
            '"353"=""\r\n\r\n'
            '[HKEY_CLASSES_ROOT\\Windows Media\\WMSDK\\VideoEncode]\r\n'
            '"WMV3"=""\r\n\r\n')
        self.assertEqual(sum(map(len, keys.values())), 2)

    def test_rejects_changed_registry_root_path_or_value_type(self):
        for before, after in [
            (b'HKCR,', b'HKLM,'),
            (b'AudioEncode', b'Unexpected'),
            (b'"353",,""', b'"353",0x10001,1'),
            (b'REGKEY_WMSDK="WMSDK"', b'REGKEY_WMSDK="Other"'),
        ]:
            with self.subTest(after=after), self.assertRaises(ValueError):
                media.codec_registry(self.INF.replace(before, after))

    def test_existing_dll_stamp_cannot_skip_new_codec_registration(self):
        lines = media.bootstrap().decode("ascii").splitlines()
        target = lines.index(":media_setup_ready")
        codec_import = lines.index("regedit /S C:\\media-codecs.reg")
        codec_done = lines.index(":media_codecs_ready")
        launch = lines.index("call C:\\files\\launch.bat")
        self.assertIn("if exist C:\\media-setup-v2.done goto media_setup_ready", lines[:target])
        self.assertLess(target, codec_import)
        self.assertLess(codec_import, codec_done)
        self.assertLess(codec_done, launch)
        self.assertIn("if errorlevel 1 goto media_setup_failed", lines[codec_import:codec_done])
        self.assertIn("if exist C:\\media-codec-reg-v1.done goto media_codecs_ready", lines[target:codec_import])
        self.assertTrue(any("MOVIES_TRACE" in line for line in lines[codec_done:launch]))
        self.assertIn('if "%MOVIES_TRACE%"=="1" set WINEDEBUG=+ole,+loaddll,+quartz,+wmvcore', lines[codec_done:launch])
        self.assertIn('if "%MOVIES_TRACE%"=="graphics" set WINEDEBUG=+d3d,+wgl,+loaddll', lines[codec_done:launch])


if __name__ == "__main__":
    unittest.main()
