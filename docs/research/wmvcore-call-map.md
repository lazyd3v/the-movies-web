# Windows Media ABI and actual startup requirements

Read-only audit, 2026-11-07, original MoviesSE.exe SHA256
`48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`.
Addresses are original x86 virtual addresses. This document combines static
Capstone disassembly, existing original-game traces, and local ffprobe inspection;
it does not claim that media works in the static Wasm runtime. No original bytes,
conditionals, or installed-module markers were changed.

## What blocks startup

There are two separate checks, neither of which is either delayed WMVCore factory:

1. `9A6960` obtains the system directory, appends `\\wmvcore.dll`, and, when
   byte `105BE88` is true, calls the file-existence helper at `9A6A5A`.
   The helper ultimately uses `FindFirstFileA`/`FindClose` without inspecting
   contents. The current browser run fails this check and displays the localized
   Windows Media Player/component error. See [DirectInput preflight](directinput-call-map.md).
2. After file checks, `9A6E52` calls `A2C470`: CoInitialize; create FilterGraph,
   CaptureGraphBuilder2, and WM ASF Writer; associate the capture builder with
   the graph; then add the writer as `L"ASF Enc/Writer"`. Negative HRESULTs fail
   startup. The Boxedwine trace previously proved this path is executed. This
   check does not itself read a profile, encode frames, or decode an intro.

Required classes/interfaces and call sites:

| Call | Class / interface or operation |
| --- | --- |
| `A2C4B1` | FilterGraph `{e436ebb3-524f-11ce-9f53-0020af0ba770}`, IGraphBuilder `{56a868a9-0ad4-11ce-b03a-0020af0ba770}` |
| `A2C4C9` | CaptureGraphBuilder2 `{bf87b6e1-8c27-11d0-b3f0-00aa003761c5}`, ICaptureGraphBuilder2 `{93e5a4e0-2d50-11d2-abfa-00a0c9c6e38d}` |
| `A2C4DB` | ICaptureGraphBuilder2::SetFiltergraph |
| `A2C4F4` | WMAsfWriter `{7c23220e-55bb-11d3-8b16-00c04fb6bd3d}`, IBaseFilter `{56a86895-0ad4-11ce-b03a-0020af0ba770}` |
| `A2C50B` | IGraphBuilder::AddFilter(writer, name) |

A legitimate implementation can create an initially stopped graph and writer
with owned objects, reference counts, graph membership and negotiated interfaces.
It must attach an actual media backend before advertising a supported writer;
returning success for these calls without an operational media path would only
bypass the compatibility probe. File enumeration, module loading and factories
must describe the same implemented capability. The probe alone is not a useful
media acceptance test.

## WMCreateSyncReader is an audio-reader path

Delay slot `F87A3C`, initial binder `C9B666`, callable tail-jump wrapper `C9B670`.
Only one direct call to that wrapper was found: `C4BD05`, inside `C4BC70`.
It passes `(NULL, 1, &this->reader)`; this is not a bare factory-presence check.

Interface layouts were checked against the
[Wine 11 wmsdkidl.idl](https://github.com/wine-mirror/wine/blob/wine-11.0/include/wmsdkidl.idl).
Slots below are zero-based, including IUnknown's three slots.

| Site | Object and method | Observed use |
| --- | --- | --- |
| `C4BD05` | WMCreateSyncReader | Reader stored at object +8 |
| `C4BD1D` | IWMSyncReader slot23 / +`5C`, OpenStream | Uses game-owned IStream at object +`C`, prepared by `C7D350`; do not assume a filesystem filename |
| `C4BD38` | QueryInterface | IID_IWMHeaderInfo3 `{15cc68e3-27cc-4ecd-b222-3f5d02d80bd5}`, literal `D75F98`, stored +`14` |
| `C4BD51` | IWMSyncReader slot18 / +`48`, GetOutputFormat | Output0, format0, result stored +`10` |
| `C4BD7B` | IWMMediaProps slot4 / +`10`, GetMediaType | 2048-byte buffer; real WM_MEDIA_TYPE with pointed-to format data required |
| `C4BD8B`–`C4BDAD` | WAVEFORMATEX access | Reads channels at format+2, sample rate +4, average bytes/sec +8 |
| `C4BB32`, `C4BB7E` | IWMHeaderInfo slot5 / +`14`, GetAttributeByName | Size-query then value-query, including UTF16 `Duration` literal `DACB98`; 64-bit duration is consumed |
| `C4BA2C` vicinity | IWMSyncReader slot5 / +`14`, SetRange | Deferred seek from object's stored 64-bit time |
| `C4BEFA` | IWMSyncReader slot7 / +`1C`, GetNextSample | Stream0, actual sample, time/duration/flags/output result pointers |
| `C4BF1E` | INSSBuffer slot7 / +`1C`, GetBufferAndLength | Copies actual returned sample bytes into caller buffer, retaining leftovers |
| `C4BFB2` | INSSBuffer::Release | Sample lifetime matters |
| `C4BAD0` | IWMSyncReader slot4 / +`10`, Close | Checked HRESULT |
| `C4BC15` etc. | Release reader/stream/media props/header objects | Must balance real ownership |

`C4BC70` is called by `C1E89B` in `C1E840`, which is called at `BE325B`.
The format reads and byte-copy loop establish an audio path. They do not establish
that this reader drives intro video. There are no loose `.wma` files in Data;
packed/imported media remains possible. A claim that implementing this single
factory plays the original intros would be unsupported.

A bounded real reader would need game-IStream callbacks, ASF demuxing, decoded
PCM output, accurate format/Duration metadata, seek/range state, end-of-stream
HRESULTs, and COM-owned sample buffers. Returning empty samples or invented
metadata is not equivalent. Unsupported formats must fail explicitly.

## WMCreateProfileManager belongs to writer setup

Delay slot `F87A40`, initial binder `AA6035`, callable wrapper `AA6050`.
Only one direct wrapper call was found: `A2E9E3` in `A2E980`, identified by the
original diagnostic string `MapProfile: Failed to create profile manager!`.

| Site | Operation |
| --- | --- |
| `A2E9E3` | WMCreateProfileManager(&manager) |
| `A2EA0B` | IWMProfileManager slot3 / +`0C`, CreateEmptyProfile(`0x70000`, &profile), i.e. WM7 profile |
| `A2EA61` / `A2EAD1` | Game file-size/read helpers obtain supplied profile data |
| `A2EAE2` | Manager slot5 / +`14`, LoadProfileByData(UTF16 data, &profile) |
| `A2EA31`, `A2EA80` etc. | Release manager on cleanup paths |

IWMProfileManager IID is `{d16679f2-6ca0-472d-8d31-2f5d55aee155}`.
The sole direct caller of MapProfile is `A2FD56` in `A2FC30`, which first queries
writer interface `{45086030-f7e4-486a-b504-826bb5792a3b}` (IConfigAsfWriter,
literal `D75AA8`) at `A2FD1D`, then calls its slot7 / +`1C`
ConfigureFilterUsingProfile at `A2FD72`. Its direct callers are `A2FF82` and
`A2FFC2`. This is writer configuration, separate from the simple startup graph
probe. Exact user actions reaching these later callers need runtime tracing;
static reachability alone does not prove startup execution.

A profile manager can truthfully maintain parsed profile state without encoding
at creation time, but its writer must reject unsupported codecs/profiles. A
WebM/MP4 encoder cannot silently claim to implement WMV-profile ASF output.
[Microsoft profile creation contract](https://learn.microsoft.com/en-us/windows/win32/wmformat/creating-profiles).

## Actual bundled media and browser implications

Local ffprobe found 114 loose `.wmv` and 102 `.ogg` files under Data, no loose
`.wma` or `.prx`. Packed archives were not exhaustively inventoried by this audit.
All three intro files contain WMV3/Windows Media Video9 at640×480 and WMAv2 audio
at44100Hz stereo:

| File under Data/intro | Container duration |
| --- | --- |
| atvi_logo.wmv | 10.766 seconds |
| frontend_intro.wmv | 10.920 seconds |
| frontend_loop.wmv | 57.040 seconds |

The [current W3C WebCodecs registry](https://www.w3.org/TR/webcodecs-codec-registry/)
lists neither WMV nor WMA, and does not require implementations to support any
codec. Therefore direct WebCodecs/HTML-video playback must not be assumed.
Practical choices are an actual ASF/WMV3/WMAv2 decoder compiled to Wasm, or
reproducibly generated browser-codec derivative assets with a source-hash manifest
and a compatibility adapter preserving timestamps, seek, original dimensions,
audio and completion events. Derivatives would leave original files untouched,
but do not by themselves implement the guest COM graph or export path.

The next useful vertical slice is real decoding of a supplied intro into timed
frames and PCM, validated against a reference decoder; then implement the exact
original graph/player interfaces that consume it. The current reader call-map
and startup writer check are insufficient to specify that video-player graph.
No module should be advertised as installed merely to reach a later trap.

## Verified reader bridge checkpoint (2026-11-07)

The dedicated native COM reader fixture passed in the browser against the real decoder service: original-style guest IStream Read/Seek callbacks → IWMSyncReader C ABI → synchronous service bridge → decoded PCM → owned INSSBuffer. Evidence: `docs/evidence/media-browser-native-reader.json` and `.jpg`; reproducible fixture build: `scripts/media-probe/build-native-reader.py`.

The tested input was a 97,317-byte audio-only ASF remux preserving the original WMAv2 compressed stream (SHA256 `c222252cc5d39ed7844a2fb95fab951ef88c99c63b23845d7211030f3a2200c7`). Two guest IStream reads supplied it. Output was 1,900,544 PCM bytes, FNV-1a `9959043a`; seeking to1.0–1.5seconds produced88,200bytes. EOF and resource release completed. This verifies the implemented audio-reader ABI and actual codec service together, not original game playback. The current native subset explicitly rejects multi-stream inputs; the original intro WMVs contain both audio and video. DirectShow graph/custom renderer integration and game-controlled playback remain separate work.
