# D3D9 backend: reusable code and bounded implementation plan

Audit date: 2026-11-07. Read-only inspection of retro-port commit
`ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf`, under
`.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp`.
Original-game call evidence is in [d3d9-call-map.md](d3d9-call-map.md).
This audit does not establish that a game frame has rendered with the new runtime.

## Main result

The cached implementation has a real **WebGPU/WGSL** backend, not WebGL.
`host/d3d8.c` is a Direct3D8 COM/state frontend; `render_rec.c` converts geometry,
textures and state into commands; `render_wgpu.c` executes them. `vshader.c` is
an interpreter for vertex shader 1.1 on the CPU; `pshader.c` translates a subset
of pixel shader 1.1 to WGSL. There is no ready-made D3D9-to-WebGL bridge here.

Use tested conversion algorithms and state conventions selectively. Importing
its COM frontend wholesale would introduce silent successful stubs, an allocator
conflict, inaccurate capabilities, and incompatible ABI layouts. Importing the
entire WebGPU backend would additionally change the current runtime's browser
thread model. Neither is a small dependency substitution.

## Useful pieces, with limits

| Source | Potential reuse | Concrete limitations |
|---|---|---|
| `texconv.c`, `d3d8.c:18-38` | Pixel conversion, DXT decoding, block pitch/row calculations | Validate each format, mip, pitch and allocation overflow independently; reject unsupported formats instead of default32bpp. DXT2/4 premultiplication needs explicit tests. |
| `render_rec.c:153-211` | FVF offsets, matrix/color helpers | `convert_vertex` copies only two texture-coordinate floats even for other declared dimensions; ignores normals and blend weights. Not a general FVF implementation. |
| `render_rec.c:375-470` | Dirty texture versions, draw-state snapshots, primitive expansion | Expands indexed primitives into per-draw CPU vertices; CPU vertex-shader work and allocations are substantial potential performance costs. Only two fixed-function texture stages become uniforms, despite eight advertised blend stages. |
| `render_wgpu.c:28+` | Fixed-function shader formulas, alpha test, pipeline cache, actual depth/blend GPU execution | WGSL/WebGPU objects cannot be fed directly to WebGL. Audit depth range, orientation, pretransformed RHW, half-pixel conventions and texture stage operations against image tests. |
| `vshader.c` | Some shader1.1 instruction math and declaration decoding | D3D8 declaration tokens, 96 constants, CPU execution. Not D3D9 shader2/3 support. Parser and operand bounds need hardening before guest input. |
| `pshader.c` | Some ps1.1 operand and texture instruction rules | WGSL output only; unsupported instructions return NULL. Caller `render_wgpu.c:498+` logs failure and can fall back to fixed-function rendering: unacceptable as faithful shader execution. |
| `render_cmd.h` | Self-contained draw/upload/readback command design | Current synchronous guest worker can retain this design without adopting upstream pthread/main-thread GPU plumbing. |

The game's explicit SetLight/LightEnable calls (`9CC660`/`9CC674`) cannot be
fulfilled by the existing renderer: the recorder only handles ambient/emissive
lighting when normals are absent (`render_rec.c:446-465`). Its packed GPU vertex
has no normal, and the fixed-function conversion ignores vertex blend weights.
Fog, clip-plane and skinning behavior therefore also need an explicit support
matrix before any corresponding capability is advertised. Merely storing these
states in `d3d8.c` does not implement their rendering effect.

## Risks that must not carry over

* `com.c:com_dispatch` returns S_OK for every NULL implementation; QueryInterface
  accepts any IID. Replace with exact interface IDs, owned references and traps
  for unimplemented slots. Upstream `heap_alloc` would violate our native heap's
  exclusive ownership invariant; use a separately reserved object region or our
  allocator through an explicit integration contract.
* `d3d8.c:70-85` claims an NVIDIA GeForce6800GT identity and WHQL status;
  `fill_caps:110-142` advertises numerous fixed capability bits, eight blend
  stages, four textures and shader1.1 without checking real implementation.
  The game queries adapter identity at `9A7CAE`; false identity can select
  hardware workarounds. Return a truthful virtual adapter identity.
* `CheckDeviceFormat` checks only the numeric format, ignoring usage/resource
  combinations; CheckDepthStencilMatch always succeeds. Format availability
  must reflect render-target, sampling, upload and readback implementation.
* `CreateDevice` ignores the renderer init return; headless is the default
  backend. Present always returns S_OK; Reset changes dimensions without
  rebuilding back/depth surfaces. Require actual backend initialization and
  presentation acknowledgement; implement loss/reset resource semantics.
* Buffer Lock (`d3d8.c:930`) returns `base+offset` without validating requested
  size or flags. Texture/surface locks likewise need bounds, lock state,
  rectangles, pitch and ownership checks. These are memory contracts, not
  just graphics conveniences.

## D3D9 frontend is a separate ABI

Use the [Wine D3D9 header](https://github.com/wine-mirror/wine/blob/wine-11.0/include/d3d9.h)
for exact vtables and [Microsoft's migration reference](https://learn.microsoft.com/en-us/windows/win32/direct3d9/converting-to-directx-9)
for changed semantics. In particular:

* IDirect3D9 has17 slots versus upstream IDirect3D8's16; Device9 has119.
  The game's CreateDevice slot16 would call beyond the upstream D3D8 table.
* D3DPRESENT_PARAMETERS9 adds MultiSampleQuality before SwapEffect: offsets
  after MultiSampleType differ. The upstream device decoder reads D3D8 offsets.
  D3DCAPS9 extends the212-byte D3DCAPS8 struct to304bytes; the adapter identifier
  also adds DeviceName. Do not reuse raw struct writes.
* SetStreamSource9 adds byte offset; preserve it when resolving every vertex.
  SetIndices9 removes BaseVertexIndex, which moves to DrawIndexedPrimitive9 and
  is signed. Wrong adaptation silently selects wrong vertices.
* FVF is set by SetFVF9, distinct from a vertex-shader COM object and a vertex
  declaration. Upstream D3D8 overloads shader handles/FVF and declaration tokens.
* Sampler states move out of texture-stage states. The game directly calls
  Get/SetSamplerState (`995027`, `7BD7CD`); use a separate state array and an
  explicit conversion when feeding shader/texture sampling state.
* Device render-target operations add target indices and separate depth-stencil
  binding. Surface9 inherits the full resource interface, so GetContainer,
  GetDesc and LockRect slots differ from Surface8. Texture9 adds auto-generation
  methods before level methods. Buffer9 interface layouts are closer but still
  need exact IDs and signatures. Resource call sites have not yet been fully
  mapped; instrument them rather than assume D3D8 offsets.

## Smallest operational original-draw path

1. Add a strict IDirect3D9 factory and slot tracing; record caller, parameters
   and HRESULT. Follow original `9A7AE6`→GetAdapterDisplayMode`9A7B01`→
   CreateDevice`9A7BBE`. Validate SDK32, adapter/type/HWND/presentation settings.
   Backend initialization must actually create the chosen graphics context.
2. Implement truthful caps/format queries and real backbuffer/D16 surface
   ownership; satisfy GetRenderTarget`9A7C24` and GetDepthStencilSurface`9A7C37`.
   Trace `A3AEB0`/`A26CE0` initialization and every resource vtable call.
   Creation without a real graphics context must fail.
3. Implement only the actual first draw's resource formats and state: buffer
   allocation/Lock/Unlock, texture levels/LockRect/UnlockRect, stream offsets,
   FVF, indices and draw bounds. Execute actual game DrawPrimitive`A57BD8` or
   DrawIndexedPrimitive`8D9A95`/`9969BA`, using the original supplied vertices,
   indices and texture bytes. These static addresses are candidates, not proof
   of the first dynamically executed draw.
4. Build/cache GLSL (WebGL route) or validated WGSL (WebGPU route) for observed
   state combinations; reject missing state effects. Clear/viewport/depth/blend
   and texture addressing/filtering must match those states. A textured UI
   quad may be a bounded first case if the trace proves it; do not force a
   fabricated FVF or replace original draws with a demo triangle.
5. At original Present`9A6370`, deliver the resulting original frame and verify
   a visible result plus readback hashes/reference pixels. Report draw count,
   state signature and original resource provenance. First-frame success does
   not establish gameplay, shaders, device reset or movie playback support.

For the proposed worker OffscreenCanvas→ImageBitmap path, keep GPU ownership in
that worker. Transfer the bitmap to the page, draw/consume it there, then signal
an explicit SAB frame acknowledgement. Do not require a message callback on the
blocked guest worker to complete Present. Enforce at most one outstanding frame,
propagate failed presentation/context loss, and close unconsumed bitmaps. Resize
and reset need explicit attachment recreation. Acknowledgement proves host
consumption of the frame, not physical scanout. Readback should use the worker's
render target directly; presentation transport must not become a substitute for
GetRenderTargetData/UpdateSurface/StretchRect, all seen in the original map.

## Dependencies and license

[Upstream source at the audited commit](https://github.com/Dhaiwat10/retro-port/tree/ecea26bcaa5e3bf4aa1d2171ff038d4cce5e37bf)
is MIT, copyright2026 Dhaiwat10 and Stephan Cilliers. Preserve its LICENSE notice
in any copied substantial source. The existing header comments do not constitute
separate third-party license grants; audit additional dependencies if copying
beyond these files.

`build_web.sh` uses `--use-port=emdawnwebgpu`, pthreads, PROXY_TO_PTHREAD,
WASMFS and a main-page GPU command pump. `render_wgpu.c` also has a native
wgpu-native/Metal path. The current port does not gain those dependencies simply
by compiling d3d8.c. If selecting WebGPU, pin the Dawn/Emscripten port dependency
and preserve its transitive notices; this audit did not fetch or license-audit
that port. Selecting worker WebGL instead can reuse arithmetic and conversion
concepts without introducing Dawn, but requires a new shader/backend adapter.

## Implemented foundation checkpoint (2026-11-07)

The selected implementation now uses WebGPU directly, with original bounded
WGSL shaders rather than copying the toolkit's frontend or renderer. A real
browser run passed on Apple `metal-3`, nonfallback, using a190×128 BGRA target
and actual D16. GPU pixel tests establish indexed diffuse draw, signed base
vertex/uint32 indices, W/V/P transform effects, depth occlusion, UV sampling and
readback conversion. Presentation consumes an ImageBitmap in the page and
acknowledges via SAB inside requestAnimationFrame. This establishes browser
virtual-display frame pacing, not physical scanout timing.

The initial nested-worker plus immediate Atomics.wait transport timed out in the
browser despite passing the Node transport test. The corrected architecture
starts the GPU worker on the page, awaits module-ready/port-connected handshakes,
then transfers a MessagePort/shared response buffer to the guest. The guest can
block synchronously while the dedicated GPU worker awaits initialization or
mapAsync. The page retains the GPU worker error handler and publishes failures
through SAB. Timeouts remain finite; unsupported commands fail explicitly.

`native/d3d9-api.c` now supplies typed D3D9 COM objects with exact D3D9 slots,
304-byte caps,56-byte presentation parameters,1100-byte adapter identifier and
32-byte surface descriptors. It uses the reserved05000000..05040000 metadata
region and0e020000..0e025000 host-method trampolines. Buffer Lock storage uses
private-owner helpers in the existing heap allocator; it never calls the
upstream guest allocator. COM resource ownership, references and bound-buffer
lifetime are exercised by sanitizer tests.

The native frontend intentionally exposes a smaller subset than the JS backend:

* One real, already initialized device matching the acknowledged HWND's client
  dimensions exactly. No implicit resize, multisampling, refresh-rate change,
  or reset success. SW vertex-processing creation is supported; the initial HW
  T&L request returns NOTAVAILABLE because lighting/HW T&L is not advertised.
* Virtual adapter identity, current single display mode, conservative format
  checks, and actual backbuffer/D16 surface descriptors. Shader versions,
  lighting limits remain zero. The texture extension below exposes only its
  implemented formats, filtering and stage operations.
* XYZ+diffuse FVF0x42/stride16 or XYZ+diffuse+UV FVF0x142/stride24, one stream, vertex/index buffers,
  bounded locks and dirty upload caching. Indexed triangle-list draws validate
  signed base vertex, supplied min/range and every index. Only16/32-bit index
  formats are accepted.
* W/V/P transforms and viewport; depth disabled or D16 write+less-equal;
  lighting disabled and cull-none must be selected explicitly. Other rendering
  state values trap rather than being silently ignored. Begin/EndScene and
  clear/present ordering are checked. Clear currently supports whole-target
  color plus depth together (or color alone when no depth attachment exists).
* Typed QueryInterface/AddRef/Release and resource GetDevice/GetType/GetDesc.
  Unimplemented slots have named traps. No texture/surface readback COM methods,
  shader programs, blend/alpha-test, fog, stencil, lights, RHW, render-target
  switching, reset or movie rendering are implied.

The native ABI browser fixture invokes those actual C COM methods in Wasm,
passes through EM_JS→`d3d9-host.js`→synchronous client→dedicated GPU worker,
checks center RGBA`[0,255,0,255]` and clear RGBA`[17,34,51,255]`, then presents
and releases/destroys all resources. It passed in the real browser. Evidence:
[foundation](../evidence/graphics-browser-foundation.json),
[RAF presentation](../evidence/graphics-browser-raf.json), and
[native ABI](../evidence/graphics-browser-native-abi.json).
These are synthetic contract fixtures, not original-game frames. The next
milestone is observing and implementing the first unsupported state/resource
combination selected by the original executable, without inflating caps to
force it down an unimplemented path.


## Observed video texture extension (2026-11-07)

Executable evidence: `9EF394` passes format21 through `99BB50→99B980→99AB70`.
The `99A510` constructor clears the render-target bit for this format. The common
call at `99B26D` therefore requests CreateTexture(width,height,1,0,21,1,out,NULL):
one A8R8G8B8 managed level. `99A5FB` calls texture slot19 with level0, NULL rectangle,
NOSYSLOCK0x800; `99A62C` calls slot20 UnlockRect. `9EE590..9EE5B0` converts BGR24
into contiguous BGRA bytes with alpha255, so the bridge returns width×4 pitch.

The native implementation now provides that exact creation path, checked subrect
locks, READONLY/NOSYSLOCK, level descriptors and level surface/container identity,
real guest storage, BGRA→RGBA GPU upload and replacement, stage0 texture binding,
UV vertices, point/clamp and texture SELECTARG1/modulation. Missing combinations
remain explicit failures/traps. It advertises one texture stage, alpha/perspective,
point min/mag and clamp only. GPU dimensions and resource budget are taken from the
real backend. Reference ownership keeps externally held surfaces/textures/devices
alive and releases bound resources on device destruction.

Validation: native ASan/UBSan and linked Wasm/Node COM contract tests pass. Expanded
real GPU fixture checks two native BGRA uploads against RGBA `[51,34,17,255]` then
`[171,205,239,255]`; browser verification passed with all6 readbacks matching. Evidence is saved in
`docs/evidence/graphics-browser-native-texture.json` and `.jpg`. This does not claim original
movie rendering: game-selected render states/FVF and media delivery still need to
reach this bridge in the original flow.

Contracts: [CreateTexture](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createtexture)
and [LockRect](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dtexture9-lockrect).
Startup diagnostics now record at most16 calls per queried method, including
original return address, arguments, HRESULT and CreateDevice presentation data;
this is separate from per-frame rendering and full import tracing.


## Logical 16-bit presentation target

The original first request at `9A7BC1` used HWVP|FPU_PRESERVE0x44 and its fallback
at `9A7BF4` used SWVP|FPU_PRESERVE0x24, with1024×768 format25 (A1R5G5B5), D16,
fullscreen and default interval. Both previously returned NOTAVAILABLE: the
frontend required exact0x20 and format22. The updated frontend accepts the preserve
bit without modifying guest x87 stack/control/status; contract tests seed and
verify those fields. HWVP remains unsupported. Format25 is gated on a real backend
capability bit and an acknowledged format selection, not merely accepted by C.

WebGPU has no packed A1R5G5B5 render attachment. This bounded implementation stores
its logical values in the existing8-bit target: each fragment clamps then rounds
RGB to32 levels and alpha to2 levels before storage. Clear uses the same rule.
Readback and ImageBitmap presentation copy that same quantized target. Blending,
dithering and render-target feedback remain unsupported; these would require
additional precision/ordering work. The implementation does not allocate or expose
a packed guest backbuffer; LockRect on it remains a named unsupported method.

`native-abi.html?format=25` tests the actual native creation flags, logical surface
descriptor, clear, draw, texture sampling/replacement and readback. Native
ASan/UBSan, linked Wasm contracts and JS validation pass. Actual browser16-bit verification passed all6 readbacks, including clear
`[16,33,49,255]`, textures `[49,33,16,255]` and `[173,206,239,255]`. Evidence:
`docs/evidence/graphics-browser-native-16bit.json` and `.jpg`. Default fixture URL retains format22 for regression.

Official references: [D3DFORMAT](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dformat),
[D3DCREATE](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcreate),
[EnumAdapterModes](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3d9-enumadaptermodes).

## Original state-cache initialization (2026-11-07)

After actual device creation, `994F10` initializes the game's state cache. The
call returning at `994F5D` queries all18 valid texture-stage states for stages0–3.
The byte mask at `E67558` enumerates1–11,22–24,26–28,32. The first prior unsupported
query is stage0/state7 (BUMPENVMAT00). The subsequent render loop returns at994FD1;
it scans up to209 and includes legacy/reserved numbers. The sampler call returning
at99502D reads all13 valid sampler states for samplers0–3 (`E67654` mask).

Native query storage now covers8 fixed-function stages and16 pixel samplers with
documented initial values. The render query covers the D3D9 enum's documented
states; reserved IDs and write-only DEBUGMONITORTOKEN return INVALIDCALL instead
of inventing values. POINTSIZE_MAX follows the frontend's zero MaxPointSize cap.
Queries preserve actual prior supported mutations. Exact redundant setter calls
succeed; changing unsupported features still traps, with bounded caller/stage/state/
value diagnostics. In particular, querying default bump, blend or disabled-stage
settings does not advertise or implement those rendering operations.

Tests cover all stage/sampler default entries, stage separation, changed values,
invalid output ranges, sparse enum holes, every render-state default, and explicit
rejection of second-stage enable, alpha blending and linear filtering. Native
ASan/UBSan and linked Wasm tests passed. This change does not alter GPU draw code.

References: [texture-stage state](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtexturestagestatetype),
[sampler state](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype),
[render state](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype).
Factual defaults were cross-checked against pinned Wine11.0 `stateblock.c`;
no Wine implementation source was copied into the frontend.

## Actual sampler initialization

`995123` selects POINT mip filtering, `995157` selects LINEAR minification,
`99518D` selects LINEAR magnification, and `9951C3` sets LOD bias-1.0, for samplers0–3.
The bridge now retains these states and passes stage0's actual address/filter/bias
settings on each draw. WebGPU samplers implement wrap, mirror and clamp addressing,
point/linear min/mag, and single-level point/linear mip selection. WGSL
`textureSampleBias` applies the requested bias. Its supported range is explicitly
bounded to[-16,15.99], matching WGSL; unsupported address/filter/bias values trap.
Resources still contain exactly one mip, so LOD clamps to0 and mip-chain creation
remains unsupported and unadvertised. Sampler bindings are cached by finite state
combinations; continuously varying bias is uniform data rather than a cache key.

The native ABI fixture now adds red/blue linear interpolation, nearest sampling,
and out-of-range UV wrap/mirror/clamp checks (16 readbacks total). Native sanitizer,
linked Wasm and JS validation/transport tests pass. Actual browser verification
passed all16 readbacks, including16-bit linear midpoint `[132,0,132,255]` and
wrap/mirror/clamp red/blue results. Evidence: `docs/evidence/graphics-browser-native-samplers.json`
and `.jpg`.

## Alpha comparison and material state

Initializer99526D sets ALPHAFUNC=GREATER5 and995292 sets ALPHAREF=100. The frontend
now stores these settings and supports actual ALPHATESTENABLE through fragment
shader discard for all8 comparison functions. Comparison uses normalized8-bit
reference before logical16-bit output quantization; discarded fragments do not
write color/depth. AlphaCmpCaps reflects these implemented comparisons. Blending
remains unsupported. Native and linked Wasm tests pass; the real GPU fixture adds
all8 comparison branches against alpha127/reference100 (32 readbacks total).

The later call9954D4 sets a68-byte material. SetMaterial validates and stores finite
fields, and GetMaterial returns the last explicitly set material. This does not
implement lighting: a draw with lighting enabled still traps. Material storage is
observable state needed for the original initializer, not fake illuminated output.

Actual browser alpha fixture passed all32 readbacks. The initial WGSL identifier
`pass` was rejected by the browser compiler; renaming it to `alphaPassed` fixed
compilation. Shader diagnostics now include getCompilationInfo line/column details.

## Light-definition startup slice

Actual wrapper9CC650 passes a104-byte D3DLIGHT9 plus its index, then enables the
light at9CC67A and sets global LIGHTING=true at9CC6A3. The observed trap register
EDX identifies index1; exact light data is now logged rather than inferred from
other callers. SetLight/GetLight and LightEnable/GetLightEnable retain16 sparse
indices, validate guest bounds and fields, preserve enable state on replacement,
and create Microsoft's default directional light for an undefined Enable index.
This supports observable state initialization only: GPU lighting is not advertised
or implemented. A draw with global lighting enabled still traps before submission.
The next rendering implementation needs the actual normal-bearing FVF and light
parameters. Native ASan/UBSan and linked Wasm tests pass, including this draw guard.

Reference: [LightEnable defaults](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-lightenable)
and [D3DLIGHT9](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlight9).

## Audited intro blending compatibility model (2026-11-07)

The observed mode6 draw uses SRCALPHA/INVSRCALPHA, ADD, and separate alpha
 disabled. Both RGB and alpha use these factors: alpha is
`As*As + Ad*(1-As)`, not ordinary source-over alpha. Native state setters retain
the original sequential configuration; a draw with any other active blend
combination traps. Stage0 supports the audited TEXTURE×DIFFUSE orderings.
Dithering, separate-alpha blending, other factors/operations, and MSAA remain
unsupported. These rules follow Microsoft's [D3DBLEND factors](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend)
and [render-target alpha contract](https://learn.microsoft.com/en-us/windows/win32/direct3d9/render-target-alpha).

The format25 implementation is deliberately a compatibility approximation:
hardware WebGPU blending accumulates unquantized-source fragments into an RGBA8
intermediate during each guest draw. A full-target pass then converts RGB to
nearest5-bit values and alpha to1-bit, stores their UNORM8 representations in a
second texture, and swaps the canonical targets. Subsequent draws, readback,
and Present all consume that converted texture. There is no hidden higher
precision feedback retained across guest draws. The two owned textures and
resolve bindings are destroyed with the backend. Opaque/default output retains
its earlier quantization; the boundary conversion is idempotent for that path.
Depth remains a separate attachment and the resolve pass never modifies it.

This is not a claim of native16-bit or original-driver bit-exact blending. The
actual WebGL2 measurements in `docs/evidence/blend16-webgl2-probe.json` already
showed that a5/5/5/1 attachment did not match the chosen strict per-fragment
rounding oracle. Neither that result nor the reviewed D3D9 documents establishes
one universally mandated legacy-driver rounding model. Dense overdraw may
produce substantially larger differences than one logical5-bit step; there is
no global one-step error bound. RGBA8 feedback also cannot represent every
mathematical k/31 value exactly.

The expanded native→EM_JS→WebGPU fixture has95 readback checks, plus actual
consumed bitmap validation after a blended draw. Its blend cases distinguish
intra-draw overlaps from separate guest draws, reverse triangle order, exercise
source alpha0/255 and transparent destination alpha, reject fragments by alpha
and depth tests, retain untouched pixels, and compare64 low-alpha overlaps
with64 separate draws. Result JSON labels the declared RGBA8-per-draw oracle
and the illustrative strict per-fragment oracle separately. Browser acceptance
must be recorded from actual runs; compilation/native tests alone do not prove
GPU rendering or original-game playback.

### Observed material20 source-color blend

The original trace confirmed wrapper parents `00993E43` / `00993E4C` selecting
SRCCOLOR / INVSRCCOLOR. The supported active combination is deliberately bounded:
no texture, stage0 RGB and alpha SELECTARG1 DIFFUSE, stage1 disabled, ADD and no
separate alpha. Unused SELECTARG1 ARG2 values do not change this equation.
For every RGBA component, output is `S*S + D*(1-S)`; alpha therefore uses source
alpha, not source red. This follows the [Microsoft D3DBLEND definition](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend).

The native33-word request retains its size: blend slot25 code2 and equation
slot31 code2 describe this mode; existing0/1 encodings remain unchanged. Public
commands default to the previous source-alpha mode. The new pipeline consumes
actual diffuse vertex data without binding a texture or changing guest bytes.
Format25 still uses RGBA8 accumulation inside each guest draw and RGB5/A1 resolve
at each draw boundary. This remains a compatibility model, not legacy-driver
bit-exact blending. Fixture cases70–72 verify componentwise color/alpha, overlapping
triangles within one draw, and feedback from the next draw. Browser acceptance is
recorded separately by the root agent after execution.

### Observed material19 destination multiplication

Original parents `00993D58` and `00993D61` confirmed ZERO/SRCCOLOR. The exact
supported source is `(diffuse.rgb, texture.a)`: both stage0 channels SELECTARG1,
RGB argument DIFFUSE, alpha argument TEXTURE, stage1 disabled, ADD and separate
alpha disabled. Texture RGB and diffuse alpha are intentionally unused. The
blend result is `D*S` componentwise; WebGPU uses ZERO/SRC for RGB and
ZERO/SRC_ALPHA for alpha, consistent with the Microsoft factor definition above.
Request codes25=3 and31=3 select this bounded mode without changing request size.

Fixture cases80–87 cover texture alpha0/128/255, changing ignored texture RGB,
changing diffuse RGB and ignored diffuse alpha, alpha-test rejection, and two
successive guest draws with the existing format25 conversion between draws.
Native tests assert exact mode routing and reject a changed active alpha argument
before GPU execution. Existing alpha/source-color/factor modes remain separate.

### Classic ADD factors, independent source equations

The next renderer revision generalizes the blend stage to classic D3DBLEND1–11
for either source or destination while keeping the implemented texture-stage
expressions separate. Earlier mode19/mode20 factor restrictions above describe
historical checkpoints, not new equations. Factors12–15 may be stored but remain
unsupported when active; non-ADD operations and separate alpha remain unsupported.
The RGB/alpha mapping is defined in `public/graphics-blend.js`: color factors use
source/destination RGB but their alpha factors use the corresponding alpha;
SRCALPHASAT uses `min(As,1-Ad)` for RGB and ONE for alpha, per Microsoft and the
[WebGPU blend specification](https://www.w3.org/TR/webgpu/#enumdef-gpublendfactor).

Legacy boolean/mode commands remain supported. Optional explicit numeric factor
pairs are validated; native requests preserve33 words and encode new pairs in
slot25 as `0x100 | source<<4 | destination`, retaining old codes0–3. Source equation
slot31 remains independent. There is no new texture-stage fallback.

Pipelines compile lazily using `createRenderPipelineAsync`; the serialized GPU
worker awaits compilation and rejects the command on failure. A64-entry LRU
bounds pipelines and drops their dependent per-texture bind groups on eviction.
Shader layout is keyed independently from factors: diffuse-only variants have no
texture binding, while modulate, factor, and diffuseRGB/textureAlpha share the
sampling shader with an explicit equation uniform. No121-pair eager startup loop
is used. Existing per-guest-draw RGB5/A1 resolution remains unchanged.

The expanded fixture adds all121 factor pairs with an independent numerical RGBA
oracle, repeats the first pair after eviction, crosses three factor pairs with
all four source equations, and verifies consecutive additive draws across16-bit
resolve boundaries. Native tests cover every pair and unsupported active legacy
factors; JS tests cover asynchronous cache completion, failure/retry and eviction.
Actual browser results are recorded separately after execution.
