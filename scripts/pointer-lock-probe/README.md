# Real pointer-lock / native cursor diagnostic

Build with `python3 scripts/pointer-lock-probe/build.py`, then open
`/runtime/pointer-lock-probe/index.html` on the local isolated server. This is a
synthetic native ABI fixture, not the game. Click Capture mouse, wait for native
warp checks, move the actual mouse, press Escape, then Verify release / finish.
The page reports actual browser errors rather than treating denied capture as a
pass. Real Chrome capture QA passed on November 7 after unlocking the Mac: the page
reported actual capture, native warp, real movement and release. This verifies the
fixture, not original-game input integration. Failed capture while the Mac was
locked did not establish a browser-specific policy limitation.

The worker runs the real `native/window-api.c` SetCursorPos/GetCursorPos through
EM_JS and the same shared pointer snapshot as the launcher. Before capture,
SetCursorPos returns FALSE/ERROR_NOT_SUPPORTED and leaves actual cursor data
unchanged. While captured, signed requests clamp to the implemented single
virtual desktop; success acknowledges an atomic virtual cursor update, never an
OS pointer warp. The snapshot retains a last validated virtual position after
capture loss, because some original callers ignore GetCursorPos's BOOL result.
A subsequent genuine page pointer sample supersedes that retained position.
Initial startup still requires a genuine pointer sample.

Capture and coordinates share one64-bit atomic word: generation/capture/validity
flags plus unsigned16 x/y. Compare-exchange prevents a stale worker warp from
modifying a newly captured generation. Legacy16-byte physical-only snapshots
remain readable but cannot acknowledge virtual warps. The page's physical POINT
snapshot remains signed32 and separately published by seqlock.

Actual captured movement is scaled to game pixels before window messages are
produced. DirectInput continues receiving only actual relative movement;
SetCursorPos does not fabricate relative deltas, keyboard events, or button
states. Pointer-lock transition/blur clears fractional motion accumulators in
both producers so scaled movements remain coherent. Root launcher focus-loss
handling clears guest focus and releases capture while keeping the last valid
virtual desktop point available to unconditional cursor queries.

Native O2, ASan/UBSan and Node/Wasm ABI tests pass. Twenty focused JS tests cover
capture state, failure detail preservation, fractional movement, mouse-message
coordinates, generation races using a real worker, and existing launcher/input
regressions. These tests do not substitute for successful real-browser capture,
movement, and Escape-release acceptance.
