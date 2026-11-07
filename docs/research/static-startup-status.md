# Static original-code startup: verified progress

The original `MoviesSE.exe` entry point (`0x00ad2451`) has executed as statically
translated native code, as WebAssembly in Node, and as WebAssembly inside the
in-app Chromium browser. There is no Wine or Boxedwine in these executions.
No original game bytes were modified. The executable SHA-256 remains
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.

## Latest threaded browser checkpoint (branch `static-port/playable`)

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
