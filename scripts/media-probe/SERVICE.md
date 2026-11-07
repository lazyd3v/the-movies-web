# Bounded real ASF writer service

New sources: `public/media-service-engine.js`, `media-service-worker.js`, and
`media-service-client.js`. The static-probe page now supplies real create/configure/release provider callbacks after verified prewarm; no WMV module marker or fake factory is installed.
The independent service is exercised by `/runtime/media-probe/service.html`.

## Provider contract

Construct the client **inside a worker**, not the window:

```js
const client = await MoviesMediaServiceClient.connect({
  workerURL: '/media-service-worker.js',
  assetBase: '/runtime/media-probe/'
});
const provider = client.provider();
// The static-probe worker supplies these in Module construction options.
// Module.moviesMediaCreateWriter = provider.moviesMediaCreateWriter;
// Module.moviesMediaConfigureWriter = provider.moviesMediaConfigureWriter;
// Module.moviesMediaReleaseWriter = provider.moviesMediaReleaseWriter;
```

Before starting guest callMain, async `connect` yields the owning event loop
while the independent worker starts, then waits for it to fetch and
SHA256-verify the pinned core JS/Wasm, instantiate the actual decoder/encoder,
and discover real WMV2, WMAv2 encoders and the ASF muxer. Missing files,
integrity failures, missing capabilities, and timeouts fail initialization.
Verified JS is imported from a local Blob; Wasm is supplied as verified bytes.
Codec assets must be same-origin. There is no fallback successful backend.

`moviesMediaCreateWriter()` returns a positive service-owned uint32 token, or0
on error. `moviesMediaReleaseWriter(token)` returns1 only after acknowledged
cleanup, and0 for unknown/already released handles or service failure.
`client.lastError` retains callback errors for diagnostics. Native final release
can therefore require a real acknowledgement. A token represents a live writer
session backed by the ready encoder service; it is not a filename or COM pointer.

Only one synchronous request per ready client is allowed. Calling requests
before initialization completes fails; initialization itself never uses a
blocking Atomics.wait while a nested worker is still starting. Its1MiB SharedArrayBuffer
carries bounded JSON and optional binary data; a20-byte atomic control buffer
coordinates request/response. The service worker executes while the guest
worker waits using Atomics.wait. No UI thread is blocked. Timeout terminates
the service; it must be recreated. `client.close()` terminates it and discards
all private session files. Explicit release is required for normal COM lifetime.

## Explicit commands and limits

`client.request(command, optionalUint8Array)` supports:

- `capabilities`: actual ready flag, exact supported profile and byte limits;
- `createWriter`, `releaseWriter`, `status`;
- `configure`: token, profile, optional `byteBudget`;
- `appendVideo`: token, consecutive `frameIndex`, exact `pts100ns`;
- `appendAudio`: token, consecutive stereo `sampleOffset`;
- `finish`: token;
- `readOutput`: token, byte `offset`, requested `length` (up to1040000).

The current profile is deliberately narrow: WMV2, BGR24 640×480,25/1fps,
1Mbit/s; WMAv2, S16LE stereo44100Hz,128kbit/s. The complete profile object must
match `capabilities.profile`; missing/different fields are rejected. **No WMV3
encoder is advertised.** This is not general Windows Media profile negotiation.

Jobs are bounded at25 video frames and44100 stereo audio frames (up to1second).
Each video append is exactly921600 bytes and PTS is `frameIndex*400000`100ns
units. PCM appends are nonempty multiples of4bytes. Finish requires nonempty
video and exactly1764 stereo samples per video frame. Every append checks
format, ordering, duration and raw/global byte budgets **before modifying**
the private file. Quota rejection leaves status/file lengths unchanged.

Limits: four live sessions;32MiB raw bytes per session (configurable downward),
64MiB total live raw/output files,4MiB maximum output. Before encoding,4MiB of
remaining global headroom must be available. The single service thread cannot
process other requests during encoding. A bounded file write wrapper rejects
output growth past4MiB before mutation. Failed encoding marks that session
failed and poisons new writer/finish operations until the service is recreated;
cleanup remains available. Host/Wasm allocation failure is not disguised as
successful encoding.

Raw appends go directly into private codec MEMFS files rather than accumulating
additional JS arrays. Finish runs actual FFmpeg with WMV2/WMAv2 and ASF flags.
Output remains service-owned and is returned only through bounded reads. Release
unlinks raw/output files and refunds their logical byte accounting. Logical
budgets do not include Wasm heap allocator overhead or artifact fetch buffers;
the worker's Wasm memory can retain freed capacity until terminated.

## Evidence and limitations

`node scripts/media-probe/service-test.cjs` uses the actual pinned core. Tests
cover unsupported WMV3 profile rejection, quota atomicity, invalid timestamps,
state transitions, double release, global accounting refund and output bounds.
It feeds25 copies of the original decoded first BGR frame and44100 decoded
stereo samples. Output is45591bytes, SHA256:
`b0afae751c3a2da0b3e2d454f560ae6dd28054d9460711c6553870c43a4feb56`.
Native ffprobe identifies WMV2 640×480 and WMAv2 stereo44100; native FFmpeg
decodes both. Evidence: `.cache/media-probe/service-{result,ffprobe}.json`,
`service-output.asf`, and `service-roundtrip.{bgr,pcm}`.

The browser fixture invokes the same real encoder using the synchronous SAB
client, reads output in4096-byte chunks, verifies the identical ASF hash, checks
missing-codec and WMV3 rejection, and checks acknowledged/double release. UI
heartbeat proves window responsiveness. Result:
`globalThis.moviesMediaServiceProbeResult`.

The ASF reports1.092seconds for a requested1second due to codec delay/padding;
no sample-exact duration guarantee is claimed. Near-black first-frame fixture
proves codec execution and protocol, not encoder visual quality. Larger jobs,
continuous movies, arbitrary profiles, WMV3 encoding, game capture, graph media
negotiation, and a real Windows Media SDK COM writer remain unimplemented.
This service is a concrete backend for future native integration, not permission
to advertise unrelated codecs or install missing-runtime markers.


For a page-owned service worker, create a MessageChannel in the page, transfer
port1 to `/media-service-worker.js` with `{op:'attachPort',port:port1}`, then
transfer port2 to the guest worker. Use `await MoviesMediaServiceClient.connect({
port:port2 })` before callMain. Subsequent requests travel directly between the
workers. The page should retain ownership/error supervision of its service
worker. The provided fixture currently uses async-prewarmed nested workers;
The static-probe page implements page-owned error-to-SAB supervision: a fatal
control flag wakes blocked guest waits independently of JSON payload state,
and the page terminates both workers. Identity guards ignore old-session errors.
Stop, terminal and guest errors also dispose the service. The guest asynchronously
prewarms before runtime creation/callMain, and closes its port on terminal.
`moviesMediaConfigureWriter(token,profile)` returns1 only after actual service
configuration acknowledgement, or0 with lastError on rejection.

Tests: `service-shared-buffer-test.cjs` rejects shared TextDecoder input in both
directions; JSON is copied before browser WebIDL calls.
`page-lifecycle-test.cjs` checks page-owned startup transport, fatal SAB flags,
stop/terminal disposal and stale callback guards. Actual writer tests remain
`service-test.cjs`; the browser service fixture passed the exact ASF output hash.

## Recorded browser verification and automated checks

The repaired service fixture passed in the actual browser:45591-byte ASF,
SHA256 `b0afae751c3a2da0b3e2d454f560ae6dd28054d9460711c6553870c43a4feb56`,
12 chunked reads, missing-codec/WMV3/duplicate-release rejection,307.745ms total.
Recorded evidence is `docs/evidence/media-browser-service.json` and `.jpg`.
That recorded run used the async-prewarmed nested service transport; the fixture
has since been switched to the page-owned MessagePort transport used by the
static probe, so its lifecycle integration requires a fresh browser run.

`tests/media-service.test.mjs` includes the lightweight shared-buffer and page
lifecycle fixtures in the existing `npm test` glob. It does not load a codec,
download assets, or encode media. The page test mocks only imported GPU setup
and presenter dependencies, retaining production async lifecycle code. It
checks stop during GPU prewarm closes a late-arriving transport and never
creates a media worker for the stopped session.

An initialization cleanup review found that an invalid asset URL could throw
after creating a nested worker but before `connect` acquired its client. The
constructor now closes that worker before rethrowing; a regression verifies it.
Close is idempotent and suppresses only cleanup transport errors, preserving
the original initialization error. Page stop terminates the guest and its
page-owned service even if async connect has not finished. Fatal supervision
uses a separate atomic flag so failure does not depend on valid shared JSON.
