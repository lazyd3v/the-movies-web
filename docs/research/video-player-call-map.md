# Original intro video: graph, sample and texture path

Read-only static audit, 2026-11-07. MoviesSE.exe SHA256
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.
All addresses are original x86 VAs. No original code, conditions, module markers
or runtime implementation changed. This maps a concrete candidate integration
boundary; it does not prove these calls have executed in the static Wasm build.
The separate startup ASF-writer probe and WMVCore audio reader are covered in
[wmvcore-call-map.md](wmvcore-call-map.md).

## Intro filenames reach the original movie-player interface

The filenames are UTF16, explaining their absence from an ASCII-only search:

| Literal | References / operation |
| --- | --- |
| `D1ADD8`: `data/intro/atvi_logo.wmv` | `45E36C` and `45E4F9` pass the filename to player virtual slot2 (+8), then slot6 (+18) starts playback |
| `D38330`: `data/intro/frontend_loop.wmv` | `690610` passes filename to `6B9390`; also referenced in frontend function `695460` at `695CD0` |
| `D38CA8`: `data/intro/frontend_intro.wmv` | Referenced at `695B2B` in frontend function `695460` |

`6B9390` obtains the player at widget+`344`, calls virtual slot2 with filename
and four zero arguments, and sets widget+`349` true. The movie-player vtable
`D73EA4` has slot2=`9EF270`. That function opens the graph through `9EE800` at
`9EF330`, then builds a game texture/material/quad using the negotiated dimensions.
The virtual-call target connection is a static class-layout inference; a runtime
trace should confirm the concrete player vtable for each intro path.

Another player/capture interface uses vtable `D73E7C`; its slot1=`9EF100` selects
file playback (mode2, `9EE800` at `9EF165`) or camera capture (mode1, `9ED880`).
Capture enumerates devices and is not necessary to decode the bundled intros.
Do not implement camera enumeration just because it shares the texture renderer.

## File playback graph setup

`9EE800(filename, option)` creates a graph and an original, executable-owned
custom renderer. It does not create a video HWND or ask the browser to overlay
an HTML video element.

| Site | COM operation (slots include IUnknown) |
| --- | --- |
| `9EE89C` | CoCreateInstance FilterGraph, IGraphBuilder |
| `9EE8F7` | Original custom renderer constructor `9ED710`, allocation `0x190` bytes |
| `9EE952` | IGraphBuilder slot3 AddFilter(renderer+`C`, `L"The Movies Texture Renderer Filter"`) |
| `9EE9B0` | Renderer IBaseFilter slot11 FindPin(`L"In"`) |
| `9EEA26` | For `.wmv`/`.asf`: CoCreateInstance WMAsfReader `{187463a0-5bb7-11d3-acbe-0080c75e246e}`, IBaseFilter |
| `9EEA9D` | AddFilter(asfReader, `L"Asf Reader"`) |
| `9EEB1F` | QueryInterface IFileSourceFilter (literal `DAF3AC`) |
| `9EEBA1` | IFileSourceFilter slot3 Load(filename,NULL) |
| `9EEC23` | Helper `9ED250` renders unconnected source output pins |
| `9EECDF` | Other formats: IGraphBuilder slot14 AddSourceFilter(filename, `L"SOURCE"`, &source) |
| `9EED4B` | Non-ASF source FindPin(`L"Output"`) |
| `9EEDC9` / `9EEF07` | Non-ASF option branch: graph slot12 Render(output) or slot11 Connect(output, rendererInput) |

Helper `9ED250` calls source IBaseFilter::EnumPins (+`28`) at `9ED26F`,
IEnumPins::Next at `9ED28F`, IPin::ConnectedTo (+`18`) at `9ED2AD`, and
IPin::QueryDirection (+`24`) at `9ED2D8`. When ConnectedTo returns
`VFW_E_NOT_CONNECTED (80040209)` and direction is output1, it calls
IGraphBuilder::Render (+`30`) at `9ED2F0`. Thus the ASF path renders source
outputs, including the audio output; this is not just a video-sample callback.
Graph negotiation must choose the existing compatible custom video sink and a
real audio renderer, with actual pin membership, media types and lifetimes.

Graph interfaces subsequently queried:

| Site | Interface IID | Stored graph-wrapper offset |
| --- | --- | --- |
| `9EEDF0` | IMediaControl `56a868b1-0ad4-11ce-b03a-0020af0ba770` | +4 |
| `9EEE00` | IMediaPosition `56a868b2-0ad4-11ce-b03a-0020af0ba770` | +8 |
| `9EEE10` | IMediaSeeking `36b73880-c2c8-11cf-8b46-00805f6cef60` | +C |
| `9EEE20` | IBasicAudio `56a868b3-0ad4-11ce-b03a-0020af0ba770` | +10 |

`9EEE28` sets IBasicAudio volume0. `9EF363` subsequently applies the player's
configured volume using the same slot7 (+`1C`) property setter. The graph must
retain audio state rather than discard that request.
Interface definitions: [Wine 11 control.idl](https://github.com/wine-mirror/wine/blob/wine-11.0/include/control.idl).

## Guest custom renderer and frame ABI

Constructor `9ED710` calls the compiled DirectShow base constructor `C8D960`,
then installs its own main C++ vtable `D73D74` and IBaseFilter vtable `D73D34`
at object+`C`. This C++ object is not a fabricated host COM object. Its existing
pin/base-class code can be exercised through a genuine connection.

- `9EE2B0`, main C++ slot44, checks the media type: FORMAT_VideoInfo
  `{05589f80-c356-11ce-bf01-00aa0055595a}`, MEDIATYPE_Video
  `{73646976-0000-0010-8000-00aa00389b71}`, MEDIASUBTYPE_RGB24
  `{e436eb7d-524f-11ce-9f53-0020af0ba770}`. Other types are rejected.
- `9EE310`, slot36, reads AM_MEDIA_TYPE.pbFormat at +`44`, then VIDEOINFOHEADER
  bitmap width at +`34` and signed height at +`38`. It retains width and absolute
  height at renderer+`160`/+`164` and allocates two `width*height*3` buffers.
- Buffer records are renderer+`16C` and +`17C`, each `{width,height,pointer,sequence}`.
  Counter is +`168`; buffer sequence fields are +`178` and +`188`.
- `9EE3B0`, slot43, is the actual sample renderer. At `9EE3BF` it calls
  IMediaSample slot3 GetPointer(&data), chooses the older buffer, copies exactly
  `width*height*3` bytes, and updates its sequence number. It does not inspect
  sample timestamps here. Timing belongs to the upstream/base renderer pipeline.

The documented DirectShow delivery path is IMemInputPin::Receive into the base
renderer, which eventually invokes DoRenderSample; see
[Microsoft CBaseRenderer](https://learn.microsoft.com/en-us/windows/win32/directshow/cbaserenderer)
and [Microsoft base renderer source](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/Win7Samples/multimedia/directshow/baseclasses/renbase.cpp).
A decoder integration should supply an actual IMediaSample with owned guest
memory and timestamps through the negotiated input pin. Calling the private
DoRenderSample address directly would skip connection, state and timing logic;
that is not established as an equivalent implementation.

The observed consumer assumes tightly packed RGB24/BGR byte order. For640-pixel
intros, a row is1920bytes and naturally DWORD-aligned. General odd-width row
padding, signed-height orientation and texture UV orientation must be validated
with reference frames; the renderer discards the height sign and performs no
row flip in the copy above. Do not assume a WebCodecs RGBA plane can be copied
unchanged into this buffer.

## Presentation is a game D3D texture

`9EF39B` calls texture creation wrapper `99BB50` with negotiated width/height and
format `0x15` (D3DFMT_A8R8G8B8). The original game owns the material and quad.

At `9EE520`, per-frame update:

1. `99A5E0` locks the texture. Its call `99A5FB` is IDirect3DTexture9 slot19
   LockRect(level0, lockedRect, NULL, flags`0x800`).
2. Select the newest of the two guest frame buffers.
3. `9EE590`–`9EE5B0` copies each three-byte BGR pixel to four bytes with alpha255.
   The destination address comes from D3DLOCKED_RECT.pBits. This loop itself does
   not advance by a separate pitch at row boundaries.
4. `99A620` calls texture slot20 UnlockRect(level0) at `99A62C`.
5. `9EE5D0`/`9EE6F0` call that update then the game's draw helper `9973C0`.

This requires a real D3D texture lock/upload/presentation path. Showing decoded
frames in a separate DOM video would not fulfill the observed game interface or
its UI composition. The texture adapter must provide a layout the original
copy loop can safely use and validate dimensions/lengths before sample delivery.

## Clock, playback state and completion

IMediaControl and IMediaPosition inherit IDispatch, so their useful slots start
at7, not3:

| Site | Operation |
| --- | --- |
| `9ED3B8` | IMediaPosition slot8 put_CurrentPosition(0 seconds) |
| `9ED3C1` | IMediaControl slot7 Run |
| `9ED425` | IMediaControl slot8 Pause |
| `9ED475` | IMediaControl slot9 Stop |
| `9ED4C7` | IMediaPosition slot7 get_Duration(double seconds) |
| `9ED507` | IMediaPosition slot9 get_CurrentPosition(double seconds) |

`9ED520` combines current position/duration; `9ED5A0` uses that progress to
transition to stopped state near the end. Precise loop/restart behavior must be
confirmed against the frontend call path, rather than inferred solely from the
filename `frontend_loop`. The graph's media clock must stop advancing during
pause, seek coherently, and deliver actual end-of-stream; a constant duration or
wall clock that advances while paused will break this consumer. No complete
IMediaSeeking method inventory or every base renderer wait/event dependency has
been claimed by this bounded audit.

## Decoder consumption contract to implement and test

A useful backend boundary is ASF source → decoded video/audio → original graph
sinks. It needs: source identity and bounded reads; RGB24 negotiation and dimensions;
owned sample bytes; 100ns DirectShow presentation/duration metadata; PCM audio with
real sample rate/channel layout; shared playback/seek/pause state; reference counts;
flush/end-of-stream and errors. A browser worker may decode ahead, but callbacks
into translated guest COM must be marshalled safely to its supported execution
context. DOM postMessage alone cannot wake guest code synchronously blocking on
an unimplemented Win32 event.

Acceptance should compare decoded frames and PCM to a reference, then prove the
original pin receives samples, the game uploads the correct texture, and A/V
position and end/loop behavior are real. None of these are yet browser-verified
by this call-map. The existing decoder feasibility work can use this contract;
it should not advertise a module merely because it can parse an ASF header.

## No supported no-media flag established

A scan of all21 absolute text references to global `105BE88` found only two
direct stores: `542E80` and `5431EE`, both write1. The latter is immediately before
renderer initialization `9A3480`. Other references gate renderer and additional
subsystems, not only media; none of these references is a direct config setter.
No original command-line or registry mechanism to turn this flag off was found.
This does not rule out indirect writes elsewhere, but is not evidence for a
supported no-video mode. Setting the global to0 or patching checks would change
the original behavior and is not proposed.
