# Bounded real ASF/WMAv2 PCM reader backend

The new `public/media-reader-{client,service,decoder,core}.js` files implement a
real independently driven audio reader. They do not install game callbacks,
WMV markers, COM factories, or modify the original media. The reader client
reuses the verified synchronous-SAB request transport; async prewarm runs
before any guest callMain. Metadata and PCM come from the pinned actual FFmpeg
core, not a synthetic Windows Media response.

## Final provider contract

Inside a worker, await `MoviesMediaReaderClient.connect(options)` and obtain
`client.readerProvider()`. Options support the existing page-owned MessagePort
and externally supervised control/payload buffers, or async-prewarmed nested
service workers. Core readiness verifies artifact hashes and WMAv2 decoding
availability before the service acknowledges initialization.

| Callback | Result |
| --- | --- |
| `moviesMediaReaderAvailable()` | Actual service capability acknowledgement, boolean; false on error |
| `moviesMediaCreateReader(size)` | Positive token, or0 on failure; declared compressed source size |
| `moviesMediaAppendReaderInput(token,offset,bytes)` |1 after accepted consecutive byte upload;0 on failure |
| `moviesMediaOpenReader(token)` | Actual metadata object; null on incomplete/invalid/unsupported source |
| `moviesMediaReadReaderSample(token,maxBytes)` | Owned sample object, explicit EOS object, or null on error |
| `moviesMediaSetReaderRange(token,start100ns,duration100ns=null)` |1 after restarting/opening the actual decoder;0 on failure |
| `moviesMediaReleaseReader(token)` |1 only after acknowledged cleanup;0 on unknown/double release |

Provider failures preserve `client.lastError`. Native factory availability can
be checked before input size exists; allocate the backend token after genuine
IStream::Stat/Seek/Read determines source size. No pointer or filename is
invented by this service.

Metadata currently accepts one ordinary, unencrypted WMAv2 stereo44100 audio
stream in ASF and exposes:

```js
{
  container:'asf', codec:'wmav2', audioStreamIndex:0,
  audioStreamNumber:1, sourceStreamCount:2,
  sampleRate:44100, channels:2, bitsPerSample:16, blockAlign:4,
  format:'S16LE', containerDuration100ns:107660000, maxSampleBytes:8192
}
```

These values are the original atvi_logo fixture's actual metadata, not constants
returned for all files. The decoded output format is deliberately restricted.
The underlying ASF may also contain video: this backend extracts audio only.
Native IWMSyncReader initially rejects `sourceStreamCount !== 1` rather than
claiming that all original streams are exposed. `audioStreamIndex` is FFmpeg's
zero-based index; `audioStreamNumber` is the actual ASF stream identifier.

A successful sample contains `eos:false`, owned `Uint8Array bytes`,
`start100ns`, `duration100ns`, sampleRate/channels/bitsPerSample, rational
`sourcePts,timeBase:[1,44100]`, and decodedSampleOffset for diagnostics.
`maxBytes` must be4–8192 and divisible by4. Partial packet reads advance PTS by
actual stereo sample frames. End of stream is **only** `{eos:true,stats,...}`;
its byte field may be absent. Decode errors return null, never EOS.

100ns values are safe JavaScript integers, never coerced to32bit. Start and
finite duration are restricted to a combined0–3600seconds. Ranges are
[start,end), selecting actual samples whose timestamp is within that interval.
Service duration `null` means through EOF; service duration0 means an empty
range. **The native Windows adapter must map guest SetRange duration0 to null**
(read-to-EOF), matching the original caller's Windows semantics.

## Storage, queues, seeking, and cleanup

Declared input size is validated before allocation: maximum32MiB per reader,
two readers,64MiB total resident compressed sources. Uploads must be contiguous
and fit the declared size;64KiB chunks fit comfortably in the1MiB RPC buffer.
Invalid offset/quota/incomplete-open requests do not mutate input accounting.
Release refunds the declared source allocation and terminates that decoder.

The coordinator retains compressed bytes for seeks. Each active decoder gets
one copy and writes it into its own codec MEMFS, so input transfer can transiently
hold additional bounded copies. Each decoder has its own Wasm heap and one
8192-byte PCM SAB slot. A pending sample blocks decoding until consumed; no
full decoded movie is stored. The codec also holds normal internal buffers and
a bounded64-entry timestamp metadata queue. The probe's native reference/output
arrays are test-only and are not present in the service.

Seek terminates the old decoder, opens a fresh verified decoder, and decodes
from the beginning while discarding samples outside the requested range. It
is accurate for the verified fixture but deliberately not indexed/fast seeking.
This bounds memory at the cost of repeated decode work. Range EOF is reported
only after decoding completes, including empty/beyond-end ranges. Queue waits
currently fail after15seconds without consumption; decoder execution and
request waits have60-second deadlines. This is not an indefinite pause or
hour-long realtime-playback guarantee despite accepting bounded metadata
ranges. Failures mark the affected session failed and require release/reopen.

The service closes all decoder workers on close; per-reader release and seek
also terminate old workers and notify outstanding queue waits. Async failures
are surfaced through the synchronous client reply. No private audio files are
written to the original installation or host user directories.

## Actual ASF stream identity

A small bounded parser walks the actual ASF Header's Stream Properties objects,
validates their64-bit sizes and identifiers, rejects duplicate/zero stream IDs,
encrypted audio, and multiple ordinary audio streams. Its stream count must
also match actual ffprobe stream enumeration; unsupported nested layouts fail
rather than inventing an identity. The original clip has:

- audio object at byte2536, size114, flags0x0001: ASF stream1;
- video object at byte2650, size133, flags0x0002: ASF stream2;
- header ends at byte2821, matching the bounded object walk.

A synthetic header using stream7 verifies the parser does not hardcode1.
The low7-bit stream identifier extraction follows the actual
[FFmpeg5.1 ASF demuxer](https://ffmpeg.org/doxygen/5.1/asfdec__f_8c_source.html);
Microsoft's [ASF structure description](https://learn.microsoft.com/en-us/windows/win32/medfound/asf-file-structure)
describes the per-stream header objects. No output number is inferred merely
from FFmpeg's zero-based index.

## Verification

Local actual-codec test: `node scripts/media-probe/reader-test.cjs`.
Lightweight state/bounds test: `node scripts/media-probe/reader-state-test.cjs`.
Stage: `python3 scripts/media-probe/stage.py`.
Browser: `/runtime/media-probe/reader.html`.

The real browser passed in980.87ms. Evidence:
`docs/evidence/media-browser-reader.json` and `.jpg`.

- Full original PCM:1,900,544bytes,475136 stereo sample frames;
  SHA256 `28f48cdfa917cdbf5b61aa5ba22fab4399959c11248ddb85ec29e3aab8f0872c`.
- Range1.0–1.5seconds:88,200bytes;
  SHA256 `d212685a2d2e3646fabfeb947dc93625ea7aacf71f1b631c9d3dfb3c411b3976`.
- PCM differs from independent native FFmpeg7.1 by at most1 S16 unit.
- Full/range hashes, timestamps, beyond-end/empty EOS, quota, malformed input,
  incomplete input, out-of-order upload, and double-release checks pass.
- Original first PCM PTS is929932100ns; final end108094331100ns. Container
  duration107660000 is a separate value. FFmpeg5.1.4 audio PTS still differs
  from7.1 by up to10.07ms; no sample-exact cross-version clock equivalence is
  claimed.

The browser evidence predates only the native-requested additional ASF stream
identity/count fields. Those fields passed subsequent actual-codec and malformed
header tests; native integration will exercise them again.

## Audio-only native fixture

To test a faithful one-stream native IWMSyncReader subset, the original WMAv2
compressed stream was remuxed into a separate ignored fixture, without decoding,
re-encoding, or modifying the source:

```sh
ffmpeg -v error -nostdin -y -i .cache/media-probe/input.wmv \
  -map 0:a:0 -c:a copy -f asf .cache/media-probe/reader-audio-only.asf
```

Native FFmpeg7.1 produced97317bytes, SHA256
`c222252cc5d39ed7844a2fb95fab951ef88c99c63b23845d7211030f3a2200c7`.
Actual reader metadata is sourceStreamCount1, audioStreamNumber1, and container
11.122seconds. Remux timestamps/duration must not be equated with the original
container. Decoding nevertheless produces the exact same1,900,544PCM bytes and
hash as above. Both the fixture and provenance are staged at
`/runtime/media-probe/reader-audio-only.asf` and `reader-audio-only.json` for the
native bridge tests. No original game stream is silently replaced with it.
