import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile


SPEC = importlib.util.spec_from_file_location("prepare_game", Path(__file__).resolve().parents[1] / "scripts/prepare-game.py")
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)


class PrepareGameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "Game Files"
        (self.source / "Data/pak").mkdir(parents=True)
        (self.source / "MoviesSE.exe").write_bytes(b"MZfake executable")
        (self.source / "Data/pak/test.pak").write_bytes(b"asset data")
        (self.source / "version.inf").write_text("[Version Info]\nExtVersion=1.2\nIntVersion=17.0\nLanguage=1\n")
        self.output = self.root / "public/game/movies.zip"

    def test_preserves_originals_and_generates_private_manifest(self):
        originals = {p.relative_to(self.source).as_posix(): p.read_bytes() for p in self.source.rglob("*") if p.is_file()}
        manifest = packager.prepare_game(self.source, self.output)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), set(originals) | {"launch.bat", "movies-web.reg"})
            for name, contents in originals.items():
                self.assertEqual(archive.read(name), contents)
                self.assertEqual((self.source / name).read_bytes(), contents)
                self.assertEqual(archive.getinfo(name).compress_type, zipfile.ZIP_STORED)
            self.assertIn(b'"%~dp0MoviesSE.exe"', archive.read("launch.bat"))
            self.assertEqual(archive.read("launch.bat").count(b'regedit /s'), 1)
            self.assertNotIn(b'reg add', archive.read("launch.bat"))
            registry = archive.read("movies-web.reg")
            self.assertTrue(registry.startswith(b'REGEDIT4\r\n'))
            self.assertIn(b'"InstallPath"="C:\\\\files\\\\"', registry)
            self.assertEqual(registry.count(b'"GD: Screen Width"=dword:00000320'), 2)
        self.assertEqual(manifest["fileCount"], 3)
        self.assertEqual(manifest["totalBytes"], sum(map(len, originals.values())))
        self.assertEqual(manifest["executable"]["sha256"], hashlib.sha256(originals["MoviesSE.exe"]).hexdigest())
        self.assertEqual(manifest["version"]["ExtVersion"], "1.2")
        self.assertNotIn(str(self.source), json.dumps(manifest))
        self.assertFalse((self.source / "launch.bat").exists())
        self.assertFalse((self.source / "movies-web.reg").exists())
        self.assertEqual({item['path'] for item in manifest['generatedFiles']}, {'launch.bat', 'movies-web.reg'})

    def test_rejects_generated_file_collisions_case_insensitively(self):
        for name in ('LAUNCH.BAT', 'Movies-Web.REG'):
            with self.subTest(name=name):
                path = self.source / name
                path.write_bytes(b'original')
                with self.assertRaisesRegex(ValueError, "conflicts"):
                    packager.prepare_game(self.source, self.output)
                self.assertEqual(path.read_bytes(), b'original')
                path.unlink()

    def test_is_reproducible(self):
        packager.prepare_game(self.source, self.output)
        archive = self.output.read_bytes()
        manifest = (self.output.parent / "manifest.json").read_bytes()
        packager.prepare_game(self.source, self.output)
        self.assertEqual(archive, self.output.read_bytes())
        self.assertEqual(manifest, (self.output.parent / "manifest.json").read_bytes())

    def test_rejects_invalid_game_without_touching_output(self):
        self.output.parent.mkdir(parents=True)
        self.output.write_bytes(b"previous")
        (self.source / "MoviesSE.exe").write_bytes(b"not executable")
        with self.assertRaisesRegex(ValueError, "MZ"):
            packager.prepare_game(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous")

    def test_rejects_empty_archives(self):
        (self.source / "Data/pak/test.pak").write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "nonempty"):
            packager.prepare_game(self.source, self.output)

    def test_rejects_output_inside_originals(self):
        with self.assertRaisesRegex(ValueError, "outside"):
            packager.prepare_game(self.source, self.source / "movies.zip")

    def test_mid_build_failure_keeps_previous_archive_and_cleans_temps(self):
        self.output.parent.mkdir(parents=True)
        self.output.write_bytes(b"previous")
        with mock.patch.object(packager, "launch_script", side_effect=OSError("disk failure")):
            with self.assertRaisesRegex(OSError, "disk failure"):
                packager.prepare_game(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous")
        self.assertEqual(list(self.output.parent.glob(".movies-*")), [])


if __name__ == "__main__":
    unittest.main()
