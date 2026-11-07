# Original WMV decoding through WebAssembly

The original `Data/intro/atvi_logo.wmv` decodes using the official single-thread
`@ffmpeg/core` 0.12.10 package, without a custom codec build. This is an isolated
codec proof. It does not provide WMVCore COM objects, stream negotiation,
seeking, sample delivery, audio playback, or game integration. No virtual DLL
marker or dependency-preflight bypass was installed.

## Reproduce locally

From the repository root, with Python 3.12+, Node, and native FFmpeg:

```sh
python3 scripts/media-probe/prepare.py '/path/to/Game Files/Data/intro/atvi_logo.wmv'
```

This downloads only the pinned official package if absent, validates its npm
SHA512, extracts it locally, verifies the exact original fixture, runs the same
Wasm core locally, runs a native FFmpeg reference, compares output, and stages
an isolated worker probe. The source clip is read only and rechecked afterward.
Native FFmpeg 7.1 was used for the recorded comparison.

Open `http://127.0.0.1:4173/runtime/media-probe/index.html` with the existing
local server and click **Run bounded decode**. The page remains responsive and
shows a heartbeat while a dedicated worker runs. Result JSON is also available
as `globalThis.moviesMediaProbeResult`. The worker checks the fixture hash and
compares both outputs against local Wasm hashes. It is terminated after the
result, or after a 60-second deadline. Each decode also has a 30-second core
execution timeout. No external network service receives the media.

## Verified local results

Input: 1,235,199 bytes, WMV3 640×480 YUV420p video and WMAv2 stereo 44.1kHz
audio, duration 10.766 seconds. Input SHA256:
`1ef433c169969e03e01869e0cf076bf89e7e09fa6d141259835c8790f9755c09`.

| Output | Bound | Comparison with native FFmpeg 7.1 |
| --- | --- | --- |
| First YUV420p frame | 460,800 bytes | Byte-identical |
| Signed 16-bit stereo PCM | 184,752 bytes / 1.04735 seconds | Same length; max absolute sample difference 1, RMS 0.445805 |

The audio command requests one second; FFmpeg's timestamp/frame boundaries
produce approximately 1.047 seconds for this ASF fixture. We retain that actual
length rather than falsely reporting exactly 44,100 stereo frames.

Video SHA256: `481e3eaf8ab8d69d48f2e9f4eb1f67d0bf43c02daf178a41e4f45f154c767160`.
Wasm PCM SHA256: `091003df5f17fe4c183e087945a87a37150ceba82a63cfadfee1e5c8e4607037`.

Local warm decode measured approximately 25 ms video and 7 ms audio, excluding
module initialization. These are bounded fixture timings, not game playback
performance. The probe never decodes the entire clip into RGBA. Input is small
and fully resident; a future full-movie implementation needs bounded packet and
sample queues rather than accumulating frames in MEMFS.

## Provenance and license

The official npm artifact is pinned by URL and integrity in `prepare.py` and
`stage.py`. `.cache/media-probe/manifest.json` records input, tarball, core JS,
core Wasm, output hashes, and reference metrics. Logs and reference/output raw
files remain in that ignored cache; generated browser assets stay under ignored
`public/runtime/media-probe/`.

- Package: `https://registry.npmjs.org/@ffmpeg/core/-/core-0.12.10.tgz`
- Tarball SHA256: `d00089ce82e1bdf637ddbe42e0c3d41a1ba8cf4c9e825e7fa4d0bb970e844bd4`
- Core Wasm: 32,232,419 bytes, SHA256 `9f57947a5bd530d8f00c5b3f2cb2a3492faa7e5d823315342d6a8656d0a6b7b7`
- Core JS SHA256: `b266ab5b952555881dd6310663986994a182acb2b7ff25cf10a25f7a37ac2b21`
- Binary reports FFmpeg 5.1.4, Emscripten 3.1.40, SIMD enabled, threads disabled.
- Official package declares **GPL-2.0-or-later**; its binary enables GPL and
  includes unrelated encoders such as x264/x265. This package is not an LGPL-only
  minimal decoder. Publishing it requires corresponding GPL source/license
  packaging; that distribution bundle has not been assembled by this probe.
- npm metadata provides no `gitHead`; the artifact is exactly pinned, but we do
  not claim an independently reproduced upstream build or audited source commit.

Primary sources: [official package manifest](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/main/packages/core/package.json),
[official architecture and dependency versions](https://ffmpegwasm.netlify.app/docs/overview/),
[official build recipe](https://github.com/ffmpegwasm/ffmpeg.wasm/blob/main/Dockerfile).
The recipe confirms FFmpeg n5.1.4 and Emscripten 3.1.40; its main branch is a
reference, not an immutable provenance claim for the downloaded binary.

A smaller custom decoder can remove unrelated libraries and expose incremental
libavformat/libavcodec operations, but requires a separate pinned source build
and license audit. The existing core proves WMV3/WMAv2 decoding is available;
it does not remove the need to implement the original game's actual Windows
Media interfaces faithfully.

## Bounded ASF writer proof

`encode.cjs` queries the actual core's encoder registry: WMV1, WMV2, WMAv1,
WMAv2 are present; **WMV3 encoding is absent**. It repeats the decoded first
640×480 frame at 25fps and encodes the decoded PCM for a requested one second,
using WMV2 at 1Mbit/s and WMAv2 at 128kbit/s into ASF. The first source frame is
near-black (Y17, U128, V128); this is a container/codec execution test, not an
assessment of complex-image quality. No arbitrary Windows Media profile is
claimed to be supported.

The result is 45,591 bytes, SHA256
`bd6c9b0f11a63b3f9a4e3f3f3d159f00c6df8ce66d9f3c8b170c64fca3497696`.
Native ffprobe confirms WMV2 640×480 and WMAv2 44.1kHz stereo. The ASF duration
is 1.092 seconds; decoded PCM is 163,840 bytes (about 0.929 seconds). Encoder
padding/delay and timestamps therefore need an explicit future sample/timeline
contract; this does not establish sample-exact writer duration.

Both the same Wasm core and native FFmpeg 7.1 decode the generated ASF. Their
first video frame matches byte-for-byte; PCM lengths match and S16 samples
differ by at most 1. Evidence: `.cache/media-probe/encode-result.json`,
`encoders.log`, `encode.log`, `roundtrip.asf`, `roundtrip-ffprobe.json`, and
`roundtrip-reference.log`. `prepare.py` includes the bounded Wasm writer run.
The independent native roundtrip inspection was performed with:

```sh
ffprobe -v error -show_streams -show_format -of json .cache/media-probe/roundtrip.asf
ffmpeg -nostdin -y -i .cache/media-probe/roundtrip.asf \
  -map 0:v:0 -frames:v 1 -pix_fmt yuv420p -f rawvideo .cache/media-probe/roundtrip-reference.yuv \
  -map 0:a:0 -t 1 -c:a pcm_s16le -f s16le .cache/media-probe/roundtrip-reference.pcm
```

The browser probe's second button runs decode + writer + roundtrip decode,
checks ASF and decoded-sample hashes against the local identical Wasm core,
and reports a separate `writer` JSON object. The worker is still terminated
when complete. This does not register an ASF writer COM class or advertise
WMVCore availability to the game.
