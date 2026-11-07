# Development direction: static recompilation

Decision, 2026-11-07: pursue a separate, game-specific static recompilation
prototype. Keep the current Boxedwine launcher as the compatibility reference.
Do not switch the launcher until the replacement executes the original game.

The target pipeline is:

```text
Original MoviesSE.exe -> recovered functions -> x86-to-C -> WebAssembly
                                                          |
                                      game-specific Windows ABI
                                       /         |          \
                              D3D9 renderer   files/input   media/audio
```

This can remove runtime instruction decoding/JIT compilation and execution of
Wine's Windows/Linux compatibility code. It does not recover original C++ source:
translated code still models x86 registers, flags and guest memory. Performance
improvement is a hypothesis until measured in comparable gameplay scenes.

## Why this route

The user's priority is mouse, animation and simulation responsiveness. Further
archive-loading tweaks do not establish improvements in those areas. Existing
observations identify substantial emulator CPU/memory usage, but do not yet
separate the costs of game logic, Wine, graphics and cross-thread dispatch.

[retro-port](https://github.com/Dhaiwat10/retro-port) provides an MIT-licensed
x86-to-C translator and a Windows/browser runtime. Its D3D8 renderer is useful
reference material; the game requires D3D9, which upstream explicitly calls a
large extension. We pin revision `ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf`
for reproducible experiments, rather than adopt the whole template as a working port.

[Theseus](https://github.com/evmar/theseus) is another credible static translation
reference, producing Rust and Wasm, but describes itself as very experimental.
Neither toolkit is evidence that this game will run without additional work.

## Measured gaps

The local PE audit found 330 ordinary imports and four delayed imports. Comparing
those names with retro-port's registered import handlers gives 171 matches and
163 missing registrations. **A matching name is not a compatibility pass**:
handlers may be partial implementations or constant-return stubs. COM vtables,
dynamic loading and callback semantics expand this surface further.

All four delayed imports lack registrations: Direct3DCreate9,
DirectInput8Create, WMCreateSyncReader and WMCreateProfileManager. The existing
Boxedwine run also proves Windows Media COM initialization occurs during startup.
Returning success without implementing these contracts would hide failures.

Repeat the source-level comparison after generating the PE audit:

```sh
python3 scripts/compare-runtime-imports.py > docs/research/runtime-import-comparison.json
```

The toolkit checkout lives under `.cache/retro-port`. Proprietary binaries,
translated game code, dependencies and build products stay in ignored cache
directories. There is no deployment or external hosting dependency.

## Acceptance gates

Gate 1 has a bounded passing result: two original leaf routines (38 x86
instructions total) compiled into a 1,469-byte standalone Wasm module and passed
1,335 checks in Node's WebAssembly engine. No imports are present in that module.
This covers string length/comparison and calling-convention behavior only;
neither a whole executable nor a rendering path was tested. See the
[recompilation probe](recompilation-probe.md) for reproduction instructions.

1. Execute small real game routines as Wasm and compare results and relevant
   register/stack effects against independent expectations. This is a toolchain
   smoke test, not a gameplay benchmark.
2. Recover the executable's function boundaries and indirect-call targets. Build
   an instruction/translation coverage report. Unsupported instructions and APIs
   must stop with diagnostics rather than silently returning success.
3. Execute original startup code with traced import calls, including threading,
   exception handling and delay imports, through window/device initialization.
4. Implement and validate the actual D3D9 resource/state/shader and media contracts
   exercised by this game. Reach the original menu and load an original studio.
5. Compare sustained frame times, input latency, memory and simulation behavior
   against the reference on the same machine, scene and settings. Validate
   save/reload and audio before replacing the default launcher.

No whole-game speedup, replacement menu, studio gameplay or complete native
compatibility layer is established by the initial audit or routine probe.

## Browser-port precedents and agent task guidance

Primary-source review, 2026-11-07. These comparisons supplement the decision
above; they do not update the historical audit counts or establish whole-game
compatibility. Reported performance from another project is not a prediction
for The Movies.

| Project and approach | What its authors document | Relevance and limit here |
| --- | --- | --- |
| [Serious Sam browser port](https://possiblenull.com/2021/01/26/Serious-Sam-Engine-In-The-Web-Browser/) — source port | Compile the released engine with Emscripten, adapt OpenGL through gl4es, and replace the infinite main loop with an Emscripten callback. The author lists missing multiplayer/splitscreen and audio delay. | Browser integration remains substantial even with source. This does not provide a route to compile our closed-source Windows executable directly. |
| [DiabloWeb](https://github.com/d07RiV/diabloweb) — recovered-source port | Build from Devilution, remove platform dependencies, expose a minimal JavaScript interface, and significantly adapt event handling. Full-game assets are supplied by the player. | Demonstrates dependency replacement and browser event adaptation after source recovery; it is not an arbitrary EXE translator. |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp#how-it-works) — binary recompilation | Translate machine-code functions to C using symbol/metadata input; a separate runtime supplies platform behavior and indirect-function lookup. | Useful architectural precedent for separating translation from compatibility. Its MIPS/N64 instruction and platform implementation is not an x86/Win32 backend. |
| [PS2 Web Recomp](https://github.com/snuri00/ps2-web-recomp) — binary recompilation to Wasm | Translate executable code to C++, implement platform services, and render through WebGPU. Exploration runs collect missing function targets for regeneration. A software renderer provides a graphics reference. The authors explicitly limit their demonstrated game coverage. | Particularly relevant to evidenced callback recovery and semantic comparisons. Its console runtime is not reusable as a Windows compatibility layer, and its reported frame rates are not transferable. |
| [retrowin32](https://github.com/evmar/retrowin32) — CPU interpretation plus Win32 replacement | Interpret x86 and implement the Windows API directly, without a guest Windows installation. The project warns that untested applications are likely to fail and says development moved to another approach. | Confirms the focused API-layer architecture, while illustrating the difference between supporting selected programs and implementing Windows generally. |
| [Boxedwine](https://www.boxedwine.org/) — CPU/Linux emulation plus Wine | Run unmodified 32-bit Wine through emulated CPU/Linux services, including an Emscripten browser build. | Corresponds to our earlier compatibility route; it retains a broader intervening platform stack than direct game translation plus API replacement. |

Browser scheduling must be part of the design. Emscripten documents that newly
created pthread workers can require an event-loop turn before starting, and that
blocking a thread needed for proxied work can deadlock. Worker pools and keeping
the browser UI thread responsive address specific constraints, not game-specific
performance guarantees. See the [official pthread documentation](https://emscripten.org/docs/porting/pthreads).

The following workflow lessons are our inference from those sources and this
port's debugging evidence, rather than claims that the projects prescribe agent
prompts: assign a bounded observed failure; preserve source provenance; separate
translation, ABI, backend and whole-game acceptance; verify semantics before
optimizing; and measure an identical workload before claiming improvement.

Reusable agent task template:

```text
Objective: advance the original executable past [observed stop/checkpoint].
Own only [explicit files/modules]. Other agents are active; preserve their work.
Do not modify original game files or hand-edit generated translation output.

Evidence: inspect original callsite [VA], actual arguments, calling convention,
return value, memory layout, ownership/lifetime and immediate consumers.
Distinguish observed behavior, documented API semantics and inference.
Coordinate shared contracts before changing cross-module interfaces.

Implement the bounded real behavior required by that evidence. Keep unsupported
features explicit through correct errors or diagnostic traps; do not advertise
capabilities or return success for behavior the backend cannot perform.

Verification: add meaningful semantic fixtures for register/stack/memory effects,
state transitions, failure paths and relevant concurrency. For graphics/audio,
check actual output. Then verify the native ABI in the real browser backend and
rerun the original executable to the next checkpoint. A fixture or compiled
module passing does not establish that the original game is playable.

Performance: profile a repeatable scene/checkpoint with tracing disabled; change
one measured bottleneck and compare matching inputs, settings and hardware.
Report limitations and uncertainty. Do not infer FPS from source-port examples,
compiler flags, synthetic fixtures or successful compilation.

Delivery: report changed files, evidence, tests, next original checkpoint and
remaining gaps. Signal source readiness before another agent snapshots a build.
Keep generated-output manifests truthful and record recovery inputs/patch hashes.
```

## Threaded code-size experiments (November 7)

The proven-local SIMD copy rewrite is integrated. Only CPU-register/local-array
copies bypass the generic guest-pointer locking wrapper; guest transfers retain
protection. Full regenerated code matches the baseline after normalizing those
builtin names: 12 of 300 C files change, with an identical header. Native sanitizer
and Wasm comparisons cover 21 forms with 100 randomized states each.
The measured unit200 object falls from 8,638,179 to 4,551,771 bytes; its new compile
takes 39.40 seconds. The historical baseline compile estimate is not a controlled
timing comparison. [Measurement](threaded-local-simd-measurement.json).

The subsequent browser artifact, including new input APIs, is about 4.6 MB smaller
than the preceding threaded artifact (about 1.39%). This is a size observation,
not an FPS or startup-speed result.

A separate blanket noinline scalar-accessor candidate is **rejected**. It preserved
locking and passed correctness checks, but its scalar pthread-Wasm benchmark was
5.60 times slower despite a 20.2% smaller unit200 object. Contention timings were
noisy. It remains isolated and is not used by the port.
[Measurement](threaded-accessor-measurement.json).

### Proposed O1/O2 original-intro comparison (not completed)

Predeclare six complete graphics-client windows ending at Present serials
241, 361, 481, 601, 721 and 841: 720 frames total. In the
[packed-texture checkpoint log](../evidence/static-browser-packed-textures-stop.log),
each has 720 draws, 720 vertex uploads, 720 index uploads, no texture uploads,
and 44,044,800 uploaded bytes. Omit the first window as warmup; later windows
change workload or include intro transitions and asset loading. Require matching
counters **and visual phase/progression** in O2; timing-dependent animation may
invalidate matching by serial alone. Do not select a different interval after
seeing which looks faster.

Use matching executable, generated code, runtime and native source hashes,
including the resource-table fix; only optimization should differ. Keep the same
visible tab, resolution, hardware/power state and profiling options, with other
game instances stopped and compilers/tests idle. Prefer counterbalanced paired
runs O1→O2 and O2→O1; a single pair is exploratory. Record cache/run order because
Wasm tiering and code caching can outlast the omitted warmup window.

Compute aggregate FPS as 720,000 divided by summed interval milliseconds, and
report per-window costs, graphics share and main-thread measured waits. Child
waits overlap and must not be subtracted. Residual time is not proven pure CPU
execution. Preserve behavior/output checks and report unmatched workloads or
failures. Any measured improvement applies to this original intro only, not
menu interaction or gameplay performance.


### First O2 original intro sample (not a speed comparison)

The threaded O2 build `88068a15ac44a9a3e730badc730d921cd812c8a5e4f5032b0cbacdbc9555ea77`
renders the original Lionhead intro. With compiler idle during the measured
intro and individual file logging disabled, the predefined windows ending
241, 361, 481, 601, 721 and 841 contain 720 frames in 30,777.485 ms (23.39 FPS).
This is one intro sample, not a comparison or gameplay result. An equivalent
O1 sample and repeated paired runs are still needed. The later loading portion
overlapped a new diagnostic build and cannot be used for startup benchmarking.
[Partial trace](../evidence/static-browser-o2-capacity-partial.log),
[window aggregates](../evidence/static-browser-o2-capacity-profile.json),
[original intro image](../evidence/static-browser-o2-original-intro.jpg).


### Isolated vertex-helper lock experiment (not integrated)

A bounded study in `.cache/leaf-lock-study/README.md` wraps only the 38-line,
call-free body of translated `f_009e9ab0` in the existing reentrant guest lock,
after its preemption poll. All 26 nested memory accessors, bounds checks and
arithmetic remain. Native ASan/UBSan and pthread Wasm tests pass 500 randomized
CPU/memory comparisons and 20,000 concurrent-observer vertex updates. Three
alternating-order Wasm runs show median helper-loop time 214.8 ms versus 119.4 ms
(1.80× throughput), not measured frame-rate improvement.

The entire parent particle routine is ineligible: it invokes Windows and graphics
services that can block. Fatal bounds/hardware traps inside the candidate can
leave the outer lock held until whole-process termination; recoverable cleanup
and the complete observer contract are not established. The candidate remains
isolated and has not changed the shipped translator or game module.


The required fatal-lock browser acceptance test **failed**. A real bounds trap
occurred with lock depth 1 while the parent was blocked. Both owned Workers
received terminate(), yet the independent C/Wasm heartbeat advanced from 5 to
150 over one second. [Measured result](../evidence/leaf-lock-fatal-cleanup-timing-failed.json).
This is stronger evidence than the earlier JS-timer cleanup fixture and exposes
a gap in the existing whole-pool termination claim for executing Wasm. Cooperative
cleanup was applied only after failure measurement. Leaf-lock promotion remains
blocked while shutdown semantics are investigated; production code is unchanged.


### Startup text-loop candidate (November 7; deferred)

A pre-window API trace repeatedly reached `0x0048ad6c`. Binary inspection
identifies this as a finite per-byte lowercase loop, not a failure retry loop.
The wrapper sign-extends input bytes and processes the stored string length,
including embedded NULs. Its CRT routine obtains thread-local locale state and
can refresh locale ownership; replacing it with host `tolower` or a Unicode
operation would remove side effects and mishandle non-ASCII input. No substitution
was made. A future guarded ASCII-branch optimization must preserve the existing
TLS/locale path and compare all byte patterns, CPU/stack effects, LastError and
locale mutations against the original. No performance benefit is measured.


### Remaining rendering scope and implementation direction

The original material dispatcher has 46 selector values, but these reuse a
smaller set of Direct3D operations. The next implementation step separates
standard blend factors from source-color equations, with real GPU pixel checks
for their combinations. This should remove repeated per-material blend blockers
without accepting unsupported texture operations or claiming full D3D9 support.

Other distinct gaps found by static inspection are multiple texture stages
(mode 14), generated texture coordinates and texture transforms (mode 15),
normal-bearing geometry and active lighting/culling, and render-target texture
creation/switching/copies. Render-target creation rejection has been observed
in the original run. Static presence alone does not establish which remaining
features are necessary for the menu or a playable studio. Shader strings alone
do not prove programmable shader support is required.

A functional main menu, playable studio, original audio/movie workflows, durable
saves and controlled gameplay performance remain unverified. Fixture counts
measure implemented contracts and must not be presented as completion percentages.
