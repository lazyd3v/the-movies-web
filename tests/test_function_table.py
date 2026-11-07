"""Compile the exact patched table in isolation; requires the cached pinned toolkit."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / ".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/runtime/runtime.c"


class FunctionTableTests(unittest.TestCase):
    @unittest.skipUnless(SOURCE.exists() and shutil.which("clang"), "cached runtime and clang required")
    def test_pinned_patch_growth_collision_and_failure_guards(self):
        self.assertEqual(hashlib.sha256(SOURCE.read_bytes()).hexdigest(), "4f7dc0c579a486b9a8c3ac87a271c28ff03dcfd311588838c6929da807ef6cbb")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "runtime").mkdir()
            shutil.copyfile(SOURCE, work / "runtime/runtime.c")
            subprocess.run(["git", "apply", str(ROOT / "patches/retro-port-function-table.patch")], check=True, cwd=work)
            modified = (work / "runtime/runtime.c").read_text()
            start = modified.index("typedef struct { uint32_t addr; GuestFn fn; } FnEntry;")
            end = modified.index("static void unimplemented_import", start)
            extracted = work / "table.c"
            extracted.write_text(modified[start:end])
            binary = work / "test-table"
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            '-DFUNCTION_TABLE_SOURCE="' + str(extracted) + '"',
                            str(ROOT / "tests/test_function_table.c"), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("101024 registrations", result.stdout)


if __name__ == "__main__":
    unittest.main()
