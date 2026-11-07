"""Fail-closed checks for the optional static translation probe (requires its venv)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


probe = module("probe", ROOT / "scripts/probe-recompilation.py")
tool = module("x86toc", ROOT / ".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/translator/x86toc.py")


class Guardrails(unittest.TestCase):
    def test_changed_input_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input"
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                probe.checked(path, probe.sha(b"original"))

    def test_non_leaf_escaping_trapping_or_incomplete_code_rejected(self):
        # Synthetic test snippets, not original executable bytes.
        for code in [b"\xe8\0\0\0\0\xc3", b"\xeb\x7f", b"\x0f\x0b", b"\x0f"]:
            with self.subTest(code=code), self.assertRaises(ValueError):
                probe.translate_leaf(tool, code, 0x1000, 0x1000, 0x1000 + len(code), "synthetic")


if __name__ == "__main__":
    unittest.main()
