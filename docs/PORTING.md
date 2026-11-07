# The Movies browser compatibility investigation

This project runs the original Windows executable through Boxedwine's WebAssembly
x86/Wine runtime. It does not recreate the simulation or compile the original
engine to WebAssembly. The original main menu renders and accepts mouse input
with pointer capture enabled. Studio gameplay remains unverified. Audio
initialization is observed, but playback is not yet verified.

## Local installation findings

The supplied installation contains 719 files totaling about 2.5 GB.
`MoviesSE.exe` is a 12,230,656-byte PE32 Intel x86 executable. `version.inf`
identifies The Movies Stunts & Effects version 1.2, internal version 17.0.
No engine source or build project is present. Source filenames embedded in the
executable are debugging strings, not source code.

Executable strings identify `d3d9.dll`, `Direct3DCreate9`, and `WMVCore.DLL`.
The supplied `Docs/readme.txt` requires DirectX 9.0c with hardware T&L and Windows
Media Player 9 codecs. Those requirements are the principal graphics and media
compatibility risks for a Wine/browser runtime.

The generated `launch.bat` imports inferred windowed 800×600 and graphics registry values
under `Software\Lionhead Studios Ltd\TheMovies`, and installation values under
`Software\Lionhead Studios Ltd\TheMovies Stunts & Effects`. Key names were found
in executable strings and `install.log`; value types, chosen values and their
effectiveness are **inferred and unverified**. The first-run dialog still reported high graphics quality, so these settings must not be described as verified low-detail configuration. No original game file is patched.

## Reproducibility

Run `python3 scripts/setup-runtime.py --cache-dir /tmp` to install the pinned
Boxedwine build 121 MultiThreadedJit runtime. Downloads and reused files are
checked against SHA-256 hashes. Changed runtime files cause an error rather than
being overwritten. `public/runtime/manifest.json` records URLs and file hashes.

Run `python3 scripts/prepare-game.py` to package the supplied installation, or
provide another `Game Files` directory as its positional argument. Files are
streamed into a deterministic ZIP_STORED/ZIP64 archive. The adjacent manifest
records original relative paths, sizes, version and executable SHA-256; it does
not reveal the source directory. Original files are never modified.

The optional `python3 scripts/build-runtime.py --jobs 3` requires Python 3.12+,
Git, make and clang++. It builds the pinned Boxedwine source with
`patches/boxedwine-stored-read-ahead.patch`, using Emscripten 5.0.7, and stages
an isolated multithreaded JIT variant in `public/runtime/read-ahead`. The patch
buffers 256 KiB reads of stored ZIP entries. Source/bootstrap hashes, toolchain
release identifiers, patch hash and artifact hashes are recorded in that
directory's manifest. Compilation and cache tests passed, and this variant
reached the original main menu; no overall speedup has been measured.
It does not replace the upstream runtime or the Wine root. The launcher disables
the optional mode when its artifacts are absent.

## Runtime limitations to verify

- The 2.45 GiB archive mounts successfully through the bounded HTTP range filesystem. The browser test confirmed ZIP64 mounting and Wine startup; cache capacity is 32 MiB.
- Wine Direct3D 9 acceleration and the browser graphics bridge must support this
  particular game's rendering path. WMV startup playback may fail independently.
- The multithreaded runtime requires cross-origin isolation and SharedArrayBuffer.
  Ordinary static hosting without suitable response headers is insufficient.
- Saves, audio playback, studio input, stability and performance require interactive tests.
- Game packaging and runtime setup do not establish a working gameplay result.

## Upstream sources

- [Boxedwine source](https://github.com/danoon2/Boxedwine)
- [Pinned build 121 archive](https://www.boxedwine.org/builds/branches/master/builds/121/artifacts/build-121.zip)
- [Matching Wine root](https://www.boxedwine.org/builds/demos/build/master/121/mt-jit/TinyCore15Wine11.0-web.zip)
- [Browser filesystem helper at pinned commit](https://github.com/danoon2/Boxedwine/blob/26bf258ab0dc00aa1ec6da81d1ca263f922f8947/project/emscripten/fsfs.js)
- [Boxedwine license at pinned commit](https://github.com/danoon2/Boxedwine/blob/26bf258ab0dc00aa1ec6da81d1ca263f922f8947/license.txt)

The setup copies Boxedwine's GPL license into `public/runtime/LICENSE-Boxedwine.txt`.
The filesystem helper includes its Emscripten MIT attribution. The Wine root
contains additional software with its own licensing; the Boxedwine license does
not replace those licenses or the original game's terms.

## Observed browser results (2026-11-04–05)

- Boxedwine build 121 / Wine 11 successfully rendered Notepad in the browser.
- Original `MoviesSE.exe` ran and displayed its first-run graphics dialog.
- Without native media components it then reported `Missing Windows Media Component!`; the log identified missing COM class `{7c23220e-55bb-11d3-8b16-00c04fb6bd3d}` (WM ASF Writer). Wine 11's builtin qasf implements the reader, but not this writer.
- `scripts/setup-media.py` extracts a checksum-pinned WMP10 package into a separate overlay. Its DLLs registered successfully in the browser, including qasf and wmvcore.
- The original main menu rendered with the read-ahead runtime; [screenshot evidence](evidence/original-main-menu.jpg) is retained. Enabling **Lock/hide mouse pointer** allowed relative mouse movement and clicks; without it, the game cursor remained at the top-left.
- Studio simulation, movie export, save/load and playable performance remain unverified. A menu selection began a transition with Windows Media calls, which does not establish successful video playback or entry into the studio.
- The original Intel startup splash and loading progress bar rendered. AudioWorklet initialization also began; this does not yet verify audio playback.
- A hidden browser severely throttled startup: observed archive reads increased from approximately 2 MiB per 20 seconds to 190 MiB per 20 seconds after `visibility.set(true)`. The prolonged white window was therefore not sufficient evidence of a graphics failure. Keep the game tab visible during loading; these figures measure that test's archive progress, not frame rate or general network throughput.

Upstream references for the media compatibility layer: [Winetricks](https://github.com/Winetricks/winetricks/blob/master/src/winetricks), [Wine 11 qasf](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/qasf/qasf_main.c), [Wine 11 writer](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/wmvcore/writer.c).

## Emulator crash identified and repaired

All three tested execution modes initially trapped with `RuntimeError: null function` after `NtQuerySystemInformation`. The single-threaded stack identified `fread`; inspecting the caller's string constant identified `/proc/stat`. Pinned `platform/linux/platform.cpp` reads it with `BReadFile(...).readAll()` without checking that the file opened. This is a missing browser-host procfs file, not evidence of a graphics/JIT failure.

`public/proc-stat.js` supplies read-only virtual CPU accounting with monotonically increasing synthetic idle ticks and per-open snapshots. It does not report actual host CPU usage. The initial static version removed the crash in a browser test; the dedicated module adds coherent snapshots and tests. Failure traces before the fix are retained under `docs/evidence/`.

## Exact startup media check (read-only executable analysis)

The original executable was inspected with `objdump` and byte reads; no game
instructions or checks were patched. The addresses below are virtual addresses
for the supplied PE32 executable, image base `0x00400000`. They identify what
must work, rather than establish which call currently fails.

The call at `0x009a6e52` invokes the boolean compatibility probe at `0x00a2c470`.
It performs this sequence, returning false on any negative HRESULT:

| Call site | Required operation |
| --- | --- |
| `0x00a2c477` | `CoInitialize(NULL)` |
| `0x00a2c4b1` | Create `CLSID_FilterGraph` (`e436ebb3-524f-11ce-9f53-0020af0ba770`), requesting `IID_IGraphBuilder` (`56a868a9-0ad4-11ce-b03a-0020af0ba770`) |
| `0x00a2c4c9` | Create `CLSID_CaptureGraphBuilder2` (`bf87b6e1-8c27-11d0-b3f0-00aa003761c5`), requesting `IID_ICaptureGraphBuilder2` (`93e5a4e0-2d50-11d2-abfa-00a0c9c6e38d`) |
| `0x00a2c4db` | `ICaptureGraphBuilder2::SetFiltergraph(graph)` |
| `0x00a2c4f4` | Create `CLSID_WMAsfWriter` (`7c23220e-55bb-11d3-8b16-00c04fb6bd3d`), requesting `IID_IBaseFilter` (`56a86895-0ad4-11ce-b03a-0020af0ba770`) |
| `0x00a2c50b` | `IGraphBuilder::AddFilter(writer, L"ASF Enc/Writer")` |

On failure, the caller displays `SM_AVI_RENDER_FAILED_TITLE` and
`SM_AVI_RENDER_FAILED_MESSAGE`. The supplied `stringdatabase.lhts` maps that
title to `Missing Windows Media Component!` and its message to the request for
a newer Windows Media Player and codecs. This specific check does not enumerate
codecs or load an encoding profile. Successful DLL registration alone does not
prove that writer creation or graph insertion works.

A separate earlier check verifies that the system-directory `wmvcore.dll`
exists. Its localized title is `Error`, from
`SM_D3D_OPEN_FAILED_WMCOMPONENT_TITLE`; it is distinct from the observed popup.

For tracing, `WINEDEBUG=+ole,+loaddll,+quartz,+wmvcore` covers COM creation,
loaded native/builtin modules, and Wine's graph implementation. Wine 11's
[`FilterGraph2_AddFilter`](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/quartz/filtergraph.c)
logs the filter name under `quartz` and propagates an unsuccessful
`IBaseFilter::JoinFilterGraph` HRESULT. Search for `ASF Enc/Writer` to distinguish
failure before graph insertion from failure during it.

`setup-media.py` also reproduces two original runtime installation details:

- The unmodified `WMVCORE.DLL` resource `PRXFILE/200` is extracted as
  `C:\windows\wmsyspr9.prx` (316640 bytes, SHA-256
  `95f1edbc3cb9aa70a374a61b43bd16818636b0190aa841504130bb718c39c2c7`).
  The original `DllRegisterServer` already attempts this extraction; including
  the file explicitly makes its presence deterministic.
- The original `WMFSDK10.inf` section `Reg.Codecs` supplies empty string values
  beneath `HKCR\Windows Media\WMSDK\AudioEncode` and `VideoEncode`. These
  entries are separate from DLL registration. A generated `.reg` file imports
  exactly that section, with an independent `media-codec-reg-v1.done` stamp.

These are compatibility installation steps, not changes to the game or vendor
DLL instructions. With both additions, the next browser run passed the game's
media probe: the trace reported `MediaEventSink_Notify` event `0x50`, removed
`ASF Enc/Writer`, and continued into `DINPUT8.dll`, HID and SetupAPI loading
without the missing-media popup. This experiment does not isolate whether the
explicit profile file, codec registry entries, or both were necessary. The
subsequent main-menu test confirms further startup progress, not studio gameplay.
