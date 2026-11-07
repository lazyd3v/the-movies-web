"""Regression: heuristic entries inside instructions must not truncate final code.

Run with .cache/recomp-probe/venv/bin/python; synthetic instructions only.
"""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import json

ROOT = Path(__file__).resolve().parents[1]


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


wrapper=load("full_translate",ROOT/"scripts/translate-game.py")
TOOLKIT=ROOT/".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp"


@unittest.skipUnless(importlib.util.find_spec("capstone") and importlib.util.find_spec("pefile") and TOOLKIT.exists(), "probe venv and pinned toolkit required")
class BoundaryTests(unittest.TestCase):
    @unittest.skipUnless(wrapper.GAME.exists(), "Original local executable required")
    def test_evidenced_raster_callback_decodes_all_returns_without_dispatch(self):
        import pefile
        config = json.loads((ROOT / "scripts/translation-extra-entries.json").read_text())
        item = config["entries"][0]
        self.assertEqual(wrapper.digest(wrapper.GAME), wrapper.EXE_SHA)
        with tempfile.TemporaryDirectory() as directory:
            tool = load("raster_x86toc", wrapper.prepare_translator(TOOLKIT, Path(directory)))
            pe = pefile.PE(str(wrapper.GAME))
            program = tool.Program.__new__(tool.Program)
            program.base = pe.OPTIONAL_HEADER.ImageBase
            program.img = pe.get_memory_mapped_image()
            program.text_end = item["end"]
            program.trace = False
            program.ghidra_entries = {item["entry"]}
            program.entries = {item["entry"], item["end"]}
            program.jumptables = {}
            program.md = tool.Cs(tool.CS_ARCH_X86, tool.CS_MODE_32)
            program.md.detail = True
            function = {"entry": item["entry"], "name": "evidenced_raster", "ranges": [[item["entry"], item["end"]]]}
            translator = tool.FunctionTranslator(program, function)
            decoded = translator.decode()
            self.assertEqual([i.address for i in decoded if i.mnemonic == "ret"], [0xa54427, 0xa5447a, 0xa545f4])
            self.assertEqual(decoded[-1].address + decoded[-1].size, item["end"])
            source, unsupported, _ = translator.translate()
            self.assertEqual(unsupported, [])
            self.assertNotIn("call_indirect", source)
            self.assertNotIn("jump_indirect", source)
            self.assertNotIn("guest_trap", source)

    def test_last_heuristic_function_runs_past_false_mid_instruction_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            patched=wrapper.prepare_translator(TOOLKIT,Path(directory))
            tool=load("patched_x86toc",patched)
            # push 0x11223344; call 0x2000; pop ecx; ret, at 0x1010.
            instructions=bytes.fromhex("68 44 33 22 11 e8 e6 0f 00 00 59 c3")
            program=tool.Program.__new__(tool.Program)
            program.base=0x1000;program.img=b"\xcc"*16+instructions
            program.text_end=0x101c;program.trace=False
            program.ghidra_entries={0x1000}
            program.entries={0x1000,0x1010,0x1012,0x2000}
            program.jumptables={}
            program.md=tool.Cs(tool.CS_ARCH_X86,tool.CS_MODE_32);program.md.detail=True
            function={"entry":0x1010,"name":"synthetic_final_leaf","ranges":[[0x1010,0x1012]]}
            translator=tool.FunctionTranslator(program,function)
            decoded=translator.decode()
            self.assertEqual([instruction.mnemonic for instruction in decoded],["push","call","pop","ret"])
            source,unsupported,count=translator.translate()
            self.assertEqual((unsupported,count),([],4))
            self.assertIn("f_00002000(c)",source)
            self.assertNotIn("jump_indirect",source)
            self.assertEqual(program.next_entry(0x1010),program.text_end)
            self.assertEqual(wrapper.digest(TOOLKIT/"translator/x86toc.py"),wrapper.TRANSLATOR_SHA)

    def test_known_later_ghidra_boundary_still_wins(self):
        with tempfile.TemporaryDirectory() as directory:
            tool=load("patched_boundary",wrapper.prepare_translator(TOOLKIT,Path(directory)))
            program=tool.Program.__new__(tool.Program)
            program.ghidra_entries={0x1000,0x1100};program.text_end=0x2000
            self.assertEqual(program.next_entry(0x1010),0x1100)


if __name__=="__main__":unittest.main()
