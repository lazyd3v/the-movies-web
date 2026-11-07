# Original WinMM call audit and bounded browser PCM sink

Original `MoviesSE.exe` SHA256 `48f909d7f89051ff0d3277f7dc0aa7c65cd1c92015542fcbc3d5f86700b1566a`. Evidence: PE import table, Capstone disassembly of original bytes, and `.cache/recomp-full/out/recomp_191.c`. No executable changes. This is backend evidence, not installed WinMM emulation or a claim the game plays audio.

## Observed original calls

All listed Win32 imports use 32-bit stdcall (callee removes arguments); pointers and handles are 32-bit. MMRESULT is a 32-bit result, zero success.

| Import / IAT slot | Arguments (stack order) | Original call / use |
|---|---|---|
| waveOutGetNumDevs / D16460 | none; returns UINT | C1AEC0 wrapper tests nonzero |
| waveOutGetDevCapsA / D16458 | device, WAVEOUTCAPSA*, size | C12F4C, size 0x34 |
| waveOutMessage / D1645C | device, message, param1, param2 | C12F3C: device -1, message 0x2015, two DWORD output pointers; returned first output then used as GetDevCaps device |
| waveOutOpen / D16478 | handle*, device, WAVEFORMATEX*, callback, instance, flags | C1B1E4 query; C1B20C actual open |
| waveOutPrepareHeader / D16474 | handle, WAVEHDR*, 32 | C1B33F initial buffer loop |
| waveOutWrite / D16470 | handle, WAVEHDR*, 32 | C1B35B initial silence; C1AFEF steady submission |
| waveOutReset / D1646C | handle | C1AF01 shutdown |
| waveOutUnprepareHeader / D16464 | handle, WAVEHDR*, 32 | C1AF32 shutdown loop |
| waveOutClose / D164AC | handle | C1AF4D shutdown |

Message 0x2015 is DRVM_MAPPER_PREFERRED_GET. The native adapter reports virtual device 0 only when the real backend is available, otherwise DWORD -1; PREFERREDONLY=1 accurately reflects that no fallback sink exists. Both output pointers are validated before writes. Unknown messages remain unsupported. [Microsoft mapper contract](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutmessage). No waveOutPause, waveOutRestart or waveOutGetPosition import was found. Other WinMM waveIn, mixer, PlaySound and timing imports are outside this output-sink slice.

## Format, headers, callbacks

C1B180 constructs PCM tag 1, 44100 Hz, 16 bits, channel count from argument 1 (rejects >2), block alignment channels*2, average bytes/sec 44100*alignment, cbSize 0. Query uses WAVE_MAPPER -1, null handle/callback/instance and flags 9 (QUERY|DIRECT). Actual open uses flags 0x30000 CALLBACK_FUNCTION, callback C1B070 and instance = audio object.

Audio object fields: +604 header count, +608 outstanding count, +610 header array, +614 device handle, +618 sample pool, +61C circular index, +624 critical section. Header stride is 32 bytes; data lengths channels*256 bytes = **128 PCM frames**. WAVEHDR offsets: lpData 0, dwBufferLength 4, dwBytesRecorded 8, dwUser 12 (buffer index), dwFlags 16, dwLoops 20, lpNext 24, reserved 28. Initial buffers are zeroed silence.

Callback ABI: `void CALLBACK waveOutProc(HWAVEOUT, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)`; original C1B070 returns with `ret 20`. It handles only WOM_DONE 0x3BD and decrements positive outstanding count under its critical section; other messages are ignored. The steady submission routine increments outstanding **after waveOutWrite returns**. Synchronous completion inside Write would race this accounting. A future native bridge must dispatch callbacks at a safe guest execution boundary, never execute guest code on the audio rendering thread. For WOM_DONE, param1 is the original header address, param2 zero.

[Microsoft waveOutOpen](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutopen) documents format-query and callback selection. [Microsoft waveOutReset](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutreset) requires returning all pending buffers as done after stopping/resetting. That includes cancelled buffers: “done” must not be counted as played audio.

## Implemented backend contract

`public/audio-sink.js` exports `createMoviesAudioSink`, called directly from a user gesture. It requires isolated shared memory, a running 44100 Hz AudioContext, and successful AudioWorklet initialization. It does not silently resample a different browser context rate. No audio starts on page load.

The returned descriptor contains transferable-by-sharing SABs for exactly one producer and one consumer. The producer can live in the game worker using `new MoviesAudioProducer(descriptor)`; the page must then relinquish producer ownership. The rendering consumer is `public/audio-worklet.js`. Default storage: 32768 interleaved S16 frames (131072 bytes stereo), 36 control bytes; configurable capacity power-of-two 128..65536, at most 256 pending buffer records. The worklet converts directly to output floats without intermediate buffers. Device/hardware buffering beyond AudioWorklet output is not measured.

Producer operations:

- `write(id, Int16Array)`: copies complete PCM frames, publishes only after copy; rejects full queue, duplicates and malformed buffers before mutation. Caller polls completions to reclaim records.
- `pause()` / `resume()`: queued data retained. An already executing render quantum may complete across pause; a future synchronous WinMM pause adapter must account for that boundary.
- `requestReset()` then `poll()`: no new writes until rendering thread acknowledges. Captures pre-reset read position so already-consumed buffers report `played`, discarded ones `reset`. No discarded frames increment consumed counters. Future waveOutReset must wait for acknowledgement before returning and reset its WinMM position epoch.
- `poll()`: returns completed buffer IDs, `played`/`reset` reason, consumed/rendered/underrun counters and queued count. `played` means consumed into AudioWorklet output, **not verified audible output or physical DAC completion**. Underruns write silence without advancing consumed source frames.
- `close()`: releases queued records as `closed`, closes backend lifetime; it is not a direct waveOutClose implementation (native close must reject outstanding buffers unless reset first).

Counters are unsigned 32-bit, wrap-safe for bounded outstanding spans. Telemetry counters wrap after ~27 hours at 44100 Hz and are not a 64-bit WinMM position clock. Reset acknowledgement requires the browser to keep rendering; a suspended/failed audio context needs explicit error/timeout in a future bridge, not fabricated success. Native WAVEHDR flags and callbacks are not installed by this code.

## Validation

Run `node --test tests/audio-ring.test.mjs`: five tests cover actual consumption, pause, silence on underrun, reset publication race, cancellation, overflow atomicity, duplicate IDs, wrap and format limits.

Run `python3 scripts/audio-probe/stage.py`, open `http://127.0.0.1:4173/runtime/audio-probe/`, explicitly click **Play quiet audio test**. The bounded diagnostic submits 128-frame stereo buffers, verifies paused consumption stays zero, plays 16384 frames, checks underrun silence accounting, resets eight queued buffers, restarts with 8192 frames, then closes two pending buffers. It emits machine-readable `audioProbeResult`; real browser verification passed: 192 played buffers / 24576 consumed frames, zero paused consumption, eight reset cancellations, two close cancellations, context closed. Evidence: `docs/evidence/audio-browser-sink.json` and `.jpg`. The stop button closes the actual audio context. No original game files or shared launcher code are changed.

## Native ABI to real worklet fixture

`python3 scripts/audio-probe/build-native.py` builds the actual `native/audio-api.c` with Emscripten and stages `/runtime/audio-probe/native.html`. Explicit **Play native audio test** calls the compiled WinMM imports, copies real PCM through the provider into AudioWorklet, pumps guest callbacks only after real consumption, and checks 64 played buffers (8192 frames), eight reset completions, native header flags, mapper device and close. Source/artifact SHA256 values are saved in `native-manifest.json`.

This fixture uses a minimal CPU/callback harness, not original game execution. The page asynchronously waits for actual render-thread reset acknowledgement before calling native waveOutReset, because blocking Atomics.wait is unavailable on the page thread. A production guest-worker bridge must perform that acknowledgement synchronously inside the reset provider and remove consumed/discarded IDs from its provider completion queue. The actual game launcher has not been wired. Browser acceptance of this second fixture passed: 64 played callbacks, 8192 actually consumed PCM frames, eight reset callbacks, mapper device 0 with preferred-only policy, and closed audio context. The earlier sink-only browser acceptance remains separate. The first fixture attempt exposed an Emscripten EM_JS pointer-parameter parser issue (`uint32_t*ids` became the wrong JavaScript identifier); pointer declarations now include the required whitespace, and staging checks the generated function signatures. No native handler or original executable change was needed for that bridge correction.

## Production worker bridge

`public/audio-client.js` owns the producer exclusively on the guest worker. It exposes synchronous `moviesAudioAvailable/Open/Write/Poll/Reset/Close` methods, bound by `movies_audio_init_browser()` in the native module. Mono PCM is copied and duplicated into the stereo render ring; stereo PCM is copied unchanged. Close releases the virtual device while retaining the prewarmed context for a later open. Backend errors and reset timeout fail explicitly. Reset waits only on the worker for the actual worklet RESET_ACK and drains both played/discarded producer records before native completion bookkeeping.

The diagnostic launcher prewarms AudioContext directly in the launch click, transfers the shared descriptor to the worker, and closes the context on stop/terminal/error or stale initialization. Renderer failure/suspension is a failure, not silent successful playback. The worker disposes its producer at terminal exit. Native registration and safe callback-boundary integration are coordinated by the parent; this document does not claim original-game playback.

Original disassembly confirms why an arbitrary post-import callback hook is unsafe: C1AFEF calls waveOutWrite, C1AFFC enters the critical section, C1B00A increments outstanding count, and C1B010 leaves the critical section. A callback after Write or EnterCriticalSection can run before increment. The initial integration therefore restricts pumping to Sleep/PeekMessage boundaries; guest child-thread playback remains a separate scheduling requirement.

`tests/audio-client.test.mjs` tests mono conversion, owned copying, actual-consumption IDs, reopen, independent-renderer synchronous reset, and honest timeout. Updated launcher lifecycle tests cover audio cleanup during GPU prewarm cancellation. `/runtime/audio-probe/production.html` runs the actual compiled native module and production client in a worker against the real page-owned AudioWorklet, including synchronous reset; real browser acceptance passed: production provider and worker synchronous reset enabled, 64 played callbacks / 8192 frames, eight reset callbacks, reset request 1 acknowledged as 1, and context closed. Evidence is `docs/evidence/audio-browser-production.json` and `.jpg`, separate from the earlier page-driven native fixture.
