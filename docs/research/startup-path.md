# MoviesSE executable startup slices

Static inspection of the supplied `MoviesSE.exe`, SHA-256
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.
All addresses below are original **virtual addresses**, image base `0x00400000`.
Disassembly used Capstone 5.0.7 and pefile 2024.8.26 in
`.cache/recomp-probe/venv`. These are inspected control-flow paths, not an
execution trace. Function descriptions are inferred; original symbols are absent.
The runtime comparison uses cached retro-port commit
`ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf`, specifically
`plugins/retro-port/skills/port-pc-game-to-browser/template/recomp`.

## First executable milestone

Run the original entry `0x00ad2451` until **just before `0x00ad252c`**.
On the successful Windows XP compatibility branch this requires only three
import implementations: `GetVersionExA`, `GetModuleHandleA`, `HeapCreate`.
It also requires correct translated x86 control flow, stack, flags, indirect
import dispatch, mapped PE headers/sections/BSS, and FS-relative memory.
Do not substitute a new entry that jumps directly to WinMain: that would skip
required CRT and game global initialization.

| Instruction/call site | Actual operation | Required behavior |
| --- | --- | --- |
| `ad2458` → `ad7c10` | Installs SEH frame using `fs:[0]` | Writable per-thread TIB, initial exception chain `ffffffff` |
| `ad2464` → `ad10e0` | Stack allocation/probe helper, size `94` | Writable correctly aligned guest stack; stack semantics |
| `ad2471` → `[d16254]` | `GetVersionExA` | Populate OSVERSIONINFOA; platform 2, major 5, minor 1 is a supported branch |
| `ad24c4` → `[d16270]` | `GetModuleHandleA(NULL)` | Return mapped `400000`; code immediately reads MZ, PE and CLR directory fields |
| `ad251a` → `ad8da0` | Heap initialization | `ad8db1` calls `HeapCreate(0, 1000, 0)` for this entry argument |
| `ad8dc0` → `ad8d86` | Select allocator by OS version | NT platform 2 and major ≥5 selects normal allocator, avoiding older allocator initialization |
| `ad252c` → `ada01f` | Next slice: CRT locks and TLS | Stop here until implemented |

Assertions for this first slice: first API is `GetVersionExA`; image validation
passes; returned heap is nonzero and recorded at `10dade8`; allocator selector
at `10dadec` is 1; execution reaches `ad252c` without unsupported instruction
or import. A fake success return without the corresponding output writes is
not sufficient. Import calls use Win32 callee stack cleanup; guest CRT helpers
can use different conventions and must retain their original instructions.

## Subsequent slices, in actual entry order

1. **CRT locks and thread state (`ada01f`, return to `ad2531`).** Its initial
   call `ad6f6e` walks 36 lock descriptors at `e9a2f8`, initializing entries
   marked 1 via `ae97c4`. That helper dynamically resolves
   `InitializeCriticalSectionAndSpinCount` with `GetModuleHandleA("kernel32.dll")`
   and `GetProcAddress`. A NULL result is supported: fallback `ae97b4` calls
   imported `InitializeCriticalSection`, returns TRUE and pops two arguments.
   Next `ada01f` looks up `FlsAlloc`, `FlsGetValue`, `FlsSetValue`, `FlsFree`.
   If FLS is unavailable, it selects imported `TlsGetValue`, `TlsSetValue`,
   `TlsFree` and wrapper `ad9df3` around `TlsAlloc` (discarding the FLS destructor
   argument). Allocate a real TLS index, allocate/zero CRT thread data
   (observed size `8c`), preserve it through TlsSet/GetValue, and provide
   `GetCurrentThreadId`. Heap allocation and lock operations reached inside
   these callees must work. Returning NULL for unsupported dynamic optional
   APIs is preferable to claiming a callable implementation that does nothing.
2. **Early initializer interval (`ad253d` → `ae3ec7`).** The observed table
   start and end are both `ddcb50`, so this specific interval is empty.
3. **CRT file descriptors (`ad2545` → `ae4278`).** Allocates descriptor storage,
   calls `GetStartupInfoA`, examines inherited handles, calls `GetStdHandle`,
   `GetFileType`, critical-section helper `ae97c4`, then `SetHandleCount`.
   Return initialized STARTUPINFOA and consistent standard-handle types.
4. **Command line and environment.** `ad2556` calls `GetCommandLineA`;
   `ad2561` → `ae72d8` retrieves `GetEnvironmentStringsW`, converts with
   `WideCharToMultiByte` (size query then output), and frees with
   `FreeEnvironmentStringsW`. ANSI fallback uses `GetLastError`,
   `GetEnvironmentStrings`, `FreeEnvironmentStringsA`. Valid environment
   blocks terminate with two NULs. `ad256b` → `ae7236` initializes argv,
   including locale helper `ae3d81` and `GetModuleFileNameA`.
   `ad257c` → `ae7003` builds environment pointer arrays.
5. **CRT and game static initialization (`ad258f` → `ad0211`).** This is a
   major coverage boundary, not a single API stub. It calls an optional
   function pointer at `e99d28`, walks `[e4dbbc,e4dbd4)` (6 slots, 5 nonzero
   entries: `ace8f0`, `ad05d5`, `adf4a0`, `ae3d81`, `ada2b6`), invokes
   `ace950` with callback `ae3f0b`, then walks `[e4d000,e4dbb8)` (750 slots,
   749 nonzero). The latter starts `ae73fa`, `d10911`, `d10933`, `d10949`,
   `d10955`, `d06de0`. Recover/register these **indirect** code targets;
   a direct-call-only translator is insufficient. The first target `ae73fa`
   initializes the security cookie using `GetSystemTimeAsFileTime`,
   `GetCurrentProcessId`, `GetCurrentThreadId`, `GetTickCount`, and
   `QueryPerformanceCounter`. Remaining constructor effects have not been
   exhaustively traced in this inspection.
6. **Application entry.** `ad25aa` calls `GetStartupInfoA`; helper `ae6fa6`
   extracts the command-line tail. `ad25cd` calls `GetModuleHandleA(NULL)`;
   `ad25d0` calls `542b20` with the WinMain-shaped argument list
   `(hInstance, NULL, commandLineTail, showCommand)`. Its first direct calls
   include `9b7b80`, `4015d0`, `9b4900`, `9b5cd0`, `542aa0`.
   Arrival here proves bootstrap progress, not game initialization or rendering.

These milestones intentionally stop before unanalyzed callees. The tables
above establish ordering on the inspected successful path, not an exhaustive
API closure for each large helper.

## Delayed imports are executable guest thunks

| Import | Original IAT slot | Initial thunk | Descriptor |
| --- | --- | --- | --- |
| WMVCore!WMCreateSyncReader | `f87a3c` | `c9b666` | `e4a85c` |
| WMVCore!WMCreateProfileManager | `f87a40` | `aa6035` | `e4a85c` |
| DINPUT8!DirectInput8Create | `f87a34` | `aef120` | `e4a87c` |
| d3d9!Direct3DCreate9 | `f87a48` | `afba33` | `e4a89c` |

Each thunk supplies its IAT slot in EAX, saves ECX/EDX, calls common helper
`c9b296` with descriptor and slot, restores ECX/EDX, then **jumps to EAX**.
The helper uses `LoadLibraryA` at `c9b392`, `InterlockedExchange` at `c9b3e3`
to install/cache the module, and `GetProcAddress` at `c9b469`. It writes the
resolved address back to the IAT at `c9b4b3`. Optional notification/failure
hooks live at `10daa3c`/`10daa38`. Unload bookkeeping may use LocalAlloc and
FreeLibrary. Failed loads/resolution raise `c06d007e`/`c06d007f` through
`RaiseException`; invalid descriptor attributes raise `c06d0057`.

Therefore ordinary-import loader support alone is not necessarily a blocker:
retain original delay IAT values and translate the guest thunks/helper, then
supply real dynamic library/export dispatch when first reached. Alternatively
implement explicit lazy host delay resolution with equivalent ABI and errors.
Do not silently overwrite delayed imports with NULL or generic success stubs.
Dynamic jump targets returned by GetProcAddress must be recognized by the CPU
call dispatcher. This analysis does not establish which delayed API executes
first during gameplay; import-table ordering cannot answer that question.

## SEH and threading boundaries

`ad7c10` pushes handler `ad2968`, links the prior `fs:[0]`, and sets a frame
state field to `ffffffff`. `ad7c4b` restores the previous chain. Writing and
restoring this chain can execute without raising an exception, but does not
prove exception handling. Correct failure paths require Windows-style guest
exception dispatch and unwind; native Wasm/C exceptions do not substitute.
No static PE TLS directory is present, but CRT dynamic TLS is mandatory above.

Cached retro-port already initializes a main TIB and FS base in
`runtime/runtime.c:374` and has thread-local data/host pthread-backed
CreateThread in `host/kernel32.c:757`. Its TLS uses a 32-slot allocation mask
and per-thread storage, and its critical sections use recursive pthread locks.
That is a useful implementation starting point, not tested compatibility with
this game. Its `RaiseException`/`RtlUnwind` at lines 174/186 explicitly trap
because exception handling is unimplemented. Preserve this explicit failure
until real semantics exist; do not claim SEH support from TIB initialization.

The inspected bootstrap requires current-thread identity and storage but has
not demonstrated a CreateThread call before the constructor boundary. Begin
with one valid thread context; add actual concurrency when traced calls demand
it. A future worker must get its own stack, FS/TIB, TLS, last-error value,
calling convention and registered guest callback targets.

## Cached runtime gaps relevant to this path

The cached GetVersionExA already reports XP5.1; GetModuleHandleA(NULL) returns
`400000`. Its known-module list lacks d3d9, dinput8 and wmvcore, and unknown
LoadLibraryA fails with error126. GetProcAddress maps registered names to
callable dynamic import addresses and returns NULL for unknown names; ordinal
lookups return NULL. Its PE loader binds ordinary imports only. These behaviors
must be distinguished from the current project's strict bootstrap, which may
intentionally register fewer APIs. Neither module-name recognition nor the
171-name import overlap establishes graphics, COM/media or game compatibility.

Recommended immediate sequence: prove the three-import checkpoint, prove CRT
TLS allocation, then file descriptors/argv/env; instrument every constructor
entry and imported API before attempting WinMain. Record guest EIP, ESP,
arguments, return, unsupported target and per-thread ID. Keep missing APIs
fatal and visible. This yields a reproducible executable startup slice without
hiding missing behavior behind generic stubs.
