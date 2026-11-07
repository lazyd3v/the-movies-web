# Bounded original-media sample adapter

`stream-adapter.js` uses the actual core's exposed `FS.registerDevice`,
`FS.makedev`, and `FS.mkdev` APIs. Its character-device `write` callbacks receive
chunks directly from FFmpeg's rawvideo and PCM muxers; decoded output is never
stored as a growing MEMFS file. We inspected these API implementations in the
pinned local `ffmpeg-core.js` before using them. `FS.createDevice` was avoided
because its output callback runs once per byte.

The format is explicitly this fixture's 640×480 BGR24 video and stereo 44.1kHz
S16LE audio. Video uses `-vsync 0`, preserving the 309 decoded frames rather
than duplicating frames to impose a nominal frame rate. One 921,600-byte video
accumulator assembles writes into frames. An 8,192-byte audio accumulator
assembles PCM packets. The synchronous `publish` callback must consume or copy
its buffer before returning; the next write reuses it.

Video PTS comes from `showinfo` with explicit rational time base 1/1000. Audio
PTS spans come from `ashowinfo` with rational time base 1/44100 and original
sample counts; a packet spanning two decoded audio frames carries two spans.
Audio starts at PTS4101/44100, not at zero. Metadata queues are capped at64 and
fail explicitly on overflow/missing timestamps. This logger-based extraction is
version-specific to the pinned core; a production libavcodec adapter should
expose frame timestamps directly instead.

## Full-clip evidence

Run:

```sh
node scripts/media-probe/stream-node.cjs
python3 scripts/media-probe/stream-reference.py
python3 scripts/media-probe/stage.py
```

`prepare.py` also runs these commands. The independent native reference reads
one frame at a time from an FFmpeg stdout pipe and hashes it. It does not save
a full rawvideo movie. Local Wasm evidence stores frame hashes/timestamps,
audio packet timing metadata, and a small PCM fixture for comparison.

- 309 frames, first PTS0, last PTS10733 at time base1/1000.
- Every BGR24 frame hash and video PTS equals native FFmpeg7.1.
- 1,900,544 PCM bytes, 475,136 stereo sample frames; native count matches.
- Maximum S16 difference1; RMS difference0.684990.
- **Audio PTS does not exactly match FFmpeg7.1**: differences up to444 samples
  (10.07ms). The adapter exposes the pinned5.1.4 decoder's own timestamps.
  Correct cross-stream scheduling remains to be audited before synchronized
  playback; sample offset alone is not presented as original-media time.

Evidence lives in `.cache/media-probe/stream-{wasm,comparison}.json`,
`stream-reference-audio.log`, and the two PCM references.

## Browser queue proof

Open `/runtime/media-probe/stream.html` on the existing localhost server and
click **Decode full clip**. COOP/COEP cross-origin isolation is required.

The decoder worker publishes through **one** 921,600-byte SharedArrayBuffer
slot. It posts only packet metadata and waits using `Atomics.wait`; the consumer
copies one packet, hashes video or compares PCM, then acknowledges with
`Atomics.notify`. It deliberately waits2ms per packet by default. This forces
backpressure without relying on decoding and consumption having similar speed.
The delay can be changed to0–100ms. A stalled consumer causes a10-second
backpressure error; overall decoder timeout is60seconds. The UI heartbeat runs
independently.

Resident sample buffers are bounded: producer video921,600 + audio8,192,
shared slot921,600, and at most one consumer copy921,600 bytes (plus decoder
internal state). The test additionally holds a1,900,544-byte native PCM
reference and small per-frame hash/timestamp metadata. These comparison-only
fixtures are not requirements of a future game consumer. The input WMV itself
is1,235,199 bytes and remains resident; this is bounded decoded-output streaming,
not an incremental compressed-input reader.

The browser verifies each video hash/PTS against local Wasm, each PCM value
against native FFmpeg (tolerance1), and audio spans against local Wasm metadata.
The independent script already checks video against native. Result JSON is
`globalThis.moviesStreamProbeResult`; it explicitly labels audio timing as a
local-Wasm comparison.

**Stop worker** terminates the decoder and drops pending data; restarting begins
at the start of the clip. Arbitrary seeking is not implemented or claimed.
There is no sample scheduler, audio device, graph integration, Windows Media COM
object, or builtin module marker. The consumer runs as fast as backpressure
allows, not at presentation time. Audio/video output ordering follows FFmpeg's
mux writes and is not itself a synchronized playback order.

## Observed output ordering: not ready for a timed graph

Both output maps run inside **one** `core.exec`, not separate decoding passes.
Nevertheless, FFmpeg's output buffering produces large bursts. The recorded
packet order starts with64 video frames (last PTS2.566s) before the first audio
packet, then32 audio packets, then53 video /32 audio,37 video /32 audio, etc.
Largest observed video burst64; audio burst32×8192bytes (about1.486s PCM).
After both streams have appeared, video can lead the latest delivered audio
end timestamp by2.811s; audio leads latest video PTS by at most0.0764s.
These compare delivered timestamps, not a playback clock.

Consequently, a graph that waits for audio before consuming initial video can
**deadlock** against this single-slot producer. The browser proof intentionally
drains packets immediately. It proves bounded transfer and decoder fidelity,
not an immediately usable timed playback queue. Future integration needs
independently driven streams or verified low-latency output flushing and a
bounded scheduling policy that guarantees progress. Merely increasing the
video queue to cover this fixture's64-frame burst would consume about59MB and
would not establish a general bound for other files.

Exact evidence is `.cache/media-probe/stream-wasm.json` (`packetOrder`) and
`.cache/media-probe/stream-order.json`. No source media was changed.

## Independent clocked consumer proof

`timed.html` resolves the shared-producer deadlock by running **two separate
core instances in two workers**. Each gets the unchanged compressed input.
One executes only the video output map; the other only the audio output map.
`independent-node.cjs` verifies both modes reproduce every prior frame hash,
video PTS, audio timestamp span, audio packet count, and whole PCM hash.
The earlier single-worker proof remains available for comparison.

Open `/runtime/media-probe/timed.html`, click **Start clocked clip**, and keep
the tab visible. Both first packets must be ready and checked before the clock
starts. The UI then acknowledges each independent queue only when that packet's
original decoder PTS is due. Neither decoder waits for the other stream's queue.
The test is muted and has no audio-device scheduling or game graph.

The decoder queues are separately bounded at one video frame921,600bytes and
one audio packet8192bytes. Producer accumulators are the same sizes; unused
stream accumulators are omitted in single-stream mode. Consumer comparison
copies at most one packet per stream; the queued actual samples remain in SAB
until acknowledged. Each decoder independently holds the1,235,199-byte input
and its own Wasm heap. The final result reports each heap's actual byte length;
these heaps are additional to the small queue bounds. There is still one
1,900,544-byte native PCM test reference plus expected hash/timing metadata.

Result `globalThis.moviesTimedProbeResult` records:

- all309 frames,475136 stereo PCM sample frames, and232 audio packets;
- maximum video/audio delivery lateness and counts over100ms;
- underrun episodes where a sample is over100ms due but its queue is not ready;
- maximum difference between latest delivered audio/video **start** PTS (a
  delivery diagnostic, not an audio-device synchronization guarantee);
- whether the document became hidden, and a separate `timingPassed` verdict;
- clock duration, priming timings, final counts, queues, and decoder heap sizes.

The clock uses `performance.now` and a4ms polling timer, not requestAnimationFrame.
The output requests no presentation before its PTS. Browser scheduling can
still delay delivery; hidden-tab timing is explicitly marked invalid. The
10.766-second container clip's exposed decoded audio spans end at approximately
10.809seconds, so completion waits for that actual sample timeline rather than
truncating to container duration. The FFmpeg5.1.4/7.1 audio PTS discrepancy
remains: separate workers do not fix timestamp semantics.

**Stop** terminates both workers, cancels the timer, and invalidates async
callbacks with a generation counter. Restart creates fresh workers and starts
at the original beginning. Priming has a30-second deadline, stalled queue
consumers a10-second deadline, and the decoder a60-second execution timeout.
No arbitrary seeking, audio playback, video presentation, DirectShow graph, or
Windows Media module availability is claimed by this clocked sample test.
