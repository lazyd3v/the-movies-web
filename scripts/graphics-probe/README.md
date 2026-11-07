# Graphics foundation diagnostic

Run `python3 scripts/graphics-probe/stage.py`, start the existing local server,
and open `/runtime/graphics-probe/`. It requires a secure context, cross-origin
isolation, worker WebGPU and OffscreenCanvas. The generated files are ignored.
This synthetic diagnostic is not a game frame and does not register d3d9.dll.

`public/d3d9-gpu.js` exports `MoviesD3D9GPU` as an ES module. No upstream code
was copied. Contracts follow the [WebGPU specification](https://gpuweb.github.io/gpuweb/)
and [WGSL matrix conventions](https://gpuweb.github.io/gpuweb/wgsl/#matrix-types).

## Native bridge boundary

* `await MoviesD3D9GPU.create({canvas,width,height})`: adapter/device/pipeline
  creation and actual depth16 probe. Failure throws; no headless fallback.
  Returned `capabilities` describes this backend subset and real queried limits,
  not a ready-made D3DCAPS9 structure or an impersonated GPU.
* `uploadVertices(bytes, 'XYZ_DIFFUSE'|'XYZ_DIFFUSE_UV')`: returns an opaque
  handle. Layout is little-endian floatXYZ, uint32 ARGB diffuse, optionally
  floatUV; stride16/24. Copies input bytes before upload.
* `uploadIndices(bytes, 'uint16'|'uint32')`, `uploadTextureRGBA(w,h,bytes)`:
  owned handles. Texture has one mip, nearest filtering, clamp-to-edge only.
* `clear({color:unsignedD3DCOLOR,depth:0..1})`: real GPU color/depth clear;
  first draw must follow a clear, avoiding implicit attachment initialization.
* `drawIndexed({vertices,indices,firstIndex,indexCount,baseVertex,world,view,
  projection,texture,depthTest,viewport})`: triangle-list only. Signed base
  vertex and every referenced vertex are checked. Matrices are16 finite D3D
  row-major floats; raw storage produces transposed WGSL column matrices, so
  shader `P*V*W*position` matches the game's row-vector `position*W*V*P`.
  Depth test is disabled or D16 write+less-equal. No lighting/blend/alpha-test,
  stencil, fog, programmable shaders or additional hidden state. Unsupported
  draw fields throw. A UV format requires a texture; the other format forbids it.
* `await readPixels()`: actual GPU copy+map returns tightly packed, top-row-first
  RGBA8. Row padding and BGRA canvas formats are handled. It needs its worker
  event loop to progress. A synchronous guest readback must wait on a separate
  graphics worker, or explicitly suspend guest execution; blocking this same
  worker while awaiting mapAsync deadlocks.
* `present(callback)`: submits a persistent target→canvas copy and synchronously
  obtains an ImageBitmap. Calls `callback(bitmap,serial)` and returns its result.
  Callback owns/consumes/closes the bitmap on success; synchronous callback
  failure closes it here. Use callback to transfer bitmap and wait for a SAB
  acknowledgement from the page; no return message handler on the blocked
  producer is needed. Callback must enforce one outstanding frame. An async
  callback must manage failure cleanup itself and be awaited by its caller.
* `await validate()`: waits for GPU submission and surfaces device/uncaptured
  validation errors. Async GPU failures poison subsequent calls. Device loss
  requires a new backend; reset/resize is explicitly unsupported.
* `release(handle)` destroys an owned upload; `dispose()` destroys all objects.

Resource creation and draws are synchronous submissions, not proof of completed
GPU work. Diagnostic readbacks and validation supply that proof. Initialization,
readback and pipeline creation are async; this API deliberately does not conceal
that requirement behind synchronous stub success.

## Validation

`node --test tests/d3d9-gpu.test.mjs` checks CPU byte/format/range validation and
absence-of-GPU failure behavior, not rendering. The browser page tests indexed
color draw, 16/32-bit indices, negative base vertex, real W/V/P matrix effects,
depth occlusion when D16 exists, UV texture sampling and GPU readback. It then
transfers an ImageBitmap, validates the page canvas center pixel and acknowledges
through SAB. `graphicsProbeResult` and the visible JSON contain actual results.

## Observed browser result

On2026-11-07 the parent agent ran the staged page in the real in-app browser and
reported PASS on an Apple `metal-3` nonfallback WebGPU adapter. The actual
190×128 target used `bgra8unorm`; D16 creation/occlusion, padded RGBA readback,
XYZ diffuse draw, nonidentity world/view/projection, UV texture sampling, and
ImageBitmap→page canvas pixel validation with SAB acknowledgement all passed.
This is hardware graphics plumbing evidence, not an original-game frame.
A refreshed browser run also passed uint32 indices with negative BaseVertexIndex.
Evidence: `docs/evidence/graphics-browser-foundation.json` and its screenshot.
These observations predate the dedicated-worker transport update below; its
subsequent browser verification also passed, as recorded at the final checkpoint.


## Synchronous guest-worker client

`public/graphics-client.js` exports `MoviesGraphicsClient`. Construct it inside
our guest worker with `{width,height,presentationPort,presentationAck}`. The page
creates a MessageChannel and8-byte SharedArrayBuffer, transfers one port to the
guest worker, and listens on the other. The client transfers that port onward
to its dedicated GPU worker. The page receives `{type:'graphics-frame',bitmap,
serial}` directly from the GPU worker. After consuming/closing the bitmap, the
page writes success1 (failure0) to ack[1], then serial to ack[0], and notifies
ack[0]. The guest worker does not handle presentation messages while blocked.

The client has synchronous `uploadVertices`, `uploadIndices`,
`uploadTextureRGBA`, `clear`, `drawIndexed`, `readPixels`, `present`, `validate`,
`release`, `batch` and `dispose` methods. Their arguments mirror the backend,
except opaque resources are positive integer handles and `present()` waits for
the explicit page acknowledgement. `readPixels()` returns an owned RGBA copy,
not a promise or a view into memory overwritten by the next command. Creation
also waits synchronously for actual async GPU initialization in the other worker.

`batch([{command,args},...])` supports clear/draw/upload/release commands;
results retain command order. Uploaded bytes are copied and transferred as
owned ArrayBuffers. A later batch can reference returned upload handles.
Entire batch syntax is validated before execution; execution failures may occur
after earlier commands changed state and explicitly report the failed index.
No rollback or atomic transaction is implied. Unknown commands/features fail.

Bounds:32 commands/batch,32MiB transferred upload bytes/batch,8192 live resources,
256MiB resource-byte budget,4096 target dimensions,64MiB maximum response payload.
The constructor defaults to15s wait timeout (configurable1..60000ms) and at least
1MiB response storage, expanded for the target's RGBA size. Responses carry an
exact serial, status, kind and validated payload dimensions. Any remote error,
malformed response, or timeout permanently closes/terminates that client. Local
argument errors are rejected before dispatch. `close()` unconditionally
terminates the GPU worker; `dispose()` first requests resource destruction.

The shared response is released only after payload/header writes, through an
atomic status store+notify. No Promise/event-handler on the guest worker is
needed to complete a call. The GPU worker alone awaits initialization, validation
and mapAsync. Presentation can wait for the page SAB because no callback on the
GPU worker is needed for that acknowledgement. All waits have finite deadlines.

`tests/graphics-transport.test.mjs` runs a real Node worker responder to test this
transport, explicitly without emulating or claiming GPU drawing. The staged
browser diagnostic runs the same pixel tests through the real GPU worker and
reports its transport description in `graphicsProbeResult`.

### Browser startup correction

The initial browser transport probe timed out during initialization because it
created a nested module worker and immediately blocked its owner. That design
worked in the Node transport test but did not establish browser startup safety.
The current diagnostic uses `await prepareGraphicsTransport({width,height})`
from `public/graphics-host.js` on the page first. It waits for explicit GPU
module-ready and command-port-connected messages. Only then does the page
transfer `transport.commandPort` to the guest and share `transport.responseBuffer`.
Pass both as `MoviesGraphicsClient` constructor options. Browser callers without
that prepared port now fail immediately instead of risking the same timeout.

The page owns the GPU Worker and retains a live error handler. Script/runtime
errors publish a diagnostic response and transport-failure flag into the shared
buffer, waking the blocked guest. The page must call `transport.close()` when
the guest finishes/fails; closing only the guest's MessagePort does not terminate
a page-owned worker. Module startup, initialization and RPC deadlines remain15s;
the correction does not increase timeouts. A Node MessagePort transport test
covers the revised routing; the subsequent actual browser retest passed.

## Native ABI fixture and verified checkpoint

Run `python3 scripts/graphics-probe/build-native-abi.py` and open
`/runtime/graphics-probe/native-abi.html`. This compiles the actual
`native/d3d9-api.c` and heap allocator with a synthetic C caller, then exercises
registered guest COM vtable addresses and stdcall stack cleanup through the
real `d3d9-host.js` adapter, separate GPU worker and RAF presenter. It is not an
original executable or game frame. The synthetic HWND corresponds to the test's
actual190×128 page canvas; game integration must use its real window registry.

The browser passed this fixture on2026-11-07, including actual C CreateDevice,
vertex/index Lock/Unlock, indexed draw, clear, GPU readback, Present acknowledgement,
resource release and device destruction. Evidence is saved at
`docs/evidence/graphics-browser-native-abi.json` and `.jpg`. The page-owned GPU
transport and subsequent RAF version also passed; the earlier timeout is
resolved without increasing deadlines.

Native scope now also includes XYZ_DIFFUSE_UV FVF0x142 and the observed video
texture path: one managed A8R8G8B8 level, tight BGRA guest pitch, checked full/subrect
LockRect/UnlockRect, NOSYSLOCK/READONLY flags, RGBA upload, level surface identity,
and stage0 point/clamp sampling. SELECTARG1(texture) and texture×diffuse modulation
are implemented independently for color/alpha. Native and linked Wasm contract
tests pass. The expanded browser fixture also passed both BGRA texture
replacement/readback checks against the real Apple Metal WebGPU adapter; see
`docs/evidence/graphics-browser-native-texture.json` and its screenshot.
All six pixel checks passed; this is synthetic ABI coverage, not game rendering.

Lighting, shaders, blend/alpha test, RHW, device reset/resize, other texture
formats/mips/pools and alternate render targets remain unsupported. Sampler point/linear filtering and wrap/mirror/clamp addressing now map to real
GPU samplers; anisotropy, border addressing and mip-chain resources remain unsupported. Available texture
memory is a bounded logical resource budget, not a physical VRAM measurement.


Append `?format=25` to the native fixture URL to exercise logical A1R5G5B5
presentation (default22). The real GPU rounds fragment/clear RGB to5 bits and alpha
to1 bit before8-bit storage; readback and presentation use that same target. This
is not a packed CPU-visible backbuffer. Shader blending, dithering and backbuffer
LockRect remain unsupported. The fixture verifies quantized clear and two texture
replacements, while the existing saturated green Present stays unchanged.

The16-bit fixture passed all6 readbacks in the actual browser; evidence is saved
in `docs/evidence/graphics-browser-native-16bit.json` and `.jpg`.


The sampler extension raises the native fixture to16 readbacks. It exercises
actual C sampler state writes for linear versus nearest filtering and wrap,
mirror, clamp UV addressing, with the original initializer's mipPOINT and bias-1.
Single-level LOD remains0. Native sanitizer and linked Wasm contracts plus16 JS
validation/transport/host tests pass; the actual browser passed all16 checks. Evidence:
`docs/evidence/graphics-browser-native-samplers.json` and `.jpg`.


The alpha-test extension raises this fixture to32 readbacks, covering all8 actual
GPU comparison branches. Native and linked Wasm tests pass; the actual browser
passed all32 checks. Alpha discard precedes16-bit quantization, so accepted pixels
with alpha127 legitimately have stored alpha0 in A1R5G5B5. Shader creation now
reports getCompilationInfo line/column errors rather than only an invalid module.

The audited intro blend fixture extends the native ABI page to95 pixel checks
and a second actual bitmap presentation check. Run both `native-abi.html` and
`native-abi.html?format=25`. The declared format25 model is RGBA8 hardware
blending followed by RGB5/A1 UNORM8 conversion after each guest draw, with the
converted texture fed to subsequent draws/readback/Present. Sources are not
quantized before blending. It is a compatibility approximation, not legacy
D3D9 driver bit-exactness; intra-draw overdraw can diverge substantially from
per-fragment5-bit rounding. JSON `blendComparisons` reports both references
without treating the illustrative strict model as a conformance requirement.
