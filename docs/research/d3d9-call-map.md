# Original executable Direct3D9 bridge implementation map

Static inspection of MoviesSE.exe SHA256
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.
Addresses are original x86 virtual addresses (image base `400000`). This is
not an executed rendering trace. Names below map vtable offsets to the
[Wine 11.0 Direct3D9 ABI header](https://github.com/wine-mirror/wine/blob/wine-11.0/include/d3d9.h).
Slots are zero based, including IUnknown's three methods; byte offset = slot × 4.

## Window and callback dependency comes first

At `5430B7` the executable stores **`542380`** as WNDCLASS.lpfnWndProc.
`RegisterClassA` is called at `543113`; `CreateWindowExA` at `543187` returns
an HWND saved in ESI. `GetClientRect` immediately follows at `543195`.
The HWND is then supplied to renderer wrapper `9A3480` at `543202`; that
wrapper calls renderer initialization `9A7A50` at `9A34DA`.
The first argument is saved as the rendering window in global `105BEB0`.

Ghidra's `.cache/recomp-full/functions.json` does not list `542380`, but the
translator's pointer-target heuristic **did recover and register it**:
`out/recomp_035.c` defines `f_00542380` (`PTR_00542380`), `out/recomp.h`
declares it, and `out/recomp_stubs.c` registers guest address `00542380`.
There is no missing-function blocker for this callback in the current output.
The window API must actually dispatch it with stdcall
`(HWND, UINT, WPARAM, LPARAM)` arguments and propagate its guest LRESULT.
Returning a fabricated result without invoking it can bypass game state changes.

Message pump `5422B0` calls PeekMessageA with PM_REMOVE=1, then TranslateMessage
at `542303` and DispatchMessageA at `54230A`. WM_CLOSE (`10`) and WM_QUIT (`12`)
set a flag returned by the pump. It also conditionally filters message `105`.
The rendering reset/lost-device path has another message pump (imports loaded
at `9A5DAA`/`9A5DB7`). HWND identity, client dimensions, message queue, callback
execution and return values must agree across windowing and rendering APIs.

## Actual initialization sequence

`9A7A50` calls delayed-import stub `AFBA4E` at `9A7AE6` with SDK version **32**.
The Direct3DCreate9 return is stored in global **`105CA40`** (IDirect3D9*).
A NULL result fails initialization.

1. `9A7B01`: IDirect3D9 slot **8**, GetAdapterDisplayMode, adapter 0,
   output `105CA48`; a negative HRESULT fails initialization.
2. `9A7B0C` calls helper `9A79D0`. The program clears 14 DWORDs at
   **`105C8C0`**, the D3DPRESENT_PARAMETERS structure. It sets backbuffer count
   1, swap effect 1, automatic depth/stencil enabled, format `50` hexadecimal
   (D3DFMT_D16), and chooses windowed/fullscreen from global `105BE92`.
   Width/height and color format depend on current game settings.
3. `9A7BBE`: IDirect3D9 slot **16**, CreateDevice, adapter 0, HAL device type 1,
   focus HWND from `105BEB0`, parameters `105C8C0`, output **`105CA44`**.
   Behavior flags are `40` hexadecimal (hardware vertex processing), optionally
   ORed with `4` (multithreaded). On failure it retries at `9A7BF1` with `20`
   (software vertex processing), retaining optional `4`. Both failures return false.
4. `9A7C0E`: Device slot **7**, GetDeviceCaps, output **`105C910`**.
5. Helper `9A51B0` performs format checks. GetRenderTarget slot **38** at
   `9A7C24` saves render target 0 at `105CA68`; GetDepthStencilSurface slot
   **40** at `9A7C37` saves the surface at `105CA6C`.
6. Calls `A3AEB0` and `A26CE0`, additional resource setup, then IDirect3D9
   slot **5** GetAdapterIdentifier at `9A7CAE` and adapter-name comparisons.

Do not advertise arbitrary capability bits or formats to get past these tests:
the returned capabilities select the subsequent game rendering paths.

## IDirect3D9 calls directly tied to the global object

| Slot | Method | Representative call sites |
|---:|---|---|
| 5 | GetAdapterIdentifier | `9A7CAE` |
| 6 | GetAdapterModeCount | `9A7921` |
| 7 | EnumAdapterModes | `9A7943` |
| 8 | GetAdapterDisplayMode | `9A7B01` |
| 10 | CheckDeviceFormat | `9A51D3`, `9A51FA`, `9A5221`, `9A5248`, `9A526F` |
| 16 | CreateDevice | `9A7BBE`, `9A7BF1` |

## Device calls directly tied to global `105CA44`

| Slot | Method | Representative call site |
|---:|---|---|
| 3 | TestCooperativeLevel | `9A5E1C` |
| 4 | GetAvailableTextureMem | `A3AED1` |
| 7 | GetDeviceCaps | `9A7C0E` |
| 16 | Reset | `9A5EA6` |
| 17 | Present | `9A6370` |
| 23 | CreateTexture | `99A7B1`, `99B211`, `99B26D` |
| 26 | CreateVertexBuffer | `A3A67A`, `A3AE17` |
| 27 | CreateIndexBuffer | `A3AC9C`, `A3AE37` |
| 30 | UpdateSurface | `9A5335`, `A2B4A3` |
| 32 | GetRenderTargetData | `9A52D9`, `A2B410` |
| 34 | StretchRect | `A2B3B4` |
| 37 | SetRenderTarget | `9A53B5`, `9A7082` |
| 38 | GetRenderTarget | `9A5F1C`, `9A7C24` |
| 40 | GetDepthStencilSurface | `9A5F40`, `9A7C37` |
| 41 | BeginScene | `9A4F8B`, `9A4FD6` |
| 42 | EndScene | `9A4FBF`, `9A4FE9` |
| 43 | Clear | `9A5077`, `9A56D7` |
| 44 | SetTransform | `991894`, `991E92` |
| 47 | SetViewport | `9A5B20`, `9A5B70` |
| 48 | GetViewport | `9A970E` |
| 49 | SetMaterial | `9954CE` |
| 51 | SetLight | `9CC660` |
| 53 | LightEnable | `9CC674` |
| 55 | SetClipPlane | `982888`, `AA7D2E` |
| 57 | SetRenderState | `888044`, `8888C1` |
| 58 | GetRenderState | `994FCB` |
| 65 | SetTexture | `991502`, `9915F4` |
| 66 | GetTextureStageState | `994F57` |
| 67 | SetTextureStageState | `99105D`, `99108E` |
| 68 | GetSamplerState | `995027` |
| 69 | SetSamplerState | `7BD7CD`, `7BD7FE` |
| 81 | DrawPrimitive | `A57BD8` |
| 82 | DrawIndexedPrimitive | `8D9A95`, `9969BA` |
| 89 | SetFVF | `8D9A07`, `996977` |
| 100 | SetStreamSource | `8D9A2C`, `99699A` |
| 104 | SetIndices | `8D9A4A`, `9E4EDC` |

`Present` at `9A6370` passes four NULL optional arguments and compares the
HRESULT with **`88760868`** (D3DERR_DEVICELOST); that result calls `9A5D60`.
Device-loss handling and Reset therefore affect actual control flow, not just
error reporting. Resource references held across Reset require defined ownership.

## Fixed-function and shader evidence

Strong fixed-function evidence exists: direct-global-object analysis found 18
SetTransform call sites, 18 SetFVF sites, 162 SetTextureStageState sites, 58
SetRenderState sites, and explicit material/light setup. A practical first
bridge should capture these states and generate/cache equivalent browser shaders
for **observed combinations**, with vertex/index upload and texture resources.
Getters must report the same state that setters store.

This does **not** prove that programmable shaders are absent. The executable
contains vertex/pixel shader profile and assembler strings (`vs_1_1`, `vs_2_0`,
`ps_1_4`, etc.), consistent with linked shader support code. This limited scan
found no Create/SetVertexShader or Create/SetPixelShader call directly rooted in
`105CA44`; aliases, helper arguments, embedded COM objects and alternate global
stores were not covered. Shader strings alone do not establish an executed shader
path. Instrument interface calls to resolve this before choosing a shader scope.

## Bounded implementation sequence

1. Execute the recovered WndProc `542380`; implement the observed window/client
   geometry/message APIs with consistent HWND state. Reach real Direct3DCreate9.
2. Provide an owned guest COM object/vtable for IDirect3D9, adapter/display/format
   queries and CreateDevice; implement the observed parameter and HRESULT paths.
   Instrument every vtable slot with arguments and caller EIP. Unsupported slots
   must trap by name, not succeed.
3. Implement device caps plus render/depth surface objects, reference ownership,
   and the actual initialization methods. Inspect subordinate Texture/Surface/
   VertexBuffer/IndexBuffer vtables and lock/unlock callers before implementing
   resource memory contracts; this audit has not mapped those methods.
4. Implement measured fixed-function draw state → browser shader/program cache,
   streams/FVF, texture upload, viewport, clear, Begin/EndScene and Present.
   Validate the first original frame and render-target readback requirements.
5. Extend from dynamic traces for loss/reset, readback/media, programmable
   shaders and additional resource interfaces. A first frame is not gameplay.

The slot inventory is a **lower bound**: a conservative straight-line register
tracking scan over recovered function ranges seeded only by global `105CA40`/
`105CA44`, plus manual initialization disassembly. It misses aliases and calls
across control-flow joins, and the recovery can miss entire callback functions.
It should guide instrumentation and concrete milestones, not be treated as a
complete list of interfaces required for the game.
