# Original executable → C → WebAssembly probe

Two small routines from the supplied `MoviesSE.exe` were statically translated
and executed successfully as a standalone 1,469-byte WebAssembly module in Node.
All 1,335 reference checks passed across 267 input samples. This establishes a
small translation/ABI foothold, **not a playable port or a performance result**.

| Original virtual address | Size | Instructions | Observed semantics |
| --- | ---: | ---: | --- |
| `0x00401240` | 17 bytes | 8 | NUL-terminated byte-string length |
| `0x00401270` | 65 bytes | 30 | Unsigned byte-string comparison returning −1, 0 or 1 |

Names are inferred from disassembly, not recovered symbols. These are likely
library-style helper routines embedded in the executable, not representative
studio simulation workloads. They have no external calls, global-data reads,
floating-point instructions or operating-system dependencies.

## Reproduce

The probe uses [retro-port](https://github.com/Dhaiwat10/retro-port) commit
`ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf` under `.cache/retro-port` and the
Emscripten 5.0.7 toolchain already installed by `scripts/build-runtime.py`.
Node and Python with the following isolated dependencies are required:

```sh
git clone https://github.com/Dhaiwat10/retro-port.git .cache/retro-port
git -C .cache/retro-port checkout ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf
python3 -m venv .cache/recomp-probe/venv
.cache/recomp-probe/venv/bin/pip install capstone==5.0.7 pefile==2024.8.26
.cache/recomp-probe/venv/bin/python scripts/probe-recompilation.py "/path/to/Game Files/MoviesSE.exe"
.cache/recomp-probe/venv/bin/python tests/probe_guardrails_test.py
```

Use `--toolkit /path/to/template/recomp` or `--emcc /path/to/emcc` to supply
existing tools elsewhere. The script verifies the full executable, both routine
ranges, translator and CPU-helper SHA-256 hashes before generating code. It
does not download tools or modify the original executable.

Generated C, Wasm and the machine-readable report stay in ignored
`.cache/recomp-probe`. A sanitized result is retained in
[recompilation-probe-result.json](recompilation-probe-result.json). It contains
addresses, hashes and counts, with no original bytes or generated game code.

## What is exercised

The script calls the toolkit's `FunctionTranslator` directly with manually
verified bounds, instead of claiming full Ghidra function recovery. Generated
functions use the toolkit's CPU-state and guest-memory helpers. A small harness
supplies two input buffers and a guest stack; there is no Wine or x86 interpreter
in the resulting module, and the Node test rejects any WebAssembly imports.

Tests cover empty strings, embedded NULs, high unsigned bytes, equal strings,
prefixes, differences, inputs up to 65,534 bytes and seeded random cases. Expected
results come from independent JavaScript byte-array operations. Every call also
checks the guest stack adjustment, return-address sentinel and preservation of
EBX, ESI, EDI and EBP. These are reference tests, not differential execution of
the original x86 binary under Windows.

The probe rejects incomplete decoding, out-of-range branches, external calls,
unsupported instructions and generated fallback traps. Two additional guardrail
tests exercise altered-input hashes and synthetic rejected instruction paths.
No success report is retained after a failed new attempt.

## What remains unresolved

Whole-game translation requires function/jump-table recovery, indirect-call
resolution, Win32/COM/D3D9 implementations, exception handling, callbacks and
thread semantics. The toolkit stores x87 values as host doubles, explicitly
leaving exact 80-bit behavior unsupported. Its broader translator can emit
trapping fallbacks; this probe rejects them rather than treating compilation as
success. None of those larger compatibility questions is answered by string
helpers passing. No runtime speed comparison or browser gameplay test was made.
