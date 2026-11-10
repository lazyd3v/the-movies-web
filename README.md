# The Movies — WebAssembly compatibility build

**Development direction (November 7):** a separate static-recompilation prototype
is being developed to address gameplay and input performance. The earlier
reference below still uses Boxedwine. See the [architecture decision and measured
gaps](docs/research/native-port-decision.md); a replacement runtime is not yet ready.

The native path runs statically translated original game code without Wine or
Boxedwine and presents through a D3D9-to-WebGPU compatibility layer. It renders
the original animated Lionhead intro and the original loading screen. It loads frontend assets. The main menu,
studio gameplay, original video playback and gameplay performance remain unverified.
The executable bytes are unchanged.

SYSTEMMEM texture support and a larger resource table remove the previous NULL
texture crash. Correct dynamic-buffer lock handling and referenced-data uploads
now reach additional original draws. FVF 0x104 and the loading-screen material
are implemented. The observed screen-space sprite format and source-color
blend mode now pass GPU checks. The original run accepts the restored sprite
draws. Classic blend factors now pass a broader GPU matrix. The latest original
startup run stops at an unsupported window message; main-menu access is still unverified. Main-menu playability remains unresolved. Texture
decoding and graphics contracts pass native, sanitizer, Wasm and real-GPU checks.
See [current startup evidence and limits](docs/research/static-startup-status.md)
and the [original rendered frame](docs/evidence/static-browser-first-original-frame.jpg).

Matched intro windows average 30.69 presented frames/s, with heavier particle
sections around 18. About 17.7% of those windows is spent in graphics bridge calls;
main Sleep/condition-variable waits are negligible. This is not a gameplay benchmark.
Performance experiments and rejected candidates are recorded in the
[architecture decision](docs/research/native-port-decision.md).

For the native diagnostic, open `/static-probe.html`, select **O1 experimental
guest threads**, and click **Run original startup**. The optional timing checkbox
records bounded graphics and native wait summaries. Individual asset-read logging
is off by default; enable it only for filesystem diagnosis. O2 guest threads is
also available experimentally, with no verified speedup yet. **Capture mouse** uses real
browser pointer capture, with Escape to release. The native mouse diagnostic passed actual Chrome capture, cursor positioning,
physical movement and release on November 7. Original-game captured input still
needs playtesting; uncaptured cursor warps fail truthfully.
**Restore game window** appears when the original game minimizes its virtual window.
Private file changes currently remain temporary and are discarded when the worker stops.

## Boxedwine reference build

Runs the supplied **original Windows executable** through a pinned Boxedwine x86-to-WebAssembly runtime and Wine 11's Direct3D-to-WebGL bridge. This is an experimental compatibility port, not a JavaScript recreation. Gameplay compatibility is still being tested.

**Current verified limit:** The original game reaches its main menu in the browser, with working mouse movement and clicks when pointer capture is enabled. See the [captured main menu](docs/evidence/original-main-menu.jpg). Studio gameplay remains unverified. Browser audio initializes, but playback is not verified. This is an experimental compatibility build.

## Run locally

Requires Node 20+, Python 3.10+, and `cabextract` for Windows Media setup. No npm dependencies.

```sh
python3 scripts/setup-runtime.py
python3 scripts/setup-media.py
python3 scripts/prepare-game.py "/path/to/The Movies/Game Files"
npm start
```

Open http://127.0.0.1:4173 in a recent desktop Chromium browser. Use **Test Wine with Notepad** to isolate runtime startup from game compatibility, then **Launch game**. The existing local setup is already downloaded and packaged, so only `npm start` is needed here.

Keep the game tab visible during startup. A hidden browser severely slowed loading in testing; making it visible allowed the startup splash to render.

Enable **Lock/hide mouse pointer** in the player controls to use the game's menu. Without pointer capture, the game cursor remained stuck at the top-left in testing; with it enabled, relative mouse movement and menu clicks worked.

Runtime setup verifies pinned SHA-256 hashes. Packaging streams the original files without changing them and creates a generated `launch.bat` and registry file. Windows Media setup creates a separate native-DLL overlay; it does not change the game or the pinned Wine root. Downloaded runtime binaries and proprietary game files are ignored by Git. See [porting notes](docs/PORTING.md) for exact versions and inferred registry settings.

If a multithreaded JIT launch crashes, the execution-mode selector also provides a multithreaded interpreter and single-threaded JIT for diagnosis. These are compatibility experiments, not verified gameplay modes.

**Strict memory ordering (slower)** is off by default, matching upstream's performance setting. Enable it when diagnosing intermittent multithreaded race-related crashes, particularly on ARM hosts. It adds memory fences and may substantially slow execution; it is not a general fix for game compatibility problems.

The optional **Buffered archive reads** mode uses a locally built runtime with 256 KiB read-ahead for stored ZIP entries. To build it, install Python 3.12+, Git, make and clang++, then run:

```sh
python3 scripts/build-runtime.py --jobs 3
```

The script downloads its toolchain into `.cache/runtime-build`, runs the cache tests, and stages the variant in `public/runtime/read-ahead`. Standard runtime setup is still required. This build reached the original main menu, but an overall loading-time improvement has not been measured. Without its artifacts, the launcher falls back to the upstream runtime.

## Architecture

- `public/player.js`: integration hooks and boot diagnostics for the upstream runtime and optional local variant.
- `public/proc-stat.js`: virtual Linux CPU statistics required by the browser emulator.
- `public/range-file.js`: read-only Emscripten file backed by HTTP ranges, with a bounded 32 MiB cache. Handles binary BOM sniffing and offsets above 2 GiB.
- `scripts/serve.mjs`: localhost HTTP server with streaming byte ranges and cross-origin isolation headers.
- `scripts/prepare-game.py`: reproducible ZIP_STORED game archive and manifest.
- `scripts/setup-runtime.py`: pinned runtime acquisition and integrity checks.
- `scripts/setup-media.py`: checksum-verified Windows Media compatibility overlay.
- `scripts/build-runtime.py`: optional Boxedwine build with stored-entry read-ahead.

The full 2.45 GiB game is not copied into WebAssembly memory. Wine's approximately 95 MB root filesystem is loaded once per launch; game archive chunks are fetched on demand. The emulator still needs substantial memory for Wine, graphics and the game itself. Synchronous filesystem reads may briefly block the page.

Saves and Wine's writable filesystem use IndexedDB for this browser origin. Use the player's **Get File(s)** control to export files or an overlay before clearing browser data. Save/load behavior in the game still requires verification. Reloading or stopping terminates the emulator.

## Checks

```sh
npm test
```

Tests cover large-file ranges, isolation headers, path confinement, bounded-cache reads, binary transport, archive reproducibility and preserving originals. They do not establish game compatibility.

## Hosting later

Nothing is deployed. A future host must serve the contents of `public/` over HTTPS, support exact HTTP byte-range responses (`206`, `Content-Range`, `Accept-Ranges: bytes`), preserve binary ZIP contents without HTTP content-encoding transformations, and return these headers:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: same-origin
```

Serve `.wasm` as `application/wasm`. Keep runtime and game files on the same origin. The game archive is larger than many hosting providers' per-file limits. The local server implements the required behavior. No GPT Sites integration is used.

Boxedwine is GPL-licensed; pinned sources and its license are recorded in `public/runtime/manifest.json` and `public/runtime/LICENSE-Boxedwine.txt`. Original game assets remain separate from repository source.

## Public website

`public/play.html` is the page meant for the public site. It does three things:
- It asks the player for their own game folder: the folder with `MoviesSE.exe` and `Data`.
- It checks that `MoviesSE.exe` is the exact file the build was compiled from.
- It reads the game files from that folder on the player's computer (`public/local-game-files.js`). Nothing is uploaded or hosted.

Chrome and Edge remember the folder between visits; other browsers ask each time. The page also has fullscreen and credits.

The diagnostic page (`static-probe.html`) and the indexed game archive (`public/game/`) are for local development only.

```bash
npm run build:site      # dist/: play page as index.html, its scripts, the O1 threaded build, ffmpeg.wasm
npm run preview:site    # http://127.0.0.1:4175 serving dist/
node scripts/bench/play-page-check.mjs   # headless end-to-end check of dist/ with a local game folder
```

`dist/` never contains game files or `MoviesSE.exe`. It includes `_headers` with the cross-origin isolation headers that threads require. `probe.wasm` (about 132 MB) and `ffmpeg-core.wasm` (about 32 MB) exceed Cloudflare's 25 MiB per-file limit, so they need R2 or similar storage.

### Error reports

Players can also send a report themselves: the "Report a problem" button, in the FAQ and under the game, opens a dialog with a description, an optional contact and an optional technical log. On builds without a report endpoint, such as local ones, it copies the report to the clipboard instead.

When the game stops with an error, the public page can send a report: the stop reason, the tail of the technical log, the build, the browser and the graphics card. The collector is a Cloudflare Worker with a D1 database (`workers/crash-reports/`); see `wrangler.toml` for the one-time setup.

Reports are sent only when all of these hold:
- the site was built with `npm run build:site -- --report-endpoint https://<worker>/report`;
- the page is not on a local host;
- the player has not unticked "Send error reports".

The Worker accepts reports only from the origins in `ALLOWED_ORIGINS`. It stores no IP addresses: a salted, daily-rotating hash is kept only to rate-limit floods.

```bash
MOVIES_REPORTS_URL=https://themovies.romanf.cc/api MOVIES_REPORTS_TOKEN=$(cat .cache/reports-token) node scripts/reports.mjs summary
```

### Deploying

`npm run deploy:site` builds the site with error reports on, uploads the new release to the R2 bucket `themovies-site` (with `index.html` last), and deploys `workers/site` on themovies.romanf.cc. The Worker serves the site from R2: gzip copies, a year of caching for `/v/<release>/`, revalidation for `/`, and the thread-support headers. It also serves the report API (D1 `movies-crash-reports`) under `/api/`. You need to be logged in with `npx wrangler login`. The token for reading reports is in `.cache/reports-token`.
