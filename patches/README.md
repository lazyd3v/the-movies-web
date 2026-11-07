# Boxedwine ZIP_STORED read-ahead

`boxedwine-stored-read-ahead.patch` applies to Boxedwine commit
`26bf258ab0dc00aa1ec6da81d1ca263f922f8947`. It adds a lazy, per-open-file
256 KiB buffer to the Emscripten ZIP_STORED reader. The original game executable
and assets are untouched. Compressed ZIP entries and non-Emscripten builds keep
their original reader.

The guest-memory reader splits requests into 4 KiB pages. Previously each page
issued a native seek and read, both synchronously proxied from the browser worker
to the main thread. Read-ahead serves cache hits within WASM. The standalone test
requires four backing reads for 1 MiB of sequential 4 KiB requests instead of
256—a reduction in backing calls, not a measured overall game speedup.

Build with Python **3.12+**, Git, make, and a host C++ compiler (`clang++`):

```sh
python3 scripts/build-runtime.py --jobs 3
```

The script downloads source and Emscripten into `.cache/runtime-build`, applies
the patch, runs `tests/test_stored_read_cache.cpp`, builds only `multiThreadedJit`,
and stages files in `public/runtime/read-ahead`. It does not replace the primary
runtime. `--stage-only` validates/tests and stages an already completed build.
Both patched source files are normalized from mixed line endings before patching.

Source and SDK bootstrap archives have pinned SHA-256 checks. The bootstrap is
pinned to commit `d7b4130688bfa348392b921ab04015aa15c4f154`; SDK version `5.0.7`
resolves to release `6cd98e86d7749ff98b82b7f2ae78eb4f01942788`. The pinned
Boxedwine CI mentions emrun 5.0.7; it does not explicitly pin its full SDK.
Emscripten manages its own Node/Python/toolchain and SDL/zlib port downloads.
Compiler timestamp metadata means repeated builds are not promised byte-identical.
The staged `manifest.json` records the actual resulting hashes.

The semantic test covers sequential reads, random forward/backward seeks, offsets
above 4 GiB, EOF, short backing reads, partial failures and invalidation. An
interactive browser A/B test is still necessary to establish gameplay performance.

Boxedwine remains GPL-licensed. The upstream license is retained in
`public/runtime/LICENSE-Boxedwine.txt`; the patch and immutable source reference
provide the changes needed to reproduce this modified runtime.

## Isolated chain128 experiment

```sh
python3 scripts/build-runtime.py --chain-limit 128 --jobs 3
```

This stages `public/runtime/chain128`, preserving `public/runtime/read-ahead`.
It adds only `-DWASM_JIT_MT_CHAIN_BLOCK_LIMIT=128`; read-ahead remains enabled,
optimization stays `-O2`, and JIT warmup stays 200. Per-compiled-block checks for
yield, signals, termination, debugging and ptrace remain in place. The bound
allows four times as many compiled blocks before outer thread bookkeeping, so
latency and gameplay stability must be checked as well as load time.

The build script checks the recorded compiler configuration before staging;
`--stage-only --chain-limit 128` refuses to label a default build as the variant.
The object directory is reused, but upstream rebuilds when compile flags change.
Run the default command to rebuild/re-stage chain32. Compare runs with matching
browser visibility, game settings, safe-memory mode and cache state.

## Earlier JIT promotion experiment

```sh
python3 scripts/build-runtime.py --chain-limit 128 --warmup-threshold 50 --jobs 3
```

This stages `public/runtime/performance` and keeps both earlier variants intact.
Relative to chain128, the only added compiler define is `-DJIT_RUN_COUNT=50`.
The tiny-block additional-hit threshold is derived upstream as three times this
value, so it changes from 600 to 150. Optimization remains `-O2`; fast-math is
not enabled. Earlier promotion can reduce interpretation of moderately hot code,
but increases compilation work and potentially retained module memory. Compare
startup time, pointer response and scene animation alongside heap and JIT OOM
counters before selecting this variant as default.
