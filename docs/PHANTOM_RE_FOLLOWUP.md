# Phantom RE — followup

Concrete next steps to take the Phantom Chessboard integration from
"motor command works on the original firmware, sensor moves get
logged" to "fully wired-up, parity with Chessnut Move." Each item
notes the **verification path** (how you'd confirm it's done) and
the **files you'd touch**.

Prereq for several items: get hands on a real Phantom board, an
Android phone running the official app, and a way to capture BLE
traffic (HCI snoop log on the phone, or a Nordic nRF52 sniffer, or
Wireshark with a btsnoop_hci.log pulled via adb). Without
hardware, items in the **Verification** section can't move; items
in **Code-only** can.

---

## Verification (need hardware)

### 1. Confirm the production GATT layout on a current-firmware Phantom

The shipped APK firmware uses `7b204548-30c3-…` for the motor
command and `06034924-…` for the detected-move push. The Dart app
references `7b204548-40c4-…` and `1b034927-…` / `1b034928-…`
(see PHANTOM.md → "Generation gap"). The driver speaks the older
set; if the production firmware speaks the newer set, the driver
hits "ERROR move-cmd characteristic not exposed."

**Do:** Pair an Android phone with a real Phantom, enable
Developer Options → "Bluetooth HCI snoop log", play one move, pull
`/sdcard/Android/data/btsnoop_hci.log`, open in Wireshark, filter
on the Phantom's BD_ADDR. The `WriteRequest` packets and
`HandleValueNotification` packets reveal exactly which UUID handles
each direction and the byte payload of each.

**Then update:**

- `phantom_encode.h` — `MOVE_CMD_UUID`, `NOTIFY_DETECTED_MOVE_UUID`,
  any new chars.
- `web/chessnut_web.cpp` — `PHANTOM_WRITE`, `PHANTOM_NOTIFY` arrays
  in the EM_JS shim.
- `PHANTOM.md` — replace the "Generation gap" section with the
  confirmed production map.

**Verify:** turn on the Options → BLE verbose log toggle, connect
to a Phantom, make a move on the physical board — the status bar
should show the same UUID + bytes you saw in Wireshark.

### 2. Pin down the inbound MOVE_CMD prefix bytes

Driver currently sends `"M e2-e4"` (7 bytes, 2-byte `"M "` prefix).
The firmware's Play-Mode loop reads bytes 2..6 only, so byte 0..1
are unvalidated. The official app probably sends something
specific — could be `"M e2-e4"`, `"M 1 e2-e4"` (matching outbound),
`"Pwe2-e4"`, or otherwise.

**Do:** Same HCI capture as above; the WriteRequest payload to the
motor-cmd characteristic IS the prefix. Or: decompile `libapp.so`
with [blutter](https://github.com/worawit/blutter) and find
`sendBleMoveComm` / `sendMove` (Dart class names found in libapp.so
strings) — the format-string call site reveals the prefix.

**Then update:** `phantom_encode.h` `MOVE_CMD_PREFIX`.

### 3. Inspect the OTA-current firmware

The Dart app has `downloadFirmware`, `Roll Back Firmware`, and
`SettingPageSendingFirmware` UI strings, plus a Cloudflare R2
bucket reference (`bucketName=phantom`,
`caff8efc4a6e73a41c1095fb30373092.r2.cloudflarestorage.com`). The
production firmware presumably lives there.

**Don't** use the leaked R2 credentials in the APK's `assets/.env`
to fetch it directly — that's the company's infrastructure; pulling
private files even with public credentials is the wrong shape of
move. (And rotate-on-disclosure could happen any time.)

**Do:** Set up a transparent HTTPS proxy
([mitmproxy](https://mitmproxy.org/)) on a phone, run the official
app, trigger a firmware update, and capture the `.bin` download.
Dump it through `esptool.py image_info` and load into Ghidra the
same way as `firmware.bin` (see the existing
`Esp32AddSegments.java` script).

**Then:** redo `MapGattGlobals.java` and `TraceMotor.java` against
the new firmware to refresh the UUID map and the move-command call
chain. If the protocol changed materially, the diff vs. the current
PHANTOM.md is the new authoritative reference.

### 4. Verify the `c08d3691` mode-notify payload format

`FUN_400d2cc0` writes mode strings to `c08d3691` (R+W+N). Internal
mode tokens recovered from firmware strings: `Error Mode`,
`Buzzer Mode`, `Test Mode`, `Pause Mode`, `Sculpture Mode`,
`Await Mode`, `Play Mode`. But we don't know whether the firmware
sends the literal English string, or a numeric code, or both.

**Do:** Capture `Notification` packets on `c08d3691` while changing
the board's mode via the official app. The bytes are the answer.

---

## Code-only (no hardware needed)

### 5. Wire the detected-move parser into the digital game

`app_chessnut_apply_status` already parses `"M 1 e2-e4"` frames
into `(src_col, src_row, dst_col, dst_row, capture)`. There's a
`TODO: feed (sc,sr)→(dc,dr) into the digital game via
execute_move()` comment in `app_state.cpp` where the parser
currently just logs.

**Do:** mirror the Chessnut path in `app_chessnut_apply_sensor_frame`
— the relevant scaffolding (modal handling, ai_animating dispatch,
single-player vs multiplayer routing, illegal-move shake handling)
is already there. The Phantom equivalent is simpler: we already
have the move's coordinates, so skip the diff-detection step
entirely and call `execute_move` directly after the same legality
+ side-to-move + ai-not-thinking guards.

**Files:** `app_state.cpp` (the TODO site, plus a small new
helper). No header changes.

**Verify:** wire a unit test that feeds a fake `"NOTIFY <uuid>
4d20312065322d6534"` (`"M 1 e2-e4"` in hex) through
`app_chessnut_apply_status` and asserts the digital game state
advances by one move. The Chessnut sensor-frame test pattern is a
template.

### 6. Phantom Positioning modal + sensor-baseline confirmation

Chessnut Move has a Positioning modal that blocks the game on
start until the firmware confirms its sensor view matches the
digital starting position. Phantom needs an analogue, but
adapted: there's no full sensor-state push, only per-move
deltas, so we'd block on either (a) explicit user confirmation
("Pieces in place — tap to start") or (b) the first sensed move
that makes sense from the starting position.

**Files:** `app_state.h` (extend the existing
`ChessnutModalType` enum or add a parallel one), `app_state.cpp`
(`refresh_missing_modal` lambda, modal renderer call site),
`board_renderer.cpp` (`renderer_draw_chessnut_missing_modal`).

**Verify:** flow-test it on a Chessnut to make sure the Phantom
addition doesn't regress the existing modal — there's a 4-second
settling window and a pair-aware diff the Chessnut path uses; the
Phantom path doesn't share that scaffolding so it's strictly
additive, low risk.

### 7. Phantom auto-reconnect

`chessnut_tick_reconnect` early-returns when
`chessnut_board_kind == Phantom`, with a comment that the Phantom
bridge has no name-prefix scan path. Two ways to fix:

- **Cheap:** remember the picked address on `g_phantom_bridge`
  itself and re-issue `connect_to_address(saved_addr)` on
  reconnect. Phantom users would just tolerate the original
  picker-pair flow on first connection per session.
- **Proper:** add `request_connect()` and `start_scan()` to
  `PhantomBridge` mirroring `ChessnutBridge`'s, with the same
  cached-MAC fast path under `~/.cache/phantom_bridge_address`.

**Files:** `phantom_bridge.{h,cpp}`, `app_state.cpp`
(`chessnut_tick_reconnect`).

### 8. Choreographed multi-move reset for Phantom

`PhantomBridge::on_full_position_set` is currently a no-op with a
note that Phantom has no setMoveBoard primitive. To match Chessnut
behaviour (game reset → board physically resets), we'd need to
walk the diff between the current sensor view and the starting
FEN, generating one MOVE_CMD per piece that needs to move. With a
clear board (no pieces in odd squares) it's just the home-square
placement; with a fragmented mid-game leftover state it's a
travelling-salesman-ish scheduling problem.

**Reasonable simplification:** assume the user has placed pieces
near (but not exactly on) the home squares; emit a sequence of
MOVE_CMDs that nudges each piece to its rank-1/8 home square.
Order by furthest-piece-first to avoid path collisions.

**Files:** `phantom_bridge.cpp`
(`PhantomBridge::on_full_position_set`).

**Verify:** start a game with the toggle on, confirm motors
sequentially place each piece into the starting layout.

### 9. LED highlight on Phantom

Default no-op currently. The `vector100`/`vector200` NVS configs
are bulk LED patterns persisted on the board, not per-move
highlights — wrong shape for a "highlight last move" use case. If
the production firmware exposes a per-move LED API, that'd surface
in the HCI capture (item 1).

**Files:** `phantom_bridge.cpp`
(`PhantomBridge::on_highlight_move`),
`phantom_encode.h` (new constants).

### 10. Mode-select integration

The mode-select characteristic (`c60c786b`, R+W) accepts arbitrary
strings — the firmware just stashes them in NVS. Internal mode
tokens recovered: `Error Mode`, `Buzzer Mode`, `Test Mode`,
`Pause Mode`, `Sculpture Mode`, `Await Mode`, `Play Mode`. The
official app probably writes one of these on mode change.

**Do:** Once item 1 (HCI capture) is done, observe what the app
writes when the user picks Lichess / Stockfish / 2P / etc. Then
add an `app_phantom_set_mode(token)` API on `PhantomBridge` that
the existing menu can call when the user starts a game.

**Files:** `phantom_bridge.{h,cpp}`, `phantom_encode.h`,
`app_state.cpp` (call site at game-start).

### 11. `vector100` / `vector200` LED config

These are NVS-persisted blobs the firmware reads on boot to
configure the LED layout. Format unknown without an app-side write
trace.

**Do:** HCI capture during whatever app flow customises the LED
colours (probably a settings screen). The bytes ARE the format.

Probably out of scope for the chess-app integration — these are
"LED appearance preferences", not gameplay state.

### 12. `acb646cc` and `acb650ea` (passive containers)

The firmware registers these as R+W with no callbacks, but the
Dart app references them in its string table. They're probably
used as named buffers the app reads for some metadata — board
serial, calibration data, manufacturer info, etc.

**Do:** GATT-walker tool that reads each characteristic right
after connect and dumps the value. Could be a tiny diagnostic
function in the Phantom bridge, gated behind the BLE verbose-log
toggle. Or just open `gatttool` from BlueZ on a Linux host and run
`char-read-uuid <uuid>` for each.

---

## Polish / nice-to-haves

### 13. Promote `1b034928` parsing if it carries a different payload

Sibling to `1b034927` in libapp.so. The pair pattern (one for
src, one for dst? or one for normal moves, one for special moves?)
is suggestive. Once HCI capture reveals the meaning, the existing
`phantom::parse_detected_move` can be extended or split.

### 14. Confirm-modal styling parity

Phantom's "rejected move" feedback is firmware-side: it lights up
red LEDs and waits. The app side currently just shakes the board
(reused from Chessnut's path). Once the production firmware
behaviour is known, decide whether the digital app should
auto-clear the rejection or wait for the user to reset.

### 15. Bridge unit tests

`phantom_encode.h` is pure logic — `make_move_cmd`,
`parse_detected_move`, `is_phantom_name` are all
test-trivial. Add doctests under `tests/` paralleling the
Chessnut helpers (encode/decode roundtrip, capture vs. normal,
out-of-range coords return false).

**Files:** `tests/CMakeLists.txt` or `tests/Makefile`,
`tests/phantom_encode_test.cpp` (new).

---

## Out of scope (don't bother)

- **Sculpture Mode integration** — decorative LED patterns; not a
  chess-app feature.
- **Buzzer / Test Mode** — diagnostic modes, fired by holding
  buttons on the board itself.
- **OTA from inside the app** — letting our app push firmware
  updates. Way more risk than reward.
- **Phantom Move-equivalent FEN sync primitive** — the firmware
  doesn't have one, period; choreographing a full position is
  item 8's territory.

---

## Quick-reference: what's authoritative on what

| Source | Authoritative for |
| --- | --- |
| `firmware.bin` (in APK) | Original GATT, motor-cmd format, detected-move format, mode tokens, motor-driver call chain |
| `libapp.so` strings | UUID set the production app expects, Dart class names, OTA / Cloudflare R2 references |
| `PHANTOM.md` | Synthesis of both, plus what's verified vs. guessed |
| HCI snoop log (TODO) | Production wire format; supersedes everything above when there's a conflict |
