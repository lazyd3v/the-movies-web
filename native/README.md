# Original-code bring-up

This directory contains the new compatibility layer, not a replacement game.
The first harness uses native compilation to diagnose original entry-point
execution before adding browser integration. The standalone leaf probe already
tests the same translator's basic Wasm output.

`bootstrap.c` loads the audited executable through the pinned runtime and calls
its translated entry. Unknown functions, imports and runtime-generated code stop
with diagnostics. All game-derived translation output stays in `.cache`.

Run whole-program recovery and translation first:

```sh
python3 scripts/setup-analysis-tools.py
python3 scripts/recover-functions.py
.cache/recomp-probe/venv/bin/python scripts/translate-game.py
python3 scripts/build-bootstrap.py "/path/to/Game Files/MoviesSE.exe"
python3 scripts/build-bootstrap.py "/path/to/Game Files/MoviesSE.exe" --startup-apis
python3 scripts/build-bootstrap.py "/path/to/Game Files/MoviesSE.exe" --startup-apis --target wasm
```

The first bootstrap command deliberately registers zero Windows APIs, to locate
the first executed import. `--startup-apis` enables the narrow implementations
for startup, heap, threads, process information, text conversion, atomics,
shell folders, a private registry, game file access, mutex synchronization and
UTC time conversion, interactive message dialogs original PE icon resources and the initial window lifecycle.
Reports and logs are in
`.cache/recomp-full/bootstrap` (native) or `bootstrap-wasm` (Node/Wasm).
The Wasm diagnostic uses Node filesystem access to read the original executable;
it is not the browser launcher. Its 16 MiB stack and growable memory are bring-up
settings, not measured production requirements.
An exit from the build script means the diagnostic ran, not that the game ran:
inspect `returnCode` and `output` in its `result.json`. Code 4 is an explicit
guest trap, and `timeout` is an interrupted run with no compatibility conclusion.

The startup layer identifies its guest OS as XP SP3, preserves stdcall stack
cleanup, validates version structure sizes, and maintains thread-local last-error
state. GetModuleHandleA supports the executable and a virtual kernel32 module
handle. GetProcAddress resolves only registered kernel32 handlers, caches callable
trampolines and reports absent exports as absent. This lets the original CRT take
its optional-FLS fallback. Direct PE export-table inspection of kernel32 is not
implemented. Other modules and missing exports return failure, not fabricated
callable implementations.

The heap module owns the complete guest heap address range. Do not mix it with
the upstream `guest_alloc` bump allocator. It tracks per-heap allocation ownership,
reuses freed storage, preserves realloc contents, and implements zero-initialization.
Exception-generating or executable heap flags stop explicitly. Threaded gameplay
and guest exceptions are not yet supported by the bootstrap.

A local patch makes the runtime function table grow instead of hanging once its
fixed capacity is exhausted. The regression test registers over 100,000 entries,
including collisions and replacements; it also checks allocation-failure paths.

Contracts: Microsoft's [GetVersionExA documentation](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getversionexa)
and [GetModuleHandleA documentation](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandlea).
The observed original startup ordering is documented in
[`startup-path.md`](../docs/research/startup-path.md).

Focused native ABI checks (requires the cached toolkit headers):

```sh
npm run test:native
```

This runs the fifteen compatibility modules' contract tests, including multithreaded
TLS/recursive locks and atomics, registry conversion, private file access, and
mutex ownership, timeouts and abandonment, and Gregorian/FILETIME conversions.
Guest CreateThread, scheduling and exception unwinding remain unimplemented.

File access maps the original installation read-only at `C:\Games\TheMovies`.
Writes use a separate private overlay. The browser mounts the stored game archive
at `/game` using `stored-archive-fs.js`, which shares a bounded range cache across
all files. Registry state is currently memory-only; persistence is not established.
The private environment starts empty and accepts guest updates, including the
CRT's per-drive current-directory variables. It never reads the host environment.
Guest local time is explicitly UTC with no daylight saving adjustment.
Message dialogs require a synchronous UI callback. The browser worker bridges to
an HTML dialog and waits for an actual button response. Native/Node bring-up
without a callback intentionally stops, rather than automatically acknowledging
game notices. The browser can therefore advance beyond the Node startup stop.

Optimized experimental profiles use separate caches and browser artifacts:

```sh
python3 scripts/build-bootstrap.py "/path/to/Game Files/MoviesSE.exe" --startup-apis --target wasm --optimization 1
python3 scripts/stage-static-browser.py "/path/to/Game Files/MoviesSE.exe" --optimization 1
```

Choose the corresponding diagnostic build in `static-probe.html`. The default
O0 build remains separate. O1/O2 compile and link with matching optimization
levels, disable floating-point contraction and strict aliasing, and preserve
signed wrapping. They do not enable fast-math. Profile provenance records the
flags and artifact hashes. Each profile still requires original-code execution
validation; compilation alone does not establish compatible behavior or speed.

The initial window module owns class/window records and invokes the translated
original window procedure for creation messages. The browser host acknowledges
actual canvas creation, visibility, dimensions and cursor mutations over a
SharedArrayBuffer. Its virtual desktop is explicitly 1024×768. A bounded message queue dispatches original callbacks; a shared input ring
lets synchronous Wasm poll browser pointer events. Keyboard translation and
rendering remain unfinished, and end-to-end pointer behavior is unverified; an empty canvas is
not evidence that the game renders. Unsupported window behaviors fail explicitly.

DirectInput now has a bounded real COM implementation for the original buffered
keyboard/mouse path. A separate shared event ring carries physical key positions,
relative mouse deltas/buttons/wheel and generation timestamps. Acquisition checks
actual browser-canvas focus; queues implement peek, consume and overflow. Native
and linked Wasm tests pass, but the original program still stops at its Windows
Media dependency check before exercising DirectInput.

Builtin DLL entries are read-only **metadata**, enabled only when the module
registry and implemented export agree. They have no Windows PE backing; opens
are denied explicitly. Only implemented DirectInput is registered by bootstrap.
WMVCore is not advertised while its game-facing media adapter is unfinished.
ExitProcess now genuinely terminates the runtime and reports its exit code.
