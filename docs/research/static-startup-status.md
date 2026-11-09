# Static original-code startup: verified progress

The original `MoviesSE.exe` entry point (`0x00ad2451`) has executed as statically
translated native code, as WebAssembly in Node, and as WebAssembly inside the
in-app Chromium browser. There is no Wine or Boxedwine in these executions.
No original game bytes were modified. The executable SHA-256 remains
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.

## Original main menu reached (O1 threads, November 8)

The statically translated game now reaches the original **main menu** (Game,
Online, Movie Player, Options, Credits, Quit), rendered through WebGPU at about
17–19 presented frames/s, after about 6,200 presentations. No Wine/Boxedwine is
involved. Fixes since the render-target checkpoint below:

- D3DX surface-copy fallback (`0x00b0e5f1`): lockable `CreateRenderTarget`,
  whole same-size `StretchRect` between render targets (GPU copy), and LockRect
  on lockable render targets (GPU readback on lock, upload on non-READONLY
  unlock). The shared compositing target is copied out repeatedly during load.
- NULL texture on stage 0 (`0x00a24ee7`): per the D3DTSS_COLORARG1 docs, a colour
  argument naming TEXTURE stops the cascade (output = diffuse) and an alpha
  argument naming TEXTURE falls back to DIFFUSE. Only cases whose result is
  exactly diffuse RGBA are accepted; others still trap.
- Hidden pages: rAF-paced acknowledgement stalled Present for 15 s
  (instrumented: `pageAck=15010ms`), and the game then entered its device-lost
  loop and minimized itself (`0x009a5dd3`). The page now publishes a shared
  hidden flag; while hidden, the GPU worker completes Present itself and
  forwards an occasional snapshot. Visible pages keep rAF pacing.
- Diagnostics: graphics-bridge failures print the underlying JS error; slow
  presents (>2 s) report per-phase timings; the game worker flushes its log at
  least once per second.

**Input is the current blocker.** The game reads the mouse only through
buffered DirectInput (`GetDeviceData` at `0x00a96274`), calling `Acquire` every
update (`0x00a96249`). Acquisition requires the canvas to be the focused element
of a visible document that has `document.hasFocus()`. In the Claude desktop
browser pane, driven by the agent, the document is reported hidden/unfocused,
so a live run logged `kind=2 GetDeviceData calls=2400 notAcquired=2399
eventsDelivered=0`. Pointer lock is also refused there ("root document … not
valid for pointer lock"). Menu interaction must be verified by a person in a
focused window (preferably desktop Chrome, where pointer lock works).

## Render-target checkpoint (branch `static-port/playable`)

**DefWindowProcA input messages.** The previous stop at `0x0054262a` was the
original WndProc `0x00542380` forwarding host-injected input to DefWindowProcA.
Its switch table (`0x54263c`/index `0x54265c`) handles only 7, 8, 0xF, 0x10,
0x12, 0x20 and 0x100/0x101/0x104/0x105. Client mouse messages, WM_CHAR and
similar fall through unless UI hook `0x00a097f0` claims them. DefWindowProcA now
returns 0 for client mouse (0x200–0x20B, 0x20E) and plain key/char messages, as
Windows does for a parentless window. WM_XBUTTONUP and WM_SYS* menu semantics
still trap. Covered by a native regression test.

**Nested media decoder worker.** In the Claude desktop browser pane, a
URL-backed Worker created *inside* another worker fails with a message-less
error event, while blob-backed nested workers load. The media reader service
now starts its decoder through a blob bootstrap that imports the same-origin
script by absolute URL.

**Render-target texture stop.** With both fixes, O1-threads reached past the
loading screen (2,186 presentations, 260 s) and stopped at `0x00a24ee7`: a
textured draw with a NULL texture. Shortly before, CreateTexture at `0x0099b270`
(512×512, D3DUSAGE_RENDERTARGET, A8R8G8B8, DEFAULT) had been rejected; the game
ignores the HRESULT. [Trace](../evidence/static-browser-rendertarget-texture-stop.log).
Static analysis shows this is the shared offscreen compositing target
`0x105ca58` (thumbnails, skin blending, cinema sign, post-production UI, fog),
not a shadow map. No RENDERTARGET capability gate exists, and several
GetSurfaceLevel sites are unguarded, so it must be supported.

Implemented (native + protocol + WebGPU, with native/JS contract tests; the
real-GPU fixture pages have not yet been extended):

- RENDERTARGET textures (one level, DEFAULT, A8R8G8B8/X8R8G8B8): GPU-only,
  not lockable, sampleable by later draws.
- SetRenderTarget/GetRenderTarget (index 0) for the backbuffer or a render-target
  texture's level 0. The device holds references while bound. Viewport and
  scissor reset to the full target.
- Per-target depth16, because WebGPU attachments must match in size. The shared
  D3D9 depth contents across targets are not modelled.
- Clear with TARGET alone (observed `0x009a56d7`), and Clear now covers the
  viewport ∩ scissor instead of ignoring the viewport.
- GetRenderTargetData (render target → same-size SYSTEMMEM A8R8G8B8) and
  whole-surface UpdateSurface (SYSTEMMEM → render target), observed at
  `0x009a52d9`/`0x009a5335`.
- Still trapping: StretchRect (`0x00a2b3b4`; the game has a software fallback if
  it *fails*), D3DX surface copy internals at `0x00afe117`, sampling the active
  target, and sub-rectangle copies.

The run with render-target creation reached SetRenderTarget and stopped at the
TARGET-only Clear, which is now implemented. The next full run is pending. A
hidden page throttles presentation to about 1 frame/s, so keep the window visible.

## Previous threaded browser checkpoint

Checkpoint: classic ADD blend factors 1–11 are now independent of the supported
source equations. A 64-entry lazy pipeline cache replaces eager variant creation.
The 296-check real-GPU matrix passes on both output formats
([22](../evidence/graphics-browser-classic-blend.json),
[25](../evidence/graphics-browser-classic-blend-16bit.json)), including all 121
factor pairs, cache eviction/replay and equation combinations. Native, sanitizer
and Node/Wasm checks pass. Full npm checks pass: 106 JavaScript tests and
33 Python tests, with three Python skips. This newer native renderer is not yet
staged into the original-game Wasm build.

The mode19 original-game rerun with cursor retention passed the previous missing
pointer failure but stopped before its first presentation at `0x0054262a`,
unsupported `DefWindowProcA` message.
[Trace](../evidence/static-browser-cursor-retained-window-message-stop.log).
The exact message still needs diagnosis. No original main-menu, studio or
gameplay-performance acceptance is claimed.


The first original mode19 build (`c59141969454ebf33c9921a796ab460b4b526cc36820faac2ea3f355838349b0`)
ended before its first presentation because page focus loss discarded the genuine
launch pointer snapshot. [Failure trace](../evidence/static-browser-startup-cursor-loss.log).
Launcher retention now preserves genuine observations across blur/leave/hidden,
without inventing an initial point or moving the retained virtual cursor during
inactive scroll/resize. Held inputs and focus still release. Fifteen focused
input tests pass; browser verification is pending. This run does not verify
mode19 in the original game.


The next evidence build stores validated classic D3DBLEND settings and logs
each distinct factor with its wrapper parent. Unsupported active combinations
still stop before GPU execution. Native, sanitizer and Node/Wasm checks pass.
The original-game rerun confirms material 19: wrapper parents `0x00993d58` /
`0x00993d61`, ZERO/SRCCOLOR, FVF144, stride 28, bound texture, SELECTARG1 diffuse
RGB and SELECTARG1 texture alpha. [Trace](../evidence/static-browser-mode19-pipeline-stop.log).
The exact equation now passes native, sanitizer, Node/Wasm and 160-check GPU
fixtures for both output formats ([22](../evidence/graphics-browser-mode19.json),
[25](../evidence/graphics-browser-mode19-16bit.json)). Full npm checks pass:
104 JavaScript tests, 33 Python tests, three Python skips. The combined original-game
run is in progress; main-menu and studio gameplay remain unverified.


The next evidence run (`808f6664843632cb57c309ea90ada31c5a7df31c9d830fdef6bba6bcbb4833e4`)
identified the sprite failure precisely: `SetStreamSource` rejected stride 28
from the alternate screen-space FVF144 branch, leaving the previous binding.
The alternate branch sets FVF144 at `0x009976be`, stream stride 28 at
`0x009976e3`, and rejoins indexed draw `0x00997abb`. The earlier FVF142 audit
covered a different branch. Bound FVF144 support now passes native, sanitizer,
Wasm and 149-check GPU fixtures for both output formats
([22](../evidence/graphics-browser-bound144.json),
[25](../evidence/graphics-browser-bound144-16bit.json)). Subsequent original-game
verification is recorded below.

The same original run confirmed material mode 20 via wrapper parents
`0x00993e43`/`0x00993e4c`: SRC_COLOR/INV_SRC_COLOR, ADD, separate alpha off,
texture NULL and SELECTARG1 diffuse for RGB/alpha. It stops at draw `0x00997abb`.
[Full evidence](../evidence/static-browser-blend-pipeline-stop.log). This exact mode now passes native, sanitizer, Node/Wasm and 152-check real-GPU
fixtures ([22](../evidence/graphics-browser-mode20.json),
[25](../evidence/graphics-browser-mode20-16bit.json)). The browser presentation
check expects opaque alpha, while canonical readback separately validates render-target
alpha. Full npm checks passed: 103 JS tests and 33 Python tests, with three Python
skips. The combined original-game build (`a651baa881a21d631752762f1377bd0bce61557f66a082af6bf5137760672f3e`)
now accepts the formerly rejected sprite: first-frame draws are 7/7 and initial
loading-frame draws are 63/63. The [loading screen and progress bar](../evidence/static-browser-bound144-loading.jpg)
render. The original run passed the previous active mode 20 blend trap and stopped
next at SRCBLEND=ZERO through wrapper `0x0088804a`.
[Trace](../evidence/static-browser-mode20-next-blend-stop.log). It reached 2,093
presentations and 264.2 seconds total startup execution; these are loading/run
measurements, not gameplay FPS. No main-menu result is claimed.


The material-35 build (`49f569c13a6450ea97ed0599fd68d357f4a025c77b3a1cb7dece7362e11be54c`)
rendered the [original loading screen](../evidence/static-browser-original-loading-screen.jpg)
and advanced to SetRenderState SRCBLEND=3 at wrapper `0x0088804a`.
[Full trace](../evidence/static-browser-factor-renderstate-stop.log). It presented
2,096 frames before stopping; this mixed startup/loading run is not a gameplay
benchmark. At that checkpoint, sprite draws at `0x00997abb` were still rejected;
the bound FVF144 fix above resolved them.
The next evidence build accepts legal SRCBLEND=3/DESTBLEND=4 state storage,
but still traps unsupported active blend combinations at draw and logs their
full state. It also reports exact indexed-draw rejection reasons.

The native mouse fixture now reports PASS in real Chrome for capture, native
warp, real movement and release. Original-game captured gameplay remains unverified.


The FVF104-enabled original build (`c961bed0c7981a5e856807863539216dd337165fcafd324c30de0ee38067a2e3`)
passes that format setter and stops next at SetTextureStageState caller
`0x00992ee3`, stage0 COLOROP=SELECTARG2. [Trace](../evidence/static-browser-fvf104-stage-stop.log).
The original material mode 35 requires RGB=textureFactor.rgb and
alpha=textureFactor.a × sampledTexture.a, with SRCALPHA/INVSRCALPHA blending.
That exact equation now passes native, sanitizer, Wasm and JS tests plus
148-check real-GPU fixtures ([22](../evidence/graphics-browser-factor-alpha.json),
[25](../evidence/graphics-browser-factor-alpha-16bit.json)). The GPU computes
the equation directly before alpha-test/blend without rewriting vertices or
textures. Legacy draw command shapes retain their modulate default. Unrestricted
texture-stage emulation is not claimed. This run stops before a main-menu rendering result.


The combined build now advances to a strict SetFVF stop at original caller
`0x00a59901`, requesting FVF `0x104` (XYZRHW + TEX1 without diffuse color).
The previously clear-only portion now issues actual quad draws, with complete
120-frame windows uploading 96 vertex bytes per frame and reusing the index
slice. This proves the corrected buffer-lock path is exercised, not menu visual
correctness. The final canvas remains black. Execution reached this stop after
108,867.8 ms and 2,015 presentations, before the earlier full frontend loading
boundary; that duration cannot be compared as a startup-speed improvement.
[Trace](../evidence/static-browser-sliced-fvf-stop.log),
[capture](../evidence/static-browser-sliced-fvf-stop.jpg). FVF 0x104 bound-buffer support now passes native, sanitizer and Wasm tests,
plus both real-GPU fixtures ([22](../evidence/graphics-browser-fvf104.json),
[25](../evidence/graphics-browser-fvf104-16bit.json)), now 141 checks. Native upload
conversion preserves 24-byte guest vertices and uses 28-byte RHW vertices with
default white diffuse as specified by [D3DTA](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dta).
Original-game verification is next.


The current O1 build (`24e2eee518305ab6a7f622f28680b92bc4dd988bab7b4853534bb5e76977c75c`)
adds NOSYSLOCK buffer handling, referenced vertex/index uploads, and native
WM_SYSCOMMAND/SC_RESTORE with a browser Restore button. The preceding NOSYSLOCK
run minimized its virtual window after the intro, leaving a hidden canvas with
no restore affordance ([trace](../evidence/static-browser-nosyslock-minimized.log));
it does not establish the graphics fix's frontend outcome. Restoration now
requires the original WndProc and host acknowledgement, and preserves the last
genuine cursor point while minimized so GetCursorPos cannot block that queue.

Sliced-upload native/sanitizer/Wasm regressions cover offsets, signed base vertex,
INDEX32, sparse indices, caching and unchanged guest data. Real-GPU fixtures
pass 140 checks ([22](../evidence/graphics-browser-sliced-buffers.json),
[25](../evidence/graphics-browser-sliced-buffers-16bit.json)), including a 4 MiB
buffer with unused NaNs outside the referenced quad: upload is 96 vertex bytes
plus 12 index bytes. Unused holes *within* a contiguous referenced span still
undergo backend validation. No frame-rate improvement is claimed yet.

### Earlier capacity/frontend checkpoints

The capacity-corrected original run now passes the NULL SYSTEMMEM failure and
initializes the frontend. At presentation 4096, the caller is `0x00447cb1`,
SYSTEMMEM texture/surface are nonzero, and the frontend ready byte is 1 with
its video-completion callback cleared. Frames still have zero draw calls: this
is a black frontend loop, not a playable menu. Wasm SHA-256:
`9d425f6b96306a9ea4cbab7d9c3bd72878f041e46cc65de3363a2c0597066294`.
[Stopped-run trace](../evidence/static-browser-capacity-frontend.log),
[capture](../evidence/static-browser-capacity-frontend.jpg). O2 linking overlapped;
this run is not a controlled performance measurement. O2 with the same resource
implementation also reaches the identical zero-draw frontend loop:
[O2 trace](../evidence/static-browser-o2-capacity-frontend.log).

The next diagnostic (`d328fd29c2c5b5929bb30384ecbb1c8a3460224a8edac748a7894fab4bf5dcc3`)
identifies the original main state object and shows transition from state 1 to 2.
The state callbacks are intentionally RET/null in the original executable, so
this does not explain the missing UI. Shared rendering instead traverses the
21-entry callback table at `0x105c618`, subject to separate global gates.
[State trace](../evidence/static-browser-frontend-state.log). Investigation now
follows that shared rendering path; no game-state bypass has been applied.

The shared-UI diagnostic (`194ec8b0c1ef6f81c90e9cd381e94c311d18e840f19415f962d4db61135c682c`)
shows callback slot 11 registered, both shared suppression bytes zero, root
visibility bit 0x10 set, no optional root gate, and a live 4 MiB vertex buffer.
[UI-gate trace](../evidence/static-browser-ui-gates.log). The main-loop no-op
callbacks are normal. A concrete mismatch remains in buffer Lock: original
calls use NOSYSLOCK with NOOVERWRITE/DISCARD, while the native allowed mask
rejects NOSYSLOCK. Support and bounded failure diagnostics now pass native, sanitizer and Wasm
contracts plus both real-GPU fixtures ([22](../evidence/graphics-browser-nosyslock.json),
[25](../evidence/graphics-browser-nosyslock-16bit.json)). The original UI dynamic
VB uses usage 0x208; the separately audited usage-8 buffers belong to the static
list. Dynamic restrictions remain enforced. Original visual recovery is pending;
this is not yet proven to be the only cause of missing menu draws.

### Prior packed-texture capacity failure

The packed-texture build (`8ec1e1eca83f95d38634ead72affe1e6aac1ca4382351f18399f6a80bf341361`)
stops again at the unchecked NULL texture call, after 2,093 presentations.
[Trace](../evidence/static-browser-packed-textures-stop.log). Newly supported
managed textures make the compatibility layer's 128-object resource ceiling a
concrete failure candidate: an isolated reproduction fills 122 texture slots plus
six other objects and reproduces SYSTEMMEM E_OUTOFMEMORY with over 4 MiB of guest
heap still available. The original run did not record its object count. A
resource-capacity correction now uses 4096 lazily allocated metadata slots.
Native, sanitizer and Wasm tests verify exhaustion and released-slot reuse.
Real-GPU fixtures pass with 130 live managed textures followed by SYSTEMMEM
allocation and cleanup ([22](../evidence/graphics-browser-resource-capacity.json),
[25](../evidence/graphics-browser-resource-capacity-16bit.json)). The original run above now passes this allocation boundary. The bounded Present trace
identifies `0x0041fe06` as the original loading/progress renderer, with zero draws
and no frontend manager yet. This does not identify the previous post-loading
black-loop cause. Main-menu rendering remains unverified.

### Earlier SYSTEMMEM checkpoint

SYSTEMMEM support removes the unchecked NULL texture failure. The next original
run continued through frontend/character assets but entered a persistent black
presentation loop. It was manually stopped after 35,171 total frames; this is
**not a playable menu** and its high clear-only presentation rate is not gameplay
performance. An O2 compilation ran concurrently, so this was a compatibility run.
[Log](../evidence/static-browser-systemmem-black-loop.log),
[capture](../evidence/static-browser-systemmem-black-loop.jpg).
Wasm SHA-256: `9b410679f193fb5e083893cbecfa53ba5af932745e895f0f343c5f68e39b2b96`.

The run exposed actual managed texture requests for DXT1, DXT3 and A4R4G4B4.
These now preserve guest packed/block storage and decode to RGBA for sampling.
Native, sanitizer and Wasm contracts pass. Both real-GPU fixture formats pass
139 checks, including packed/compressed sampling and SYSTEMMEM resource ownership:
[format22](../evidence/graphics-browser-packed-textures.json),
[format25](../evidence/graphics-browser-packed-textures-16bit.json).
The fixture packaging regression that omitted graphics-metrics.js is fixed and
covered by import-closure tests. Original-game execution with these formats now reaches the resource-capacity
failure above; frontend visual verification remains pending.

### Previous NULL texture checkpoint

The 176-handler cursor/wait-profile build passes the previous `SetCursorPos`
boundary and reaches a NULL texture call at `0x009a5a1f` (return `0x009a5a22`).
The original ignored failure of its dynamic SYSTEMMEM A8R8G8B8 texture allocation,
then called GetSurfaceLevel on the NULL pointer. This is not a missing function
registration. Real CPU-backed texture support is now implemented and tested;
verification in the next original run is pending. The separate unsupported
DEFAULT-pool render-target allocation still fails truthfully; the original has
a checked fallback for that resource, which must be verified live.

[Completed run](../evidence/static-browser-native-waits.log): 2,094 presented
frames, 306,431.9 ms diagnostic execution excluding dialog interaction, 196 range
requests / 51,380,224 bytes and 2,625 cache hits. Wasm SHA-256:
`db8d2774053baa62868525f1f1ce346bc39a710bf0e679370891d39d02cfc29f`.

[Matched graphics/wait windows](../evidence/static-browser-wait-profile.json)
show 1,320 draw-containing intro frames in 43,008.635 ms (30.69 frames/s).
Graphics bridge calls account for 7,613.865 ms (17.703%); one main Sleep(10) totals
11.710 ms, and no main condition-variable wait appears in those windows.
The remaining 82.270% is not proven pure CPU: critical-section contention,
filesystem/media calls and translated execution remain outside this measurement.
Child wait intervals overlap and are not subtracted from frame time.

### Previous power-status checkpoint

The 175-handler power-status build advances through frontend, costume and prop
initialization. It stops at `USER32!SetCursorPos`, caller `0x00555220`.
[Profiled run log](../evidence/static-browser-frontend-cursor.log) records 2,097
presented frames and 305,006.0 ms diagnostic execution excluding dialog time;
200 range requests fetched 52,428,800 bytes with 2,640 cache hits.
Wasm SHA-256: `7b47675adb558e4a603284e57eb71aea287076d0767a9c5d7366d0df66270806`.

[Graphics timing aggregates](../evidence/static-browser-graphics-profile.json)
cover 11 complete 120-frame windows with draws: 1,320 frames in 42,593.145 ms
(30.99 presented frames/s), 7,333.805 ms in synchronous graphics bridge calls
(17.22%). Workload varies: one heavy particle window averages 18.27 frames/s
with 7.95% in the bridge, while later clear-only windows approach 100 frames/s.
Time outside the bridge is not yet attributed solely to CPU execution; native
wait instrumentation is being added. The complete first-to-last mean is only
9.26 frames/s because it includes lengthy frontend loading. No gameplay claim.

### Input and profiling implementation

`SetCursorPos` now has a virtual-desktop implementation: it succeeds only during
actual browser pointer capture, clamps to the virtual desktop, and updates an
atomic cursor position without emitting synthetic DirectInput deltas. Uncaptured
warps return FALSE/ERROR_NOT_SUPPORTED. Capture loss retains the last validated
virtual cursor for unconditional original queries, rejects subsequent warps, and
allows a fresh absolute page sample to replace that retained position.

Native/sanitized/Wasm API tests and JS race/input tests pass; the integrated suite
passes 98 JS tests plus 33 executed Python tests (three skipped). Real pointer
capture is **not yet verified**: both embedded-browser and Chrome attempts were
rejected while the Mac was locked. Computer-use reported the locked Mac, so this
is not evidence of an embedded-browser-specific restriction. User unlock was
requested; independent startup testing can continue with truthful warp failure.

The launcher now offers Capture mouse and Escape release, plus opt-in graphics
and native wait profiling. Wait reports separate main/child completed Sleep and
condition-variable intervals, discard startup at the first Present, and aggregate
120-frame windows. Native and pthread-Wasm tests pass. The latest run above supplies the first original-game measurements.

### Previous clock checkpoint

The 174-handler clock build advances beyond the Lionhead intro into original
frontend asset loading. `timeGetTime` now works. The game rejects the unsupported
custom video filter (`AddFilter` returns `E_NOINTERFACE`) and follows its own
failure handling; this is not successful WMV playback. The next observed stop is
`KERNEL32!GetSystemPowerStatus`, caller `0x006a3f4b`.

[Completed run log](../evidence/static-browser-frontend-power.log): 2,043 actual
Present frames; first-to-last interval 132,140.1 ms, mean 15.45 presented frames/s.
That interval includes a long frontend asset-loading gap and is not an animation
or gameplay benchmark. Diagnostic execution took 197,534.8 ms excluding dialog
interaction; 159 range requests fetched 41,680,896 bytes, with 918 cache hits.
Wasm SHA-256: `3426d84a0a27bf5aaaee410356ca9418a61161b6116faf571b17597f728c2c9e`.
The main menu and studio have not rendered on this path yet.

### First original animated rendering checkpoint

**First original animated rendering is verified.** The
[original Lionhead image](../evidence/static-browser-first-original-frame.jpg)
and [completed run log](../evidence/static-browser-intro-present.log) come from
the translated executable, not a synthetic fixture or a video substitution.
The browser consumed 1,291 actual Present frames at 1024×768. Native diagnostics
also confirm the first backend-acknowledged DrawIndexedPrimitiveUP at
`0x009eb1a3` (two primitives). The run then stops at `WINMM!timeGetTime`, called
from `0x00c8b92d`, while initializing the next intro media stage.

This 173-handler build's Wasm hash is
`df1a327a2282d73e86734426d9fea19ca00adad946e91cebfd49c931892d4793`.
The UI reported `Game frames: 485 (20.0 presented frames/s)` at one observation;
final count was 1,291. This is one presentation-rate interval during intro/loading,
not a full-run average or a menu/studio gameplay benchmark. Total diagnostic
execution to the stop was 102,302.4 ms excluding dialog interaction; asset reads
were 70 requests, 18,350,080 bytes and 174 cache hits. No Wine/Boxedwine is involved.
The main menu, studio gameplay, movie playback and gameplay performance remain
unverified on this static path.

### Earlier threaded checkpoints

The [combined depth/blend run](../evidence/static-browser-intro-blend.log)
([capture](../evidence/static-browser-intro-blend.jpg)) reaches
`SetScissorRect` at `0x009eb425`. Its Wasm hash is
`19b967bb0b69d5fd083c4fae1f2d42c0ee1a4ed2f3018afda0a76d72c0cc8eb3`.
Startup to this stop took 61,047.4 ms excluding dialog interaction; the frame
counter remained zero. Control flow proves an earlier DrawIndexedPrimitiveUP
call returned, but its HRESULT is ignored by the game, so this alone does not
prove successful submission or visible pixels.

Subsequent scissor work passes 135 real-GPU checks in
[32-bit](../evidence/graphics-browser-scissor.json) and
[16-bit](../evidence/graphics-browser-scissor-16bit.json) targets. These include
clipped draws and clears, depth, blending and preservation outside the rectangle.
The 16-bit blend path uses the documented RGBA8-intermediate/per-guest-draw
conversion model, not a claim of bit-exact legacy-driver behavior.

The [173-handler cursor run](../evidence/static-browser-intro-cursor.log)
([capture](../evidence/static-browser-intro-cursor.jpg)) passes the observed
navigation/mouse-key queries and real cursor-position conversion. The next stop
is `IDirect3DDevice9::SetVertexShader(NULL)` at `0x009eb0c0`.
Wasm: `56ce899787247a2bc58272c506a6c3fd3f7fd2b94aad5ca03429ce8024f30619`.
Elapsed startup: 61,498.4 ms excluding dialog interaction. No original Present
was observed in this run.

Subsequent renderer work supports NULL shader bindings, FVF `0x144`
(XYZRHW/diffuse/UV), and INDEX16 triangle-list `DrawIndexedPrimitiveUP`.
Real browser fixtures pass 55 pixel checks in [32-bit format](../evidence/graphics-browser-rhw-up.json)
and [the original 16-bit format](../evidence/graphics-browser-rhw-up-16bit.json),
including half-pixel alignment, perspective UVs, screen-linear colors and
pretransformed depth. These fixtures are synthetic; they do not prove an original
game frame or gameplay. Unsupported blending and other states remain explicit.

The [171-handler run](../evidence/static-browser-intro-keys.log)
([capture](../evidence/static-browser-intro-keys.jpg)) passes actual foreground
and intro skip-key queries, then stops at another `GetAsyncKeyState` query:
Page Up (`0x21`) from `0x009e9d5b`, in the intro drawing/update path.
Wasm: `a5cb3d0a90da12075ffc2e9f71e2e771844cb527388539c9cb5c9bba0cb6fd2a`;
324,954,673 bytes. This build includes the verified local-SIMD-copy rewrite.
It is 4,585,883 bytes smaller than the preceding 169-handler browser artifact,
but these runs do not establish a gameplay speedup. Startup to this stop took
61,945.6 ms excluding dialog interaction. No original Present was observed.

The [169-handler run](../evidence/static-browser-thread-debug-output.log)
([capture](../evidence/static-browser-thread-debug-output.jpg)) passes diagnostic
output and stops at `GetForegroundWindow` from `0x0045df3d`, inside the original
intro presentation loop. It has not yet reached a verified original Present.
Wasm: `af32161c598c749384c163a2346f537a6eaa026a92879af28b925576599ac6b5`.
Elapsed startup: 61,243.9 ms excluding dialog interaction; 70 asset requests,
18,350,080 fetched bytes. No gameplay timing claim follows from this checkpoint.

The [168-handler run](../evidence/static-browser-thread-systeminfo.log)
([capture](../evidence/static-browser-thread-systeminfo.jpg)) confirms that two
original CRT child-thread entries actually start: IDs 257 and 258, both at
`0x00acf974`, with independent guest stacks. CPU information and affinity checks
pass, and the next stop is `OutputDebugStringA` at `0x00bbe684`. This build's Wasm
hash is `fbb3dce7959b60ba2346f130dde39c1d230a51030412ae429c3c9bd7b6f2c08a`.
The run took 62,726.9 ms excluding dialog interaction, with 69 range requests and
18,087,936 fetched bytes. Child-thread completion and gameplay remain unverified.

The [164-handler threaded run](../evidence/static-browser-first-thread.log)
([capture](../evidence/static-browser-first-thread.jpg)) passes the previous
`CreateThread` boundary and stops at `GetSystemInfo`, called from `0x00c9a73e`.
Its browser Wasm SHA-256 is
`e8895a92232674dfc301a1db7757281204bd4a3cde9a53fb59b63898cdff29ab`.
This uses a real two-worker pthread pool and the page-owned worker transport.
The log establishes progress past thread creation; it does not yet establish
the original child routine's execution or completion. The run took 60,681.6 ms
excluding dialog interaction, with 68 range requests and 17,825,792 fetched
bytes. This is a startup checkpoint, not gameplay performance evidence.
No original game frame or interactive gameplay has been demonstrated.

## Earlier checkpoints

The browser proof is [this captured diagnostic](../evidence/static-browser-startup.jpg)
and [its original-entry/API trace](../evidence/static-browser-startup.log).
The worker executed original CRT startup through heap creation, critical-section
and TLS setup, standard handles, command line, environment and code-page setup.
That captured build stops explicitly at the then-unimplemented non-ASCII
`GetStringTypeW` operation. Its terminal code is **4**, an intentional guest trap.
Later native API work has advanced beyond that captured snapshot; it does not
retroactively change what the browser evidence proves.

A [second browser capture](../evidence/static-browser-constructors.jpg) and
[trace](../evidence/static-browser-constructors.log) verify the later build passing
locale initialization and the security-cookie initializer, then reaching the
game constructor's `InterlockedIncrement` call at `0x00acbf67`. It also ends with
an explicit code-4 trap. The native build has subsequently implemented atomics
and private shell folders and reached `RegCreateKeyExA` in a later initializer.

A [third browser capture](../evidence/static-browser-files.jpg) and
[trace](../evidence/static-browser-files.log) verify 78 API handlers and mounting
all 719 original files with one shared 32 MiB range cache. This build passes
registry initialization and the repaired final-code boundary in the translator,
then stops at `FindFirstFileA` called from `0x00ac6151`. Native and Node/Wasm
execution reach the same stop with matching registers. The browser Wasm SHA-256
for this capture is
`ff34fdf66aaa336d30f01a23b7fab8e3521f25fc21cf02b46ffb3272ec423a22`.
Directory enumeration has since been implemented, including the virtual parent
directories needed by the original CRT's installation-path canonicalization.

The [fourth browser capture](../evidence/static-browser-sync.jpg) and
[trace](../evidence/static-browser-sync.log) verify 89 handlers, successful path
searches, mutex creation and genuine worker sleep during timer calibration.
It reaches `SetEnvironmentVariableA` at `0x00ad0fc6`, again matching native and
Node/Wasm registers. This snapshot's Wasm SHA-256 is
`575bd2ba4c46d4d1ff5e2f3598e4d4a5732e19ca3bcf062d51a8087f07e0c1eb`.
The measured 367.2 ms is one diagnostic's execution through that stop, including
image loading and function registration, excluding Wasm download/compilation.
It is **not** an FPS, input-latency, full-startup or Boxedwine comparison result.
No asset range reads were requested before this stop; the executable was preloaded.

The [97-handler browser capture](../evidence/static-browser-assets.jpg) and
[log](../evidence/static-browser-assets.log) verify 97 handlers and actual reads
of `stringdatabase.lhts` and `stringdatabase_addon1.lhts`. The original code now
requests its first-run graphics-quality notice through `MessageBoxW` at
`0x00542e1b`. That USER32 call remains a strict trap; its original title/text are
reported diagnostically, with no invented dialog response. Native, Node/Wasm
and browser reach the same call and registers.

## Startup performance observation

Detailed import tracing produced 1,469,466 lines (about 149 MiB) in this startup
path, including roughly 466,600 calls each to GetLastError, TlsGetValue and
SetLastError. It is now opt-in (`--trace-imports` or the diagnostic checkbox).
File-access summaries and explicit traps remain enabled.

The heap allocator also scanned an ever-growing allocated prefix. An earliest
free-block cursor now skips it while preserving first-fit order, ownership and
coalescing. A test-only visit counter guards against quadratic live-allocation
scans; fragmentation and fixed-heap tests remain passing.

With import tracing **off in both browser runs**, the same original startup
section took **8,841.0 ms before** and **4,483.0 ms after** the heap fix. Both
reached the same first-run dialog request, read the same files, and fetched
2,359,296 bytes in nine range requests. These are single local observations,
including image load/registration and asset reads but excluding Wasm fetch and
compilation. They do not establish gameplay FPS or compare against Boxedwine.
Build hashes and source snapshots: [before](../evidence/static-browser-assets-before-heap.json),
[after](../evidence/static-browser-assets.json).

## Interactive dialog milestone

The 99-handler browser build now displays the original first-run notice in an
[interactive dialog](../evidence/static-browser-first-dialog.jpg). The text comes
from original game memory. Clicking OK returns IDOK through a SharedArrayBuffer
response to the waiting Wasm worker; no response is selected automatically.
The worker uses Atomics.wait, not a busy loop or Asyncify. The browser was
observed resuming and reaching `GetDiskFreeSpaceExW` at `0x00a0fe1a`:
[post-dialog trace](../evidence/static-browser-after-dialog.log).
Wasm SHA-256 for this proof:
`0ae16f5598932cfb2464b54699b0c8f9b57326fb41f6df37ec6167f4e48c517d`.

MessageBoxA/W validate string encoding, flags, default buttons and returned IDs.
Seven button sets are supported. Owner windows, HELP/service flags and other
unsupported behavior fail explicitly. Native/Node diagnostics without a UI
callback still stop at the dialog; their result is no longer the browser's
furthest execution point. Browser timing now excludes time waiting for a dialog
response and remains unrelated to gameplay FPS.

Storage queries and temporary files now work; see the later archive-loading
checkpoint below. Window/callback handling and the actual Direct3D9 bridge remain. See the [rendering call map](d3d9-call-map.md)
and [delay-import audit](delay-import-binding.md). Windowing and rendering are
not implemented yet.

The original menu, studio gameplay, rendering, audio, save/reload and sustained
performance have **not** been established on this new path. Main-thread heartbeat
progress while the worker runs does not measure game FPS or input latency.
The existing Boxedwine build remains the earlier main-menu reference.

## Recovered and generated code

- Ghidra exported 59,557 functions and 1,138 computed-jump records; 608 jump
  records have no statically resolved targets.
- The translator added heuristic candidates, generating 119,561 functions and
  approximately 331 MB of C. This is not a count of verified game functions.
- The generation report includes unsupported instructions and explicit traps;
  some candidates may be data mistaken for code. Only execution establishes
  whether a trap lies on an exercised path.
- Native compilation succeeded. A large registration function was split into
  468 ordered batches of 256 entries or fewer. The dispatch table was patched
  to grow instead of hanging near its fixed upstream capacity.

Metadata: [recovery](function-recovery-result.json),
[translation](full-translation-result.json). Game-derived C and binaries stay in
ignored cache/runtime directories.

## Repeat the diagnostic

See [native build instructions](../../native/README.md). After a completed
`scripts/build-bootstrap.py --startup-apis --target wasm` run, stage that exact
snapshot for the browser:

```sh
python3 scripts/stage-static-browser.py "/path/to/Game Files/MoviesSE.exe"
python3 scripts/index-game-archive.py
npm start
```

Open `http://127.0.0.1:4173/static-probe.html` and click **Run original startup**.
It runs in a worker and reports the actual terminal status. The staged manifest
records object, source, executable and Wasm hashes. It is a diagnostic, not the
game launcher. Browser output may lag ongoing native development until restaged.

The first verified browser module is 150,677,893 bytes (uncompressed), SHA-256
`60ff30637993e83bd151c27fe18f0563caaecdb56089953c01de4f8ad1744052`.
This large, unoptimized bring-up build is not a final download/performance budget.

## Original PAK archive indexing checkpoint

The 110-handler browser build continued after the first-run dialog, queried its
256 MiB private temporary storage quota, wrote its private osdetect file, and
created/deleted a temporary file. It then opened the original addon, meshscene,
misc, premium_costumes and texture PAK archives and read their indexes.
The [browser trace](../evidence/static-browser-pak.log) records the actual stop:
`KERNEL32.DLL!GetFullPathNameA`, called from `0x00ae8c37`.
[Captured diagnostic](../evidence/static-browser-pak.jpg).

This unoptimized run took 166,293.5 ms, excluding dialog interaction. Its range
cache fetched 9,699,328 bytes in 37 requests with 23 cache hits. This is a much
longer executed path than the earlier 4.48-second pre-dialog checkpoint; the
numbers are not a before/after performance comparison. No gameplay was rendered.
Private browser files remain volatile and are discarded with the worker.

The resource module additionally parses the original executable's icon 1011
(16/32/48 pixel images, selecting 32 pixels) and provides typed arrow-cursor and
white-brush metadata. Native, sanitizer, Wasm and actual-PE checks passed. It is
wired into startup and was subsequently exercised by original browser window
creation in the 132-handler run below. The native test runner includes its contract tests.

## Optimized compilation comparison

An isolated O1 build reached the same `LoadLibraryA` stop at `0x00a0ce8d` as
O0, with identical printed register state and range-cache totals (38 requests,
9,961,472 bytes, 23 hits). The requested library at this call is `imagehlp.dll`,
used for optional debug symbols. O1 took **89,265.5 ms**; a repeated O0 run after
compiler completion took **165,489.7 ms**. This single local comparison is about
46% less startup execution time, not an FPS or input-latency measurement. An
initial O0 run overlapped compilation and is excluded from this comparison.

Evidence: [O1](../evidence/static-browser-library-o1.log),
[O0 repeat](../evidence/static-browser-library-o0-idle.log),
[O1 screenshot](../evidence/static-browser-library-o1.jpg),
[profile provenance](static-o1-first-profile.json).
O1's Wasm was 130,294,473 bytes. It uses no fast-math; float contraction and
strict aliasing are disabled, and signed wrapping is retained. Identical stops
establish only this exercised startup path, not all translated code correctness.

A subsequent heap change replaces linear allocated-pointer lookup and whole-list
coalescing with a host address index and adjacent-block merging. Tests preserve
first-fit ordering, contents, ownership and allocation-failure rollback. Its
native microbenchmark improves sharply; browser impact must be measured on the
subsequent build rather than inferred from those microbenchmarks.

## Heap index and window prerequisites

The subsequent 132-handler O1 browser build processed the same 38 archive ranges
and continued past optional imagehlp loading to `GetSystemDirectoryA`, called
from `0x009a6992`. It took **33,428.8 ms** to that later boundary, excluding dialog
interaction. This is a new, longer executed path rather than the identical-stop
O0/O1 comparison above. [Trace](../evidence/static-browser-heap-window.log) and
[screenshot](../evidence/static-browser-heap-window.jpg).

The build includes real class/window state and original WndProc dispatch, a
synchronously acknowledged canvas host, a bounded message queue and a shared
browser pointer-event ring. The browser DOM and screenshot confirm that original window creation completed:
a visible canvas labelled The Movies, 1024×768, cursor none. Creation and show
invoke the translated original WndProc, and execution continued past them. The
canvas is blank: rendering and mouse dispatch are **not** established.
Keyboard translation is explicitly unsupported. The next observed compatibility
boundary in that captured build was the system-directory query. Later progress
is recorded below.


## Original media preflight and normal process exit

The 138-handler O1 browser build implements system-directory queries and genuine
`ExitProcess`. It creates the original 1024×768 window, then displays the original
“Windows Media Player or one of its components could not be found” error.
Acknowledging it produces the game's own `ExitProcess(1)`, rather than an
unsupported-API trap. [Trace](../evidence/static-browser-media-exit.log) and
[screenshot](../evidence/static-browser-media-exit.jpg).

This run took **34,400.8 ms**, excluding dialog interaction and Wasm fetch/compile,
with 38 range requests, 9,961,472 fetched bytes and 23 cache hits. The canvas remains
blank. No gameplay FPS, game input latency, or end-to-end rendering is established.
The DirectInput adapter and browser event transport have contract tests but have
not yet been reached by original game execution at this boundary.

Separate component probes establish actual WebGPU indexed draws, depth, texture
sampling and presentation through a dedicated worker while a simulated guest
blocks ([transport result](../evidence/graphics-browser-transport.json)). Actual
Wasm codecs decode the original WMV3/WMAv2 intro into bounded timed frame/audio
queues ([timed result](../evidence/media-browser-timed.json)). This timed test is
muted and does not establish audio-device synchronization or game playback.
A bounded WMV2/WMAv2 ASF encoder also round-trips real data
([writer result](../evidence/media-browser-writer.json)); WMV3 encoding is absent.
These are backend components, not proof that the original game uses them.

The native COM graph/writer foundation requires a real backend session before
creating its writer. The WMVCore module is not advertised as installed merely to
bypass the original preflight. Connecting the codec service, Windows Media
interfaces and Direct3D9 adapter remains work in progress.

## Connected browser service lifecycle

The launcher now prepares page-owned GPU and codec workers before calling the
original entry point. The GPU uses acknowledged, animation-frame-paced bitmap
presentation; the codec service verifies its artifacts and real encoders before
exposing writer callbacks. Stop, terminal exit and worker errors clean up those
services. A repeated 138-handler original-game run reached the same missing-media
exit in 33,955.2 ms with both services ready
([trace](../evidence/static-browser-backend-services.log)). This establishes
service initialization alongside original execution, not game use of their APIs.

The page-owned media service fixture passed actual ASF encoding, bounded chunked
output and rejection tests. The GPU fixture passed again with animation-frame
acknowledgement ([result](../evidence/graphics-browser-raf.json)).
A 143-handler O1 build now includes the native COM and Direct3D9 adapters and is
staged for further browser execution. Native contract tests cover 17 modules;
mocked native GPU tests remain distinct from real GPU browser tests.

The native Direct3D9 ABI has now also been exercised against the actual browser
GPU: factory/device creation, COM vtable dispatch, vertex/index buffer locks and
uploads, clear, indexed draw, present and release. Readback matched the expected
green triangle center and RGBA(17,34,51,255) background; presentation was
acknowledged inside requestAnimationFrame. This closes the mocked-native-host
versus real-JS-backend gap for that subset
([result](../evidence/graphics-browser-native-abi.json),
[screenshot](../evidence/graphics-browser-native-abi.jpg)). It is a synthetic
fixture, not original-game rendering. Lighting, shaders, blending, most texture
formats and other required game states remain outside this initial native subset.

The staged 143-handler O1 build was subsequently verified in the original browser
startup: it reaches the same media preflight and actual `ExitProcess(1)` in
33,641.5 ms with the same 38 range requests. This verifies that initializing the
new native COM/D3D9 modules does not prevent that startup path; it does not imply
that original game calls into those modules have occurred
([trace](../evidence/static-browser-d3d9-foundation.log)).

## Real Windows Media profile bridge

`WMVCORE.DLL!WMCreateProfileManager` now provides owned profile state and bounded
UTF16 XML parsing. `IConfigAsfWriter` configures the real service for its supported
WMV2/WMAv2 profile; unsupported WMV3 encoding and unsupported XML features fail
explicitly. Native sanitizer/Wasm tests pass. The browser native-COM fixture
also passed actual create/configure/release through the verified codec worker
([result](../evidence/media-browser-native-abi.json)). Its first attempt timed out
because the pthread worker loaded the harness script instead of the generated
module; setting the explicit Emscripten mainScriptUrlOrBlob fixed that test setup.

The built-in WMVCore module/file metadata is now gated on this real registered
factory, rather than a standalone presence marker. Reader and video graph paths
remain unfinished; registration does not claim those exports exist. The
144-handler O1 build is staged for an original-game startup run.

A separate bounded audio-reader service passed decoding of the original ASF
into 1,900,544 bytes of PCM, a 1.0–1.5 second seek yielding 88,200 bytes, explicit
EOS and malformed/quota/order checks. Browser samples match the local Wasm hash
and differ from native FFmpeg by at most one S16 unit
([result](../evidence/media-browser-reader.json)). Its decoder queue is 8 KiB;
this is not yet connected to original IWMSyncReader calls or an audio device.


## Original media COM preflight succeeds; actual D3D format rejection

The original executable now successfully invokes CoInitialize, creates its
FilterGraph/CaptureGraphBuilder2/WMAsfWriter objects, sets graph membership, and
adds the real service-backed writer. All observed HRESULTs are zero. It proceeds
to the actual D3D9 factory and CreateDevice calls
([targeted trace](../evidence/static-browser-d3d-format.log)).

The device requests are from `009a7bc1` (flags `0x44`) then `009a7bf4` (`0x24`).
Both request 1024×768, format `0x19` (A1R5G5B5), one backbuffer, fullscreen,
auto D16 depth, default presentation interval. Both return D3DERR_NOTAVAILABLE:
the adapter currently accepts only X8R8G8B8 backbuffers. The existing texture
support is a separate capability and cannot make that backbuffer format valid.

The apparent silent exit was also diagnosed: the game's fatal graphics dialog
uses `MB_SETFOREGROUND` (`0x10000`), previously rejected before presentation.
This flag now uses the host modal's actual default-button focus within the private
game UI. The original error is visible and requires acknowledgement
([screenshot](../evidence/static-browser-d3d-format.jpg)). Rejected owner/flag
combinations are logged. The latest run took 54,373.4 ms excluding dialog wait,
with the same 38 range reads; this longer path is not a gameplay performance test.

Worker runtime output is now batched synchronously into approximately 32 KiB
packets, flushing at window/dialog/terminal boundaries. A regression checks 10,000
API trace lines retain order while producing fewer than 10 browser messages.
A previous full-import trace overloaded the UI and was closed at its first-run
dialog; it is not used as performance evidence. Normal runs keep full tracing off.

## Native reader and 16-bit target acceptance

The dedicated native IWMSyncReader browser fixture passed with the actual codec
service: guest IStream callbacks supplied 97,317 compressed bytes from the
lossless audio-only remux, and native COM sample buffers returned 1,900,544 PCM
bytes plus an 88,200-byte seek range. Ownership and EOS checks completed
([result](../evidence/media-browser-native-reader.json)). This fixture is separate
from writer contract mocks and does not claim original-game audio playback.
The reader deliberately rejects multi-stream sources until stream selection and
video outputs are implemented. Reader service lifecycle is connected to startup.

The D3D adapter now also supports the game's logical A1R5G5B5 target: shader output
and clears quantize RGB to 5 bits and alpha to 1 bit before storage/readback/present
through the RGBA8 GPU transport. CreateDevice accepts SWVP|FPU_PRESERVE (`0x24`)
while preserving guest x87 state; mode enumeration reflects actual backend
format support. The actual native-COM-to-GPU browser fixture passed all six pixel
checks, including repeated BGRA texture uploads
([result](../evidence/graphics-browser-native-16bit.json)).
The 145-handler O1 build incorporating these fixes is staged. Original-game
device creation and subsequent rendering still require verification.


## Original graphics device creation succeeds

The 145-handler browser run now creates the original requested 1024×768,
A1R5G5B5, D16 device. Its first hardware-vertex-processing request (`0x44`) is
truthfully rejected; the game's own software fallback (`0x24`) succeeds at
`009a7bf4`. Original capability queries and additional asset reads follow.
The next explicit stop is `00994f5d: D3D9 GetTextureStageState unsupported state`.
[Trace](../evidence/static-browser-device-created.log) and
[screenshot](../evidence/static-browser-device-created.jpg).

This run took 53,558.2 ms excluding dialog interaction and Wasm fetch/compile,
with 39 range requests, 10,223,616 bytes fetched and 24 cache hits. No original game
frame or gameplay input response is established. Native state-query compatibility
is the next observed graphics task. The current validation checkpoint is 62 JS
checks, 31 Python checks (2 skipped), and 17 native module suites; these tests do
not substitute for playable original-game acceptance.


## Sampler and audio component verification

The original startup passed the texture-stage state queries and next stopped at
`00995123`, setting sampler MIPFILTER to POINT. The new adapter supports the
observed point/linear filtering, repeat/mirror/clamp addressing and LOD bias for
single-level textures. Native COM through the actual WebGPU backend passed all
16 pixel readbacks ([result](../evidence/graphics-browser-native-samplers.json)).
This is a synthetic ABI fixture, not an original game frame.

The browser AudioWorklet sink separately passed: 192 buffers / 24,576 frames
consumed, with pause, reset and close ownership checks
([result](../evidence/audio-browser-sink.json)). Native WinMM methods exist but
are not registered in the game launcher; mapper message 0x2015 remains pending.
Thread support is staged under `.cache/thread-stage` and is not game-enabled.

Matching O1 and O2 sampler-enabled artifacts are staged, with identical host
source snapshots recorded in [provenance](static-o1-o2-sampler-profiles.json).
A quiet paired original-startup comparison is pending. Neither this comparison
nor the component fixtures establishes gameplay speed or playability.


## Matched sampler-enabled O1/O2 original runs

Both browser builds passed sampler initialization and stopped at `0099526d`,
SetRenderState(ALPHAFUNC, GREATER), with identical registers, 39 range requests,
10,223,616 fetched bytes and 24 cache hits. O1 took 55,131.4 ms and O2 took
54,149.5 ms: a 1.8% difference in a single sequential pair, insufficient to
establish a repeatable speedup. Compilation/download and dialog wait are excluded.
No original game frame has been produced by the static port yet.
[Comparison](../evidence/static-browser-samplers-comparison.json),
[O1 log](../evidence/static-browser-samplers-o1.log),
[O2 log](../evidence/static-browser-samplers-o2.log).


## Alpha and native audio verification

All eight D3D alpha comparisons now execute in the actual WebGPU fragment shader
before 16-bit output quantization. Native material state roundtrips its 68-byte
structure; active lighting remains explicitly unsupported. The native-COM browser
fixture passed 32 pixel readbacks ([result](../evidence/graphics-browser-native-alpha.json)).
An initial shader validation failure was fixed before game staging.

Native WinMM calls now reach the real AudioWorklet in a separate browser fixture:
64 played callbacks, 8,192 consumed frames, eight reset callbacks, and context
closure passed ([result](../evidence/audio-browser-native.json)). Mapper message
0x2015 now reports the available virtual sink. The fixture drives reset acknowledgement
asynchronously; the production guest-worker bridge remains uninstalled. An EM_JS
pointer-parameter parsing bug was exposed and fixed by this browser test.

Regression checkpoint: 69 JavaScript checks, 31 Python checks (two skipped),
and all 18 native module suites passed. The latest alpha-enabled O1 game artifact
is staged; O2 still represents the previous sampler checkpoint.


## Original alpha/material initialization passes

The alpha-enabled O1 game run passed the prior render-state stop and read
`Data\\Textures\\thumbmask_256x128.dds` through the original archive path. It next
stopped at `00b1c71f` calling IsProcessorFeaturePresent(7), a 3DNow query.
The runtime CPUID already exposes only FPU/TSC/CX8/CMOV; the Windows query has
now been implemented consistently (CX8/TSC true, unadvertised features false).
The game's own branch at B1C735 handles the absent feature. Process ABI tests pass.
[Original trace](../evidence/static-browser-alpha-passed.log).
This run took 53,711.0 ms, with 41 range requests and 10,747,904 fetched bytes;
regression tests ran during part of startup, so it is not a comparison benchmark.


## CPU fallback passes; production audio bridge connected

The original game passed CPU feature selection and next stopped at `009cc666`,
IDirect3DDevice9::SetLight. Both 3DNow and SSE selection remain disabled consistently
with the translated CPU profile. [Trace](../evidence/static-browser-cpu-feature-passed.log).
No original rendered frame yet.

The production audio worker fixture passed actual compiled WinMM calls with
synchronous reset acknowledged by the render worklet: 64 played callbacks,
8,192 consumed frames, eight reset callbacks, reset request/ack 1/1, and closed
context ([result](../evidence/audio-browser-production.json)). The launcher now
prewarms and cleans up the real audio service; bootstrap registers nine WinMM
handlers (155 total with the CPU-feature addition). The standalone runtime patch
adds a default-off post-import hook; bootstrap only requests pumping after
PeekMessageA or Sleep, preserving completed CPU state and LastError.
Pumping immediately after waveOutWrite would precede the original pending-buffer
counter increment at C1B00A, so that boundary is deliberately excluded.

The integrated O1 build and regression suite pass: 71 JavaScript checks,
31 Python checks (two skipped); native startup and runtime-hook native/Wasm
checks also pass. Original game audio playback and child execution remain
unverified. The audio-integrated build awaits staging with the next graphics fix.


## Light initialization and font reads pass

The integrated 155-handler O1 browser run passed light initialization. The
observed light is index 1, type 3 (directional), with direction approximately
(-0.577,-0.577,-0.577). Light storage/query/enable are implemented; active-lit
draws still stop explicitly. No original lit draw or frame is claimed.
The game proceeds through original font reads and next stops at MulDiv from
`009a8815`. [Trace](../evidence/static-browser-light-audio-passed.log).

MulDiv now uses a signed 64-bit product, nearest rounding with halves away
from zero, and -1 for division by zero/32-bit overflow. Native ASan/UBSan
process ABI tests cover signs, rounding, 64-bit products and boundary cases.
The 156-handler build is being prepared for the next browser run.


## Font scaling passes; memory query is next

The 156-handler browser run passed MulDiv and further directional-light setup.
It next stopped at GlobalMemoryStatus from `00aa9e1e`.
[Trace](../evidence/static-browser-font-scaling-passed.log).
It took 54,672.6 ms with 43 range requests and 11,272,192 bytes fetched.
These are startup observations; original rendering, audio playback and gameplay
performance remain unverified. Memory reporting must describe actual guest
capacity/available allocation budget rather than inventing host physical RAM.


## Actual guest memory budget implemented

GlobalMemoryStatus now reads a mutex-protected heap snapshot shared by guest and
backend allocations: arena total, reusable free extents, and unreserved extents.
The browser arena is 256 MiB; no host physical RAM or additional swap is claimed.
The observed caller compares totalPhys against 384/768 MiB thresholds, selecting
the original low-memory tier for this capacity. MEMORYSTATUS layout, VOID ABI,
LastError preservation, bounds, and live allocation changes pass native sanitizer
and Wasm tests. The 157-handler O1 artifact is staged and under browser verification.


## Memory-tier selection passes; events ready

The original game passed GlobalMemoryStatus and initialized private StarMaker
files. Next stop: CreateEventA at `00a56559`.
[Trace](../evidence/static-browser-memory-tier-passed.log). The run fetched
17,825,792 bytes in 68 range requests with 158 cache hits, taking 54,791.5 ms.
The caller creates auto-reset unsignaled, manual-reset unsignaled and manual-reset
signaled events, then calls _beginthreadex with user start A55E90 at A56590.

CreateEventA/SetEvent/ResetEvent are now implemented with a typed mutex/event
namespace and real wait/reset/close semantics. Native sanitizer and pthread Wasm
tests pass. Nonpthread builds allow signaled/zero-timeout polls but explicitly
reject unsignaled blocking waits; no fake thread or successful wait is returned.
The 160-handler O1 build now passes original event initialization in the browser
and stops at CreateThread called from `00acfa6f` in the original CRT wrapper.
[Trace](../evidence/static-browser-events-passed.log) and
[capture](../evidence/static-browser-events-passed.jpg) record Wasm SHA-256
`df908d3de42b57780f853be0e3e5e30d23e204fd691ccc85772b622ea212a13e`.
This run took 56,695.0 ms, fetched 17,825,792 bytes in 68 range requests and had
158 cache hits. The observed user start A55E90 supersedes the earlier expected
543348 startup-thread path as the immediate original-thread integration target.
This capture is nonthreaded: it does not prove that original child code runs.



## Page-owned pthread transport and fatal cleanup verified separately

The original parent-only teardown assumption failed a real browser test: after
terminating a blocked parent, its nested child's heartbeat advanced from 20 to
45. The corrected threaded launcher puts ownership of every real pthread Worker
on the page. A MessagePort-backed Worker facade forwards Emscripten pool messages,
compiled modules, shared memory and transferables to the logical main worker.
Page Stop/fatal cleanup terminates the owned pool directly. The shared native
failure flag is published after the bounded UTF-8 diagnostic; the page watcher
reacts to that flag, never to elapsed time alone.

The repaired [browser lifecycle result](../evidence/thread-browser-owned-stop.json)
and [capture](../evidence/thread-browser-owned-stop.jpg) prove detection while the
logical parent is blocked in Atomics.wait and child termination: heartbeat stays
19 → 19 for the observation interval. This is a synthetic child failure using
the production watcher/transport, not an original-game crash or gameplay result.

The [browser filesystem trace](../evidence/thread-browser-owned-filesystem.log)
and [capture](../evidence/thread-browser-owned-filesystem.jpg) prove that actual
Emscripten pthreads still read the unchanged original archive filesystem through
this transport. All three cases read the same original 740 bytes with FNV-1a
129214825 and one 262144-byte range request. A busy main worker initially stalls
the child; explicit system-queue pumping recovers it (360.87 ms including the
intentional 350 ms stall). Immediate pumping completes in 8.695 ms; a timed
condition wait with no explicit pump completes in 48.63 ms. These fixture timings
are not game startup or gameplay-performance measurements.

The production file-table review found existing locking around handles, offsets,
quota/COW, searches and cwd; system-queue processing must remain separate from
guest callback dispatch. Child graphics/media/window providers are not inherited
and remain outside this proof. No threaded original-game run has yet been
verified. Reproduction and limits are in
[thread filesystem probe](../../scripts/thread-fs-probe/README.md) and
[thread failure probe](../../scripts/thread-failure-probe/README.md).

## DirectInput cooperative-level fix (November 8)

A user-driven Chrome run logged `SetCooperativeLevel kind=1/2 hwnd=00000000 flags=5 result=80070057`:
the original configures DirectInput at `00a95fa0` using the window global `0x105beb0` before the
D3D9 setup at `009a7ae1` stores it, ignores the failure, and then calls `Acquire` every update.
Real DirectInput (and Wine) keep the default `DISCL_NONEXCLUSIVE|DISCL_BACKGROUND` level in that
case, so `Acquire` succeeds. `native/directinput-api.c` now models that default (hwnd 0 = background,
no window-focus requirement); the page still emits events only while the game canvas is focused.
The page host also re-derives focus once per second (`MoviesDirectInputHost.sync`).
Verification of menu interaction pending a focused-browser run.

## Menu, New Game and the lit-geometry stop (November 8)

Verified in a user-driven Chrome run (O1 + guest threads): main menu after ~2,600 frames, mouse
hover/click work, **Game → New Game** reaches the studio-lot loading screen ("Lot Management" tip,
progress bar advancing) at ~17 fps in menus and well under 1 fps while loading.

Input fixes in this round:
- Full input queues drop motion instead of ending the run (`MoviesWindowInput.offer` with a quarter
  of the ring reserved for buttons/keys; same for the DirectInput host). A busy game previously
  ended the run with "Game input queue overflowed."
- Releasing pointer lock no longer moves keyboard focus to the page's Capture-mouse button; that
  left the game without focus (`GetForegroundWindow` 0), so it ignored all input.

The load then stopped at `00a51610`: `SetFVF 0x112` (`XYZ|NORMAL|TEX1`, 32-byte vertices), i.e. the
3D lot's lit geometry. `native/d3d9-api.c` now accepts that format and evaluates D3D9 fixed-function
lighting on the CPU per draw (directional/point/spot lights, attenuation, global + light ambient,
material emissive/ambient/diffuse; no vertex colours exist in this format, so every material source
resolves to the material). The result is uploaded as an `XYZ|DIFFUSE|TEX1` snapshot, cached by a hash
of world matrix, material, enabled lights and lighting states. Not modelled: specular (logged once),
`D3DCULL_CW/CCW` (drawn as CULL_NONE; depth testing hides back faces of closed meshes). Lighting is
evaluated in world space, which equals D3D9's camera-space result for an orthonormal view.
Moving lighting to WGSL is the obvious performance follow-up if per-draw re-lighting shows up in
profiles.

d3d9-webgl (MIT, ~3.5k lines, one game) was assessed and not adopted: it would mean dropping the
WebGPU worker for main-thread WebGL, still needs our guest-pointer trampolines, and its lighting
(type ignored, ≤3 point lights) and Lock/state-block semantics are weaker than what this needs.
Its pixel-format swizzles and fog formula remain useful references.

### Menu cursor freezing after leaving the page (November 8)

The original keeps an "own window has focus" byte at `[00e52c9c]`: WndProc `00542380` sets it on
`WM_SETFOCUS` and clears it on `WM_KILLFOCUS` (both via `00553f50`), and the per-frame `005550b0`
clears it whenever `GetForegroundWindow()` is not the game window. While clear, the game ignores
mouse input. The page never sent focus messages, so switching away from the browser during the long
startup cleared the flag permanently: in the main menu the float gate `[0104ccdc]` is 0 (measured),
so `005550b0`'s own re-acquire path (cursor inside the window → set flag, recentre with
`SetCursorPos`) never runs. Runs where the user stayed in the browser worked; runs where they
switched away did not.

`public/static-probe.js` now posts `WM_KILLFOCUS`/`WM_SETFOCUS` on every page-level focus transition
of the game canvas (window focus/blur, visibility, focusin/focusout, plus the 1 s resync), and
`native/window-api.c` accepts messages 7/8 from the host queue (`lParam` 0, `wParam` 0 or a live hwnd).
A temporary diagnostic line `[dinput] mouse gate ...` reports the gate, the flag, the cursor point and
the foreground window every 1,200 mouse polls.
Follow-up the same day: posting `WM_KILLFOCUS` made the original minimize its fullscreen window
(`00542580 -> 009a5d60`, `ShowWindow(SW_SHOWMINNOACTIVE)`) and loop on `TestCooperativeLevel` +
`Sleep(200)` until `D3DERR_DEVICENOTRESET`, i.e. the fullscreen device-lost/Reset protocol. Our device
is never lost and `Reset` (vtable slot 16) is not modelled, so the run hung. The page now posts only
`WM_SETFOCUS` (on every gain); losing focus is left to `005550b0`'s own foreground check. Modelling
device loss + Reset would be needed only if a pause-on-Alt-Tab behaviour is wanted.

## Load-time profiling and the allocator fix (November 8)

`scripts/bench/startup-bench.mjs` drives a separate headless Chrome over the DevTools protocol:
trusted clicks for Run/OK, synthetic relative mouse motion for the menus (the game scales DirectInput
deltas by ~1.6 and clamps at the corner), screenshots, and per-worker CPU profiles. The staged module
is linked with `--profiling-funcs`, so profiles show `f_<guest address>` and host function names.

First profile (O1 + guest threads, startup): the main game thread spent **50.5%** in `allocate`
(`native/heap-api.c`), which walked the address-ordered list of all blocks (live and free) for every
`HeapAlloc`. Free extents are now indexed per pool in an address-keyed treap augmented with subtree
maximum capacity, giving the same lowest-address first-fit choice in O(log n); the old scan remains
only as a fallback for allocations that must span adjacent extents of different pools. All heap tests
(including exact first-fit address checks) pass.

| Headless Chrome, O1 + threads | before | after |
|---|---|---|
| Run → first frame | 62 s | 8 s |
| Run → main menu | ~5 min (user runs) | 83–87 s |
| New Game → lot code | ~10 min (user run) | ~60 s |

Also: the D3D9 host can defer reply-less commands (clear/draw/release/uploads/render-target switches)
into one `batch` per synchronous point, predicting worker-assigned handles and verifying them on the
reply (`deferred:true` in the static probe; batch limit raised to 512). This did not change load time
measurably, which is CPU-bound in game code (`f_00b25079`, `f_00b24af9`, `f_00ac8360`), and its
effect on frame rate is not yet measured.

The lot now stops at `00991730`: `SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE)`.

## Studio lot rendering, input and persistence (November 8)

Headless-Chrome runs (`scripts/bench/startup-bench.mjs --until lot --lot-clicks ...`) now reach the
studio lot ~87 s after New Game, render it with lighting, lightmaps and shadows, and advance the
tutorial on clicks (~15-20 presented fps). Graphics work in this round, all in the C D3D9 layer plus
the WebGPU backend:

- **General texture-stage cascade** for stages 0-1 in WGSL (D3DTOP 1..26, D3DTA selectors and
  COMPLEMENT/ALPHAREPLICATE; NULL-texture rules from the D3DTSS docs). Not modelled: arg0 ops,
  bump mapping, RESULTARG TEMP, stages >= 2 (logged once as "texture-stage approximation").
- **Second texture + second UV set** (lightmaps): converted vertices use `XYZ|DIFFUSE|TEX2` /
  `XYZRHW|DIFFUSE|TEX2` GPU layouts; stage 1 gets its own coordinate index, texgen and matrix.
- **Any non-skinned FVF** (normal, point size, diffuse, specular, 0..8 sets of 1..4 floats) is
  converted on upload; fixed-function lighting honours `*MATERIALSOURCE` with vertex colours.
- **Texture coordinate processing**: `D3DTS_TEXTURE0..7`, `TEXTURETRANSFORMFLAGS` COUNTn/PROJECTED
  (per-vertex divide), camera-space normal/position/reflection/sphere-map texgen.
- **Topologies**: strips/fans become lists; `DrawPrimitive`, `DrawPrimitiveUP` and general
  `DrawIndexedPrimitiveUP`; point/line primitives are skipped (logged once).
- **Texture formats**: R5G6B5, X8R8G8B8, R8G8B8, X1R5G5B5, A1R5G5B5, A8R3G3B2, A8, L8, A8L8, DXT1-5
  decoded to RGBA8 and reported by `CheckDeviceFormat`. The black lot ground was caused by this:
  with only four formats advertised, the game's lot-lighting bake fell back to an all-zero
  A4R4G4B4 map that four DESTCOLOR*SRCCOLOR ground decals multiplied onto the terrain; with L8
  available the bake produces real lighting.
- Culling modes CW/CCW are drawn as CULL_NONE (depth testing hides back faces).

Input/focus: the page posts only `WM_SETFOCUS` (the original's flag at `[00e52c9c]`);
`WM_KILLFOCUS` would trigger its fullscreen device-lost/minimize loop. Presentation no longer kills
the game when the page stops acknowledging frames (sleeping display, throttled tab): after 1 s the
frame is dropped and logged.

Persistence: `/userdata` (game-overlay copies of game files, shell folders, `registry.v1`) is
mirrored write-through from the game worker's MEMFS to OPFS (`public/userdata-store.js`) and
restored before the next run. The in-memory registry is serialised to `registry.v1` after every
mutation. At startup `Game running:` is reset to empty (the original's clean-exit value), because a
browser session ends by closing the tab; otherwise every start warned "not closed properly".
`ShellExecuteA` (e.g. "view the Readme") is logged and reported as successful.

Diagnostics kept (off by default): `?traceAt=<frame>` records one frame of D3D bridge commands
(`d3d9-host.js`), and `globalThis.moviesDrawLimit` / `moviesSkipDraws` in the graphics worker limit
or skip draws for bisecting rendering bugs.

Known gaps toward a good play experience: the main-menu background movie (DirectShow video:
`AddFilter` returns E_NOINTERFACE, so the menu background is black); frame rate (~15-20 fps; nearly
every draw re-uploads its vertex range because each buffer caches only its last uploaded span);
specular lighting; point/line primitives.

### Lot frame-time work (November 8)

Profiles of the studio lot (main game thread, 25 s windows):

| Change | Waiting on graphics worker | Note |
|---|---|---|
| baseline | 45% (`#exchange`) | Present blocked twice per frame on `onSubmittedWorkDone` + `setTimeout(0)` |
| non-blocking per-frame health check in `present` | 34% | GPU errors still surface via `uncapturederror` / device loss |
| pipelined batch flush (`client.post`, verified at next exchange) | ~20% | worker executes batch N while the guest builds N+1 |

Lot load also dropped from ~87 s to ~79 s. Each vertex/index buffer now keeps a 4-entry upload
cache (`UploadEntry`), but the engine streams most geometry through 4 MB dynamic buffers rewritten
every frame, so most draws still upload fresh data. The largest remaining cost is
`convert_vertices` (~20%: CPU fixed-function lighting/texgen + layout conversion; host layer is now
compiled at -O3). The structural fix is GPU-side vertex processing: upload guest vertex data as-is
with WebGPU attributes at the FVF offsets/stride and evaluate lighting and texgen in WGSL.

## User-reported issues and coverage audit (November 8, later)

- Right-click stop: `SetRenderState(D3DRS_DEPTHBIAS)`. Render state is now stored for every
  documented state (invalid enums return INVALIDCALL; unknown states still stop). Drawn: all eight
  depth comparisons, depth bias/slope bias, colour write mask, blend operations 1..5, separate
  alpha blending, BOTHSRCALPHA/BOTHINVSRCALPHA, BLENDFACTOR constants. Logged once, not drawn: fog,
  stencil, wireframe/point fill, flat shading, user clip planes (SetClipPlane is stored).
- New Game `Clear` stop: any TARGET/ZBUFFER/STENCIL combination and rectangle lists are supported
  (stencil is a no-op: there is no stencil buffer); depth-only clears preserve colour.
- A coverage audit (imports, COM, data files; report kept at /tmp/movies-coverage-audit.md during
  the session) found 172 unimplemented imports, mostly developer/crash/online paths. Fixed from it:
  `IsBadReadPtr` (C++ typeid in save serialisers), `GetFileTime` (save list), `GetDoubleClickTime`,
  `PlaySoundA` (logged), `SetClipPlane`/`GetClipPlane`, 1 GB user-data quota, keyboard text entry
  (page posts WM_KEYDOWN/WM_KEYUP with VK + scan code; `ToUnicode`/`ToAscii` for the US layout).
  Verified headless: Escape opens the pause menu, Save Game accepts a typed name, and the `.jad`
  save is written and persisted to OPFS.
- Presentation robustness: a superseded frame is dropped instead of failing the run, and a page
  without animation frames presents after the timeout instead of erroring.
- Sound (in progress): the game's own driver creates DirectSound through COM (`0xc10240`:
  Initialize, SetCooperativeLevel(PRIORITY), GetSpeakerConfig); a minimal IDirectSound object now
  satisfies that. The 3D provider is a statically linked OpenAL router (device classes at
  `0xc144d0`/`0xc14bc0`, loader `0xc13750`: `GetCurrentDirectoryA` + `/OpenAL32.dll`, then
  `/wrap_oal.dll`) using 31 `al*`/`alc*` exports plus EAXGet/EAXSet. No sound buffers or OpenAL
  loads have been observed yet; the remaining work is an OpenAL (and possibly DirectSound buffer)
  implementation with a software mixer feeding the AudioWorklet sink, plus MP2-over-ACM for the
  1,703 speech clips.
- Not yet started: menu background video (`Data/intro/frontend_loop.wmv`, WMV3/WMA2).

## Sound, import coverage and a runtime SIMD fix (2026-11-08)

- **Import coverage:** all 330 imports of MoviesSE.exe now have host handlers, except 29 WS2_32 ordinals (online play) and the waveIn/mixer entries (OpenAL/microphone paths, unreachable).
  - New: Virtual*/Global*/Local*/CoTaskMem* (heap-api); file move/copy/truncate/attributes (file-api); ANSI locale, compare, wsprintf and date/time formatting (text-api); GetVersionExW and IsDebuggerPresent (startup-api).
  - Added by the helper agent: window/GDI, resources, process/console, file mappings, DuplicateHandle, timeSetEvent and OLEAUT32.
  - Also new: ACM MPEG Layer II streams (acm-api + mp2-decode, 75 dB SNR against ffmpeg) for user WAVs.
- **Audio path** (map in /tmp/movies-audio-map.md, summarized):
  - The Lionhead engine creates its mixer only when its CPU-detection byte `[0x10d5e12]` (MMX) is set. We set it when DirectSound is created; SSE/SSE2 stay off, so the MMX mixer is used.
  - `dsound.dll!DllGetClassObject` provides an IKsPropertySet whose calls return E_NOTIMPL, so the hardware-acceleration probe selects plain DirectSound.
  - The engine streams one looping 17,920-byte secondary buffer (44.1 kHz stereo) and polls the play cursor.
  - native/dsound-api.c keeps that ring in guest memory. The AudioWorklet reads it straight from shared Wasm memory and publishes the cursor (DMA model). If there is no renderer heartbeat, the cursor follows the wall clock.
  - Audio engine threads needed a larger guest-thread pool (2 → 8). Alertable waits are now treated as plain waits (no APC sources are imported).
- **Runtime bug fixed (patches/retro-port-simd-sign.patch):**
  - In simd.h, `v_get` for dword lanes returned `sgn ? (int32_t)v : v`, which C promotes to uint32_t, so 32-bit signed reads were never sign-extended.
  - Every `psrad`/`pcmpgtd`/`packssdw` on dword lanes was wrong, and the audio mixer produced white noise.
  - Verified with a native harness running the translated mixer block (`-1000 → -500` after the fix). This forces a full rebuild.

## Lock-free guest memory and movie playback (2026-11-09)

- **Guest memory without a global lock** (patches/retro-port-lockfree-access.patch, applied after the thread-safety patch):
  - The thread-safety patch made every R*/W*/memcpy on guest memory take one global spinlock plus a TLS depth counter.
  - With the audio engine's threads running, that lock bounced between cores on every access. Lot loading went from 156 s to 599 s.
  - Plain accesses are now ordinary Wasm loads and stores. Aligned ones are single-copy atomic, like x86.
  - The only reachable translated atomics are 36 `xchg mem, reg` groups (lock xadd/cmpxchg are translator traps). build-bootstrap.py rewrites them to `guest_xchg8/32` (real `__atomic_exchange_n`) in a private copy of the translated sources (`translated-lockfree/`, hashes recorded as `rewrittenTranslationSha256`).
  - Interlocked* imports already used host atomics.
  - Result with audio running: main menu 43 s (was 76–99 s), lot 73 s (was 156–599 s), lot ~20 fps headless, menu 31–34 fps.
- **Movies** (native/movie-api.c, map in /tmp/movies-video-map.md):
  - Every .wmv (logo, frontend intro and loop, credits, tutorials) goes through one `VideoTexture`/`MoviePlayer`. It builds FilterGraph + the game's own CBaseVideoRenderer ("Texture Renderer", RGB24 only) + CLSID_WMAsfReader, then polls `IMediaPosition::get_CurrentPosition`.
  - The ASF reader is virtual. `IFileSourceFilter::Load` sends the original file to the page's FFmpeg reader service, which uses the shipped core's wmv3 decoder (no transcoding). Each request returns one bottom-up BGR24 frame (`-vf vflip -pix_fmt bgr24`); the request payload is now 3 MB.
  - `Render(videoPin)` calls the guest pin's `ReceiveConnection` with an RGB24 VIDEOINFOHEADER, then `NotifyAllocator` with our allocator.
  - `get_CurrentPosition` drives a wall clock and pushes the newest due frame through the guest `IMemInputPin::Receive`. Movie audio (the WMAv2 reader) plays on a host-owned DirectSound streaming voice with `IBasicAudio` volume.
  - Verified headless: the logo plays all 323 frames; the frontend intro, then `frontend_loop.wmv` play and loop (end, then Stop, put_CurrentPosition(0), Run).
  - tests/test_movie_api.c checks the stdcall pop count of every exercised COM slot. A `this`-counting mistake corrupted the game's stack in the first build.

## Dropping people into rooms (translator entry-order fix)

Symptom: in a new game a hopeful could be picked up and dragged over the Stage
School or Staff Office, but clicking never dropped them (no hire).

Cause: the translator emits a function's instructions in address order. Seven
functions have a Ghidra tail-jump chunk *below* their entry, so the generated C
function started executing that chunk instead of the entry. One of them,
`009dd0d0`, computes a model's bounding sphere at load; it skipped the per-mesh
pass, leaving every model's bounds at zero. The pick proxies of the buildings
(`fac_stageschool`, `fac_gatehouse`, `fac_trailer2`, flagged pickable while a
person is held) then failed the frustum test, never became pick candidates, and
the drop validation (`0053a560` → `0052d7c0`) saw no building under the cursor.

Fix: `fix_entry_order()` in `scripts/build-bootstrap.py` inserts `goto L_<entry>`
in exactly those seven functions (00510440, 0058f440, 005b3d10, 0070f640,
008600e0, 0087feb0, 009dd0d0), placing the label on the one unlabelled block that
follows a terminator. Fixed copies go to `translated-fixed/` and are recorded in
`rewrittenTranslationSha256`. Verified headlessly: pick up, hover Create Actor
(tooltip appears), click → "Congratulations, you've just hired your first Star!".

Game behaviour to know: after the hold that picks someone up, the first release
only ends the pickup; the next left click drops them.

## Draw batching and the 16-bit resolve (particle frame-rate drops)

The game's backbuffer is logical A1R5G5B5. The GPU backend used to run a
full-screen RGB5/A1 resolve pass after *every* draw so the next blend read
quantized destination colour: ~345 full-screen passes per lot frame, plus one per
particle batch (sparkles and smoke are blended draws), with a separate command
encoder, render pass and submit per draw.

`public/d3d9-gpu.js` now:
- records consecutive draws to the same colour/depth attachments into one render
  pass and one submit (per-draw uniforms live in a 1024-slot ring buffer);
- resolves the 16-bit target only before it is observed (present, readback,
  copy, target switch, clear) or before a draw whose blend reads destination alpha.
  Non-blended draws still quantize in the shader; blended ones accumulate in RGBA8;
- recycles vertex/index buffers by size class after their last use is submitted;
- adopts the worker's transferred upload bytes instead of copying them, and checks
  vertices with a Float32Array loop. The client no longer re-copies and
  re-validates batches built by the D3D9 host.

Headless lot (same tutorial frame), presented fps: 13.7 → 25.4; while carrying a
person: 11.8 → 25.5. Per frame: 345 draws in 5 passes with 5 resolves (was 345/345).
Remaining game-thread hot spot: CPU vertex lighting/conversion (`convert_vertices`, ~25%).

## Backbuffer LockRect (D3DX lock helper B0E42C)

After about 8.5 minutes of play a run stopped at `00b0e839: D3D9 back/depth surface LockRect unsupported`. That address is inside the statically linked D3DX helper B0E42C. The helper locks a source surface directly and falls back to copies only if the lock fails. The game reaches it through 0099aa95, 0099bcc5, 009a57b6 and 009a5c3a. The source was the implicit backbuffer, which had no implementation; earlier work had covered locks of textures and lockable render targets only. The backbuffer lock now reads the last rendered image back (`readPixels`, bridge command 16). The image is returned in the backbuffer's own format: 22 is X8R8G8B8 and 25 is A1R5G5B5. Guest writes are not uploaded on unlock, and a non-READONLY unlock is logged. Locking the D16 depth surface returns INVALIDCALL, matching Windows. The fix is covered by `tests/test_d3d9_api.c`; the in-game trigger has not been reproduced.

## No recording device; startup logos skipped

- **Recording (WINMM `waveIn*`) and the mixer (`mixer*`):** the game probes `waveInGetDevCapsA(WAVE_MAPPER)` at A347A5. The browser build now reports no capture device and no mixer, as on a PC without a microphone.
  - Device-ID calls return MMSYSERR_BADDEVICEID.
  - Handle calls return MMSYSERR_INVALHANDLE.
  - `mixerGetNumDevs` returns 0.
  - Voice-over recording is therefore unavailable.
- **Startup logos:** `scripts/build-bootstrap.py` `skip_intro` patches the translated sequence update 0045E3F0.
  - The sequence covers the Lionhead logo, the Activision video, the Intel and ATI splashes, and the legal screen.
  - The patch finishes the sequence from state 0, before anything has been loaded.
  - In a headless run the menu appears about 28 s after start (about 75 s with the logos).
  - The main menu's own intro (frontend_intro.wmv) still plays.
