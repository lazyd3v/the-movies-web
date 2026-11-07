# Pthread / stored-archive filesystem feasibility

This isolated fixture uses the cached official Emscripten **5.0.7**, a preloaded one-pthread pool, a main application running in a dedicated browser worker, and the unchanged `public/stored-archive-fs.js` + `public/range-file.js`. It does not execute the original game or install graphics/media callbacks on child threads.

Build: `python3 scripts/thread-fs-probe/build.py`. Open `http://127.0.0.1:4173/runtime/thread-fs-probe/` and click **Run three filesystem cases**. Each case starts a fresh runtime/worker, mounts the real index/archive, and lets a pthread `fopen/fread/fclose` the original `Data/Audio/Atmosphere/fac_catering0.lug`. Its 740 bytes must have FNV-1a 129214825; the staged manifest records SHA256 `70da8dd61f56bd30716cf246963428909b206558db216518f0dbfe6f1742bb3a`, source/tool/artifact hashes and compiler arguments. Nothing is copied into a mock file node or preloaded as test data.

Cases:

1. Main spins for 350 ms without processing queued calls after the child starts, checks the read has not completed, then drains queued calls to recover. This makes the busy-main stall finite and observable rather than hanging the tab indefinitely.
2. Main keeps running synchronous C and repeatedly invokes `emscripten_main_thread_process_queued_calls()` until the child finishes.
3. Main uses `pthread_cond_timedwait` with five-millisecond deadlines and no explicit queue pump, mirroring the POSIX wait mechanism used by the current Win32 synchronization adapter.

Real browser verification passed all three cases. Case 0 observed the blocked child before pumping and finished in 460.235 ms after 1593 pump calls; case 1 finished in 7.52 ms with 1798 pump calls; case 2 finished in 7.24 ms with zero explicit pump calls. Every case read all 740 original bytes with FNV 129214825 and exactly one 262144-byte HTTP range request. These timings describe this small fixture, not game throughput.

## What the pinned source establishes

- `src/lib/libcore.js`, `wrapSyscallFunction`, assigns synchronous proxying to ordinary legacy filesystem syscalls when pthreads are enabled. `openat` and WASI `fd_read` therefore execute their JS FS work on the main application runtime thread; a child does not inherit or construct a second copy of the custom archive FS. [Official 5.0.7 source](https://github.com/emscripten-core/emscripten/blob/5.0.7/src/lib/libcore.js).
- `system/lib/pthread/library_pthread.c`, lines 109–116, executes the system proxy queue through `emscripten_main_thread_process_queued_calls`; it asserts that the caller is the main runtime thread. A PREEMPT_POLL integration must guard with `emscripten_is_main_runtime_thread()` when hooks also execute in children.
- `system/lib/pthread/emscripten_yield.c`, `_emscripten_yield`, calls that drain function on the main runtime thread. Whether a specific wait path invokes it is tested here, not assumed from generic documentation.
- `src/lib/libpthread.js`, `_emscripten_thread_cleanup`, directly cleans up when invoked by the main runtime thread but posts a `cleanupThread` message when invoked by a child. The parent's worker `onmessage` dispatch handles that message and returns the worker to the pool. **Draining the C system proxy queue does not dispatch arbitrary JavaScript worker messages.** Main-runtime `pthread_join` can perform cleanup directly; detached-child cleanup while the parent runs synchronous C can remain queued. [Official 5.0.7 source](https://github.com/emscripten-core/emscripten/blob/5.0.7/src/lib/libpthread.js).

The [official pthread guide](https://emscripten.org/docs/porting/pthreads.html) distinguishes browser UI thread from main application thread, warns about synchronous proxy deadlocks, and recommends preallocating workers when synchronous code needs immediate pthread startup. That page is newer than the local toolchain; the local 5.0.7 source and browser result govern this probe.

## Integration implications to validate

A conditional queue drain at generated safe polling boundaries can let child filesystem requests run without changing the archive filesystem design. A synchronous HTTP cache miss still blocks the main application worker during that bounded range fetch; it does not require the page UI thread. Do not drain while holding a lock needed by the proxied operation. This probe has no native Win32 file-handle table and does not establish that `native/file-api.c` is safe for simultaneous guest callers.

The thread pool's lifecycle is a separate concern: merely adding queue drains does not fix detached cleanup events blocked behind `callMain`. Prefer correctly retained joinable native threads and main-runtime reaping, or an explicit event-loop-friendly architecture; do not report successful thread creation when the real pool is exhausted. Child graphics/media/AudioContext providers are absent and remain unsupported, regardless of filesystem success.


The first browser attempt stalled at `loading-workers` before executing any read. The generated classic MODULARIZE runtime derives `_scriptName` from `self.location.href`; when loaded with `importScripts` in a custom wrapper, that identifies the wrapper, not the generated Emscripten script. Explicit `mainScriptUrlOrBlob: new URL('./probe.js', self.location.href).href` fixes the pthread entry script. Without it the child reloads the wrapper and its `onmessage` replaces Emscripten's pthread protocol handler. The fixture now supplies that URL and attaches independent error/messageerror listeners to nested workers. This setup requirement also applies to the production classic worker wrapper.


## Native file-table review

Read-only review of `native/file-api.c` found its existing global pthread mutex covers handle lookup/allocation, fd reads/writes/seek/close, COW/quota accounting, search snapshots, current directory and temp sequence. The file-type resolver uses the same lock. CloseHandle releases it before invoking the external synchronization/thread-handle resolver. Initialization and resolver setup occur before child creation. No missing runtime lock was found that warranted a production edit in this slice.

A child can hold that native lock while its libc syscall proxies to the main worker. The current JS archive stream operations do not reenter native file-api, so executing that filesystem proxy is distinct from recursively calling a guest file API. Preserve that boundary: system-queue servicing and guest callback dispatch must remain separate. A main-runtime-only, recursion-guarded system-queue drain at PREEMPT_POLL or after a fully returned import is appropriate; avoid adding arbitrary guest callbacks while native module locks are held. Contended pthread mutex/condition waits can themselves invoke Emscripten's system-queue service internally. This review is not a proof that every other native module or future provider is safe for reentrancy.


The fixture now also uses the production page-owned worker transport (`public/owned-worker-host.js` / `public/owned-worker-client.js`). This follows the separate watchdog finding that terminating a logical parent does not terminate its nested workers. The page owns and closes every real pthread; the main application sees a compatible Worker facade over a MessagePort. A fresh real-browser run passed all three cases through this ownership transport: mode 0 observed the stall and recovered in 360.87 ms (1466 pump calls); mode 1 completed in 8.695 ms (479 pump calls); mode 2 completed in 48.63 ms (zero explicit pump calls). Every case read 740 bytes with FNV 129214825 and one 262144-byte range request. [Trace](../../docs/evidence/thread-browser-owned-filesystem.log) and [capture](../../docs/evidence/thread-browser-owned-filesystem.jpg) validate actual Emscripten module/shared-memory cloning, pool startup and filesystem proxying through the extra message hop. These are isolated fixtures; no threaded original-game run or gameplay is verified.
