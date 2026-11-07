import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import hashlib

spec = importlib.util.spec_from_file_location("translate_game", Path(__file__).resolve().parents[1] / "scripts/translate-game.py")
translation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(translation)


class TranslationCoverageTests(unittest.TestCase):
    def test_supplement_requires_body_header_and_registration(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "recomp.h").write_text("void f_00a543b0(CPU *c);\n")
            (out / "recomp_stubs.c").write_text("register_function(0x00a543b0u, f_00a543b0);\n")
            source = out / "recomp_000.c"
            source.write_text("void f_00a543b0(CPU *c) { c->esp += 12; return; /* ret 8 */ }\n")
            translation.verify_supplemental_output(out, [source], [0xa543b0])
            source.write_text('void f_00a543b0(CPU *c) { guest_trap(c, 0xa543b0u, "gap"); /* ret 8 */ }\n')
            with self.assertRaisesRegex(ValueError, "completely"):
                translation.verify_supplemental_output(out, [source], [0xa543b0])
            with self.assertRaisesRegex(ValueError, "missing"):
                translation.verify_supplemental_output(out, [], [0xa543b0])

    def test_supplement_preserves_export_and_rejects_bad_bytes_or_overlap(self):
        data = {"functions": [{"entry": 10, "name": "original", "ranges": [[10, 20]]}], "jumptables": []}
        code = b"fixture bytes"
        config = {"schemaVersion": 1, "gameExeSha256": translation.EXE_SHA,
                  "entries": [{"entry": 30, "end": 30 + len(code), "fileOffset": 0,
                               "sha256": hashlib.sha256(code).hexdigest()}]}
        with tempfile.TemporaryDirectory() as directory, patch.object(translation, "digest", return_value=translation.EXE_SHA):
            exe = Path(directory) / "fixture.exe"
            exe.write_bytes(code)
            result = translation.supplement_functions(data, config, exe)
            self.assertEqual(len(data["functions"]), 1)
            self.assertEqual(result["functions"][1]["entry"], 30)
            with self.assertRaisesRegex(ValueError, "overlaps"):
                translation.supplement_functions(result, config, exe)
            exe.write_bytes(b"changed bytes")
            with self.assertRaisesRegex(ValueError, "checksum"):
                translation.supplement_functions(data, config, exe)

    def test_registration_batches_preserve_order_and_existing_stubs(self):
        prefix = '#include "recomp.h"\nvoid f_00000000(CPU *c) { guest_trap(c, 0x0u, "stub"); }\n'
        lines = [f'    register_function(0x{i:08x}u, f_{i:08x});' for i in range(1025)]
        source = prefix + 'void register_all_functions(void) {\n' + '\n'.join(lines) + '\n}\n'
        output, details = translation.batch_registration(source)
        self.assertTrue(output.startswith(prefix))
        self.assertEqual(details, {"batchSize": 256, "batches": 5, "registrations": 1025})
        self.assertEqual([line for line in output.splitlines() if translation.REGISTRATION.fullmatch(line)], lines)
        calls = output.split('void register_all_functions(void) {\n')[1]
        self.assertEqual(calls, ''.join(f'    register_batch_{i:05d}();\n' for i in range(5)) + '}\n')

    def test_registration_rewrite_rejects_unexpected_statements(self):
        for body in ('    side_effect();', '    register_function(0x00000001u, f_00000002);', ''):
            with self.subTest(body=body), self.assertRaises(ValueError):
                translation.batch_registration('void register_all_functions(void) {\n' + body + '\n}\n')

    def test_recovery_union_does_not_double_count_overlapping_ranges(self):
        data = {"functions": [{"entry": 10, "name": "a", "ranges": [[10, 30]]},
                              {"entry": 20, "name": "b", "ranges": [[20, 40]]}],
                "jumptables": [{"at": 15, "targets": []}]}
        report = translation.validate_functions(data)
        self.assertEqual(report["recoveredRangeUnionBytes"], 30)
        self.assertEqual(report["jumpTablesWithoutTargets"], 1)
        data["functions"][1]["entry"] = 10
        with self.assertRaises(ValueError):
            translation.validate_functions(data)

    def test_traps_and_dispatch_are_reported_without_instruction_text(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recomp_000.c"
            path.write_text('''void f_00401000(CPU *c) {
guest_trap(c, 0x00401000u, "unsupported: private operands");
call_indirect(c, t_);
}
void f_00402000(CPU *c) {
if (!d_) guest_trap(c, 0x00402000u, "divide by zero");
jump_indirect(c, t_, 0x00402001u);
}
''')
            scanned = translation.scan_generated([path])
        self.assertEqual(scanned["generatedFunctionDefinitions"], 2)
        self.assertEqual(scanned["functionsWithExplicitTraps"], 1)
        self.assertEqual(scanned["explicitTrapSitesByCategory"], {"unsupported": 1})
        self.assertEqual(scanned["architecturalDivideByZeroChecks"], 1)
        self.assertEqual(scanned["functionsUsingIndirectDispatch"], 2)
        report = translation.summarize({"stats": {}, "unsupported": [["translator crash: private details", 1]]}, scanned, {})
        self.assertEqual(report["status"], "generated_with_blockers")
        self.assertNotIn("private", str(report))
        self.assertFalse(report["playable"])

    def test_no_traps_still_does_not_claim_execution(self):
        report = translation.summarize({"stats": {}, "unsupported": []}, {"functionsWithExplicitTraps": 0}, {})
        self.assertEqual(report["status"], "generated_unvalidated")
        self.assertFalse(report["executionValidated"])


if __name__ == "__main__":
    unittest.main()
