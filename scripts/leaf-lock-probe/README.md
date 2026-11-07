# Real leaf-lock fatal exception acceptance fixture

Build: `python3 scripts/leaf-lock-probe/build.py`.
Open `http://127.0.0.1:4173/runtime/leaf-lock-probe/` and click **Run fatal-lock test**. Results appear in the page and `globalThis.leafLockProbeResult`.

This fixture extracts the exact audited generated original `f_009E9AB0`, verifies its body hash, adds only the candidate outer lock after preemption/unlock before return, and compiles it with unchanged current threaded cpu.h accessors plus PORT_BOUNDS. Generated game-derived source remains in ignored `.cache`; browser artifacts remain in ignored `public/runtime`.

Two actual Emscripten pthreads are prewarmed and owned by the page using the production `/owned-worker-host.js` and `/owned-worker-client.js`. The root blocks in Atomics.wait. One child increments a shared heartbeat; the other waits for the page to observe parent-blocked, child-ready and at least five heartbeats, then runs the exact candidate with a vertex index producing the invalid write address `0x5400105c`.

The real bounds-check handler records lock depth/address/entry into shared atomic telemetry and executes `__builtin_trap`, matching production's fatal trap mechanism. It intentionally omits production fprintf, avoiding unrelated stdio proxying while the parent is blocked. This is a real Wasm exception, not a synthetic error message or latch-only trigger.

Acceptance requires the page-owned child's actual error callback, recorded bounds trap with lock depth1 and exact address, root blocked, two owned workers before cleanup and zero afterward, and a heartbeat that stops after direct pool termination. A supervision timeout is failure. The observation intervals at 20, 100, 300 and 1000ms alone is not success: the trap telemetry and actual owned-worker teardown must also pass. The trap child cannot heartbeat after failure; the independent sibling verifies that teardown reaches surviving pool workers. Cleanup never acquires guest memory locks or waits for the blocked root.

This verifies fatal termination, not recoverable guest exceptions, general lock coarsening or frame performance. No production translator/runtime/native source is modified by this fixture.

Actual first browser run failed: correct real bounds fault/depth1 and owned count2→0, but independent heartbeat continued5→48 after300ms. The earlier synthetic JS heartbeat proof therefore does not establish synchronous Wasm termination. Diagnostic follow-up records Worker names and actual terminate() invocations. A cooperative stop flag is written only in final cleanup AFTER measurements/results, to avoid leaking the fixture thread; it cannot make acceptance pass. No production optimization may claim this fatal-path acceptance until the failure is resolved.
