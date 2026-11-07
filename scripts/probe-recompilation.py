#!/usr/bin/env python3
"""Translate two verified original leaf routines to Wasm and test in Node.

This is a bounded instruction/ABI proof, NOT a game port or performance test.
Requires capstone==5.0.7, pefile==2024.8.26 and the cached retro-port/Emscripten
toolchains. Original bytes and generated code stay in ignored .cache only.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE_SHA = "48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a"
TOOL_HASHES = {
    "translator/x86toc.py": "3d42b5d26b9ac803375da2f888e959505490d99f4e5e1d8bbb09c87565fa4dea",
    "runtime/cpu.h": "0ffb3dfda1c641a5341fcb01687893e00a002928bb338745881abb0cf3fcb36c",
}
ROUTINES = (
    (0x401240, 0x401251, "strlen_leaf", "10cf8dcb920a66d38b21acffe979afd9288ee60b916ab7dea7cbeb57d4e06a59"),
    (0x401270, 0x4012b1, "strcmp_leaf", "cb0e9f217a764eb2b5965f6bdc347ed5d343e969847cd2a8507e541b10a53add"),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def checked(path, expected):
    data = path.read_bytes()
    if sha(data) != expected:
        raise ValueError(f"SHA-256 mismatch: {path}")
    return data


def translate_leaf(tool, mapped, base, start, end, name):
    # Deliberately avoid Program's heuristic discovery of the rest of the binary.
    # Only these manually inspected leaf ranges can be decoded or reached.
    class BoundedProgram:
        trace = False
        entries = {start}
        ghidra_entries = {start}
        jumptables = {}
        md = tool.Cs(tool.CS_ARCH_X86, tool.CS_MODE_32)
        md.detail = True

        def next_entry(self, entry):
            return end

        def code(self, low, high):
            if low < start or low >= end:
                raise ValueError("Decode escaped verified routine")
            return mapped[low - base:min(high, end) - base]

    translator = tool.FunctionTranslator(BoundedProgram(), {
        "entry": start, "name": name, "ranges": [[start, end]],
    })
    instructions = translator.decode()
    cursor = start
    for instruction in instructions:
        if instruction.address != cursor:
            raise ValueError("Incomplete instruction coverage")
        cursor += instruction.size
        if instruction.mnemonic.startswith("call"):
            raise ValueError("Leaf probe cannot call another routine")
        if instruction.mnemonic.startswith("j"):
            op = instruction.operands[0]
            if op.type != tool.CS_OP_IMM or not start <= op.imm < end:
                raise ValueError("Branch escaped verified routine")
    if cursor != end:
        raise ValueError("Routine not completely decoded")
    source, unsupported, count = translator.translate()
    if unsupported or any(token in source for token in ("guest_trap(", "call_indirect(", "jump_indirect(")):
        raise ValueError(f"Unsupported translation: {unsupported}")
    return source, count


HARNESS = r'''
#include "cpu.h"
static uint8_t guest_memory[147456];
uint8_t *g_mem = guest_memory;
volatile uint8_t g_guest_lock;
int g_preempt_on = 0;
void guest_preempt(CPU *c) { __builtin_trap(); }
void guest_trap(CPU *c, uint32_t a, const char *why) { __builtin_trap(); }
static CPU state;
void f_00401240(CPU *c);
void f_00401270(CPU *c);
uint8_t *probe_input(int index) { return guest_memory + (index ? 81920 : 16384); }
uint32_t probe_run(int comparison) {
    memset(&state, 0, sizeof(state));
    state.esp = 4096;
    state.ebx = 0x12345678; state.esi = 0x87654321;
    state.edi = 0xabcdef01; state.ebp = 0x10203040;
    W32(4096, 0xdeadbeef); W32(4100, 16384); W32(4104, 81920);
    if (comparison) f_00401270(&state); else f_00401240(&state);
    if (state.esp != 4100 || R32(4096) != 0xdeadbeef ||
        state.ebx != 0x12345678 || state.esi != 0x87654321 ||
        state.edi != 0xabcdef01 || state.ebp != 0x10203040) __builtin_trap();
    return state.eax;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_exe", type=Path)
    parser.add_argument("--toolkit", type=Path, default=ROOT / ".cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp")
    parser.add_argument("--emcc", type=Path, default=ROOT / ".cache/runtime-build/emsdk-main/upstream/emscripten/emcc")
    args = parser.parse_args()
    output = ROOT / ".cache/recomp-probe"
    output.mkdir(parents=True, exist_ok=True)
    # Invalidate prior success before a new attempt, including integrity failures.
    report_path = output / "report.json"
    report_path.unlink(missing_ok=True)
    checked(args.game_exe, EXE_SHA)
    for relative, digest in TOOL_HASHES.items():
        checked(args.toolkit / relative, digest)
    spec = importlib.util.spec_from_file_location("x86toc", args.toolkit / "translator/x86toc.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    pe = tool.pefile.PE(str(args.game_exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    mapped = pe.get_memory_mapped_image()
    sources, records = [], []
    for start, end, name, digest in ROUTINES:
        if sha(mapped[start - base:end - base]) != digest:
            raise ValueError(f"Routine integrity failure: {name}")
        source, count = translate_leaf(tool, mapped, base, start, end, name)
        sources.append(source)
        records.append({"entry": hex(start), "endExclusive": hex(end), "nameInferredFromInstructions": name,
                        "sha256": digest, "instructions": count})
    generated = output / "probe.c"
    generated.write_text(HARNESS + "\n" + "\n".join(sources))
    wasm = output / "probe.wasm"
    env = dict(os.environ, EM_CACHE=str(output / "emscripten-cache"))
    subprocess.run([str(args.emcc.resolve()), str(generated), "-I" + str((args.toolkit / "runtime").resolve()),
                    "-O2", "--no-entry", "-sSTANDALONE_WASM=1", "-sFILESYSTEM=0",
                    '-sEXPORTED_FUNCTIONS=["_probe_input","_probe_run"]', "-o", str(wasm)], check=True, env=env)
    result = subprocess.run(["node", str(ROOT / "tests/probe-recompilation.mjs"), str(wasm)],
                            check=True, capture_output=True, text=True)
    tests = json.loads(result.stdout)
    report = {"scope": "Original leaf translation and Wasm execution only; not gameplay or a performance benchmark",
              "gameExeSha256": EXE_SHA, "toolHashes": TOOL_HASHES, "routines": records,
              "generatedCsha256": sha(generated.read_bytes()), "wasmSha256": sha(wasm.read_bytes()),
              "wasmBytes": wasm.stat().st_size,
              "compiler": subprocess.run([str(args.emcc.resolve()), "--version"], check=True, capture_output=True, text=True).stdout.splitlines()[0],
              "tests": tests}
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
