# Game-specific browser compatibility layer: local evidence

This is a static scope audit, not a working replacement runtime. It examines the
supplied binaries, local source checkout, and existing browser-port evidence.
The original game files were not changed or executed for this audit. Full import
names and ordinals are in `pe-import-audit.json`.

## What must execute

`MoviesSE.exe` is a 12,230,656-byte PE32 Intel x86 binary. Its `.text` section is
9,520,306 bytes, image base is `0x400000`, and entry point is recorded in the JSON.
There is no base-relocation directory or static TLS directory; dynamic TLS APIs
are imported. The supplied 719-file installation contains no C/C++ source,
project files, or separate DLLs. Embedded `.cpp` paths are debug strings only.

The executable cannot run directly as WebAssembly. Replacing Win32 calls does
not translate its instructions. Preserving the original executable still needs
an x86 interpreter/JIT or a substantial static binary translation system, including
32-bit address-space behavior, flags, x87/SSE semantics, indirect calls, stack
conventions, exceptions/SEH, callbacks, synchronization and self-modifying-code
handling where encountered. A fixed PE base simplifies loading, not execution.

## Measured ABI surface

| Import DLL | Ordinary import entries |
| --- | ---: |
| KERNEL32.dll | 191 |
| USER32.dll | 42 |
| GDI32.dll | 3 |
| ADVAPI32.dll | 10 |
| SHELL32.dll | 2 |
| WINMM.dll | 32 |
| MSACM32.dll | 7 |
| WS2_32.dll | 29 |
| SHFOLDER.dll | 1 |
| ole32.dll | 9 |
| OLEAUT32.dll | 4 |
| **Total** | **330** |

Four additional delay imports are present:

- `d3d9.dll!Direct3DCreate9`
- `DINPUT8.dll!DirectInput8Create`
- `WMVCore.DLL!WMCreateSyncReader`
- `WMVCore.DLL!WMCreateProfileManager`

These counts are entry-table counts, not an implementation estimate. COM objects
returned by those entry points expose additional vtable methods; `CoCreateInstance`
and `GetProcAddress` reach interfaces that import-table counting misses. Executable
strings also identify DirectSound/OpenAL, WinINet and other DLLs, but strings alone
do not prove that a path executes. Winsock/OLE automation imports include ordinals.

The game directly imports real thread/event/mutex/semaphore/wait operations,
memory mapping/protection/query functions, timers, heap operations, ANSI and Unicode
file APIs, window/message handling, cursor capture/warping, registry, multimedia
stream/callback APIs and COM. Constant-return stubs can support a narrowly tested
startup path; they cannot be assumed safe throughout studio simulation and editing.

`StarMaker.exe` is a separate optional program with 230 ordinary imports and one
delayed Direct3D entry point. It should be excluded from the first runtime milestone.

## Media is a substantial boundary

Existing evidence in `docs/evidence/media-passed-trace.log` shows real startup
construction of an `ASF Enc/Writer` DirectShow filter through native `qasf.dll`,
with native `WMASF.DLL` and `wmvcore.dll` loaded. This path was a startup blocker;
it is not merely an unused movie-export feature.

The current media overlay installs 21 original Microsoft x86 DLLs. Static inspection
counts 2,387 ordinary import entries across those DLLs, in addition to their delay
imports. Dependencies include `msvcrt`, `RPCRT4`, `CRYPT32`, `WINTRUST`, `SHLWAPI`,
`WININET`, `MSVFW32`, and others. Keeping these binaries inside a new thin runtime
would greatly expand the Win32 implementation surface. A game-specific replacement
should trace and implement the actually used media COM contracts, backed by native
browser/compiled codec facilities. Browser support for this game's WMV/WMA assets
must be demonstrated; it cannot be assumed. Unsupported recording/export features
must be explicit rather than silently returning success.

## Feasible staged architecture

1. **Executable bootstrap vertical slice.** Map the unmodified PE at its preferred
   address in WASM linear memory, resolve its ordinary and delayed imports through
   explicit trap addresses, and execute entry code using a validated existing x86
   CPU core. Record every API call and fail with an actionable trace on unsupported
   operations. Milestone: reach window creation and the first Direct3D call with
   the real executable; no claim of gameplay yet.
2. **Direct graphics/input path.** Implement the exercised Direct3D9 COM ABI and
   translate actual render states, resources and shaders to WebGL/WebGPU. Keep the
   graphics command stream in the worker owning an OffscreenCanvas; avoid synchronous
   per-call worker/main crossings. Implement DirectInput and Win32 cursor semantics
   against one tested input model, including loss/reacquisition of pointer lock.
3. **Startup media and original menu.** Implement the measured startup COM/media
   contract and audio timing, then prove the unmodified executable reaches its real
   menu. A native browser title screen is not equivalent evidence.
4. **Playable studio slice.** Validate original scenario loading, hiring/building,
   shooting, saving/reloading, audio and input over a sustained session. Extend the
   shim from traces, not by guessing complete API coverage.

Reusing a CPU-only component still requires adaptation: Boxedwine's current x86 core
is coupled to Linux process/memory/thread abstractions. Removing Wine and Linux is
an engineering project, not a switch. A pragmatic intermediate experiment is to
move one expensive subsystem across a native host boundary while retaining the
known-working execution path, so speed and compatibility changes can be measured.

A custom layer can remove Wine/Linux and redundant graphics translation/proxy work.
It does not eliminate x86 translation cost or the game's own work. Existing timing
must first separate guest CPU execution, JIT compilation, renderer work, I/O and
cross-thread waits; otherwise a rewrite may preserve the dominant bottleneck.

## Assets and current reusable work

The installation contains ten PAK archives, 102 OGG tracks, 114 WMV files, proprietary
scene/animation/sound data, and configuration/text files. This repository currently
has packaging, HTTP range mounting, media setup and runtime-building tools; it has
no general PAK extractor or engine reimplementation.

Read-only sampling earlier established that PAK version 4 tables start at byte40,
versions5/6 at byte52, with 56-byte records and the entry count at header byte12.
Sample INI/DDS payloads successfully zlib-decompressed after their 16-byte entry
header. Other samples contain `zcmp` or raw payloads. This is partial format evidence,
not a complete extractor specification. Extraction could support direct asset
loading or media conversion, but is unnecessary to preserve the game's own PAK
reader and does not recover engine source or simulation logic.
