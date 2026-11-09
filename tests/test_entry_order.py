"""Translated functions whose tail-jump chunk lies below the entry must start at the entry."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_bootstrap", ROOT / "scripts/build-bootstrap.py")
build_bootstrap = importlib.util.module_from_spec(spec)
with mock.patch.object(sys, "argv", ["build-bootstrap.py"]):
    spec.loader.exec_module(build_bootstrap)

# Shape of the generated f_009dd0d0: the chunk at 009db140 (a tail-jump target)
# is emitted first, and the unlabelled entry block follows its `ret`.
SAMPLE = """void f_009dd0d0(CPU *c) { /* FUN_009dd0d0 */
    PREEMPT_POLL(c);
L_009db140:;
    { uint32_t v_ = c->ecx; PUSH32(c, v_); } /* push ecx */
    if (c->zf) goto L_009db1dd; /* je 0x9db1dd */
    c->esp += 4; return; /* ret  */
L_009db1dd:;
    c->esp += 4; return; /* ret  */
    { uint32_t v_ = c->ecx; PUSH32(c, v_); } /* push ecx */
    goto L_009db140; /* jmp 0x9db140 */
}
void f_009dd140(CPU *c) { /* FUN_009dd140 */
    PREEMPT_POLL(c);
    c->eax = (uint32_t)(c->ecx); /* mov eax, ecx */
}
"""


class EntryOrderTests(unittest.TestCase):
    def test_jumps_to_unlabelled_entry_and_leaves_ordinary_functions(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "recomp_142.c"
            source.write_text(SAMPLE)
            with self.assertRaises(SystemExit):  # the guard expects exactly the seven known functions
                build_bootstrap.fix_entry_order([source], work / "out")
            fixed = (work / "out/recomp_142.c").read_text().split("\n")
            self.assertEqual(fixed[2].strip(), "goto L_009dd0d0; /* entry 009dd0d0 follows lower tail-jump chunk */")
            entry = fixed.index("L_009dd0d0:;")
            self.assertEqual(fixed[entry - 1].strip(), "c->esp += 4; return; /* ret  */")
            self.assertIn("/* push ecx */", fixed[entry + 1])
            self.assertEqual(fixed.count("L_009dd0d0:;"), 1)
            self.assertIn("void f_009dd140(CPU *c) { /* FUN_009dd140 */\n    PREEMPT_POLL(c);\n    c->eax",
                          "\n".join(fixed))


class SkipIntroTests(unittest.TestCase):
    def test_finishes_logo_sequence_from_its_start_state(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source = work / "recomp_011.c"
            source.write_text(build_bootstrap.INTRO_UPDATE + "    c->esi = (uint32_t)(c->ecx);\n}\n")
            other = work / "recomp_012.c"
            other.write_text(SAMPLE)
            out = build_bootstrap.skip_intro([source, other], work / "out")
            self.assertEqual(out[1], other)
            lines = out[0].read_text().split("\n")
            self.assertIn("W32(c->ecx + 4u, 6u)", lines[2])
            self.assertIn("c->esp += 4; return;", lines[2])
            with self.assertRaises(SystemExit):
                build_bootstrap.skip_intro([other], work / "out2")


if __name__ == "__main__":
    unittest.main()
