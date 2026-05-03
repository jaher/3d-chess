# Phantom Chessboard — protocol notes

Reverse-engineered from the official Android app
(`com.phantomapp.flutter_ble_plus` v4.0.8, package "Phantom" by
Phantom Chess) and the embedded ESP32 firmware shipped inside it.

> **Earlier draft of this file claimed Phantom was sensor-only —
> that was wrong.** Re-examining firmware strings turned up
> `Test Motor 1`, `Test Motor 2`, `axisX`, `axisY`,
> `moveChessPiece`, `Dentro de funcion comerVersion3` (capture),
> and a full Play-Mode loop that drives steppers in response to
> BLE writes. Phantom is in fact a fully robotic board. The
> findings below replace the old draft.

## Hardware

- **MCU**: ESP32 (Xtensa LX6), built with PlatformIO.
- **BLE stack**: NimBLE-Arduino (paths visible in firmware strings:
  `.pio/libdeps/esp32/NimBLE-Arduino/...`).
- **Robotic** — firmware drives X/Y stepper motors via four coil
  outputs to physically move pieces. Capture handling lifts the
  taken piece off the file/rank ("Take out the piece out of square
  too").
- Stockfish is bundled in the **app** (80 MB `libstockfish.so` in
  the APK), not the board. The board's role is purely physical
  actuation + sensor reporting.
- Firmware ships embedded in the Android APK at
  `assets/flutter_assets/assets/files/firmware.bin` (~755 KB) and
  is OTA-flashed on first connect.

## App

- Stack: Flutter (Dart compiled to ARM AOT in `libapp.so`),
  `flutter_blue_plus` for BLE.
- Permissions: `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, `BLUETOOTH`,
  `BLUETOOTH_ADMIN`, plus location for BLE scanning on older
  Android, plus `RECORD_AUDIO` (voice moves).
- Firmware was written by a Spanish-speaking team; debug strings
  include `Dentro de funcion …` ("Inside … function"),
  `Modo seleccionado …` ("Selected mode …"), `Blancas`/`Negras`
  ("White"/"Black"), and `comer` (Spanish "to eat"/capture).

## GATT layout

**One primary service**, 19 characteristics. Property byte and
chr-pointer global recovered authoritatively from the decompiled
GATT-setup function `FUN_400d2594` (each `createCharacteristic`
call's third argument is the property byte; the result is stored
in a `DAT_400d00xx` global which other firmware code uses to read
or push values).

```
SERVICE  fd31a840-22e7-11eb-adc1-0242ac120002
```

| UUID                                       | Props | R | W | N | chr ptr        | Role |
| ------------------------------------------ | :---: | - | - | - | -------------- | --- |
| `9cc3b57e-eee5-4d3e-8c1d-3fbd636d6780`     | 0x0a  | ✓ | ✓ |   | DAT_400d00bc   | board name string |
| `7b204548-30c3-11eb-adc1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d00c4   | **MOTOR-DRIVE COMMAND IN — app→board move text (7..25 chars)** |
| `7b204d4a-30c3-11eb-adc1-0242ac120002`     | 0x12  | ✓ |   | ✓ | DAT_400d00cc   | legacy status push |
| `c60c786b-bf3f-49d8-bd9e-c268e0519a7b`     | 0x0a  | ✓ | ✓ |   | DAT_400d00d4   | mode-select string (persisted to NVS) |
| `06034924-77e8-433e-ac4c-27302e5e853f`     | 0x12  | ✓ |   | ✓ | **DAT_400d00dc** | **DETECTED-MOVE PUSH — board→app, 9-byte `M 1 e2-e4` strings** |
| `c08d3691-e60f-4467-b2d0-4a4b7c72777e`     | 0x1a  | ✓ | ✓ | ✓ | DAT_400d00e4   | main R+W+N (purpose not yet pinned) |
| `acb646cc-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d00ec   | passive container |
| `acb64a32-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d00f4   | passive container |
| `acb64fb4-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d00fc   | passive container |
| `acb650ea-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d0104   | passive container |
| `acb6520c-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d010c   | passive container |
| `acb6532e-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d0114   | passive container |
| `acb6543c-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d011c   | LED-vector "vector100" config (NVS) |
| `4f1c9720-939a-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d0124   | LED-vector "vector200" config (NVS) |
| `acb65536-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d012c   | passive container |
| `acb65662-92ca-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d0134   | passive container |
| `acb65af4-92ca-11ee-b9d1-0242ac120002`     | 0x12  | ✓ |   | ✓ | DAT_400d013c   | version-info push |
| `392d9e66-937a-11ee-b9d1-0242ac120002`     | 0x0a  | ✓ | ✓ |   | DAT_400d0144   | passive container |
| `93601602-bbc2-4e53-95bd-a3ba326bc04b`     | 0x18  |   | ✓ | ✓ | DAT_400d014c   | **firmware OTA — `BeginOTA`/chunks/`EndOTA`. Don't touch.** |

Property byte decoding (standard BLE values, NimBLE-Arduino 1.x
uses these directly): 0x02 READ, 0x08 WRITE, 0x10 NOTIFY,
0x04 WRITE_NR. So 0x0a = R+W, 0x12 = R+N, 0x18 = W+N,
0x1a = R+W+N.

Earlier drafts of this document misidentified `acb646cc` as the
detected-move push channel and `06034924` as a write target. Both
were wrong — the GATT-setup decompile is the authoritative source
and `06034924` is R+N only.

## Motor-command call chain (the important one)

```
[app writes string to 7b204548]
    │
    ▼
FUN_400d2eac (onWrite handler)
    └── stores string at DAT_400d015c, rejects writes ≥26 bytes
        with "error -1"
    │
    ▼  (consumed asynchronously by the Play-Mode main loop)
FUN_400da2b8  ("Play Mode" main loop, 2255 bytes)
    │   ├── reads sensor matrix every iteration
    │   ├── on detected piece pickup, builds a sensor-move string,
    │   │   pushes it back to the app via FUN_400d2c3c (writes the
    │   │   acb646cc / 7b204d4a notify chars)
    │   ├── waits for the app's "check move" reply ("1" = ok,
    │   │   "2" = reject) on 06034924
    │   └── on "1": calls FUN_400d2ab4 to retrieve the buffered
    │       move string from DAT_400d015c, parses chars at indices
    │       2..6 (see wire format below), invokes:
    ▼
FUN_400da1a0  (piece-char dispatcher)
    │   switch on piece letter (R/N/B/Q/K/P, upper or lower):
    │     'N','n' → moveChessPiece (knight: jump path)
    │     'P','p' → pawn path (capture-aware via FUN_400d9f34)
    │     'R','r','B','b','Q','q','K','k' → generic move via
    │                                       FUN_400d9f34
    ▼
FUN_400da084  ("Dentro de funcion moveChessPiece")
    │   ├── energises 4 motor coils via FUN_400d6cb4(1..4)
    │   ├── computes (axisX_src, axisY_src) and (axisX_dst,
    │   │   axisY_dst) via FUN_400d565c
    │   ├── for capture pieces (knight/queen/etc., bitmask 0x648
    │   │   over piece-id 0..10), invokes comerVersion3
    │   │   (FUN_400d9cbc) to lift the captured piece off-square
    │   │   first
    │   └── calls the actual stepper driver FUN_400d8428 with the
    │       8-coord (src x/y, dst x/y) tuple
```

## Wire format — `7b204548` (app → board, "play this move")

The **Play-Mode loop pulls bytes 2..6 of the buffered string** —
indices 0 and 1 are not validated by the consumer in the path I
traced and look like a 2-byte prefix (likely piece + colour or
`"  "` padding; not yet pinned down). Bytes 2..6 are:

```
  byte 2: src file ('a'..'h', ASCII 0x61..0x68)
  byte 3: src rank ('1'..'8', ASCII 0x31..0x38)
  byte 4: '-'  for a normal move
          'x'  for a capture (firmware lifts the captured
                              piece via comerVersion3 first)
  byte 5: dst file
  byte 6: dst rank
```

The firmware accepts strings of length 7..25. Total length of 6 or
less is silently dropped; ≥26 returns `"error -1"` on the same
characteristic.

The firmware tracks its own piece-position table internally — it
does **not** require the caller to specify what piece is on the
source square. The piece letter that drives the dispatcher is
read from that internal table at parse time, not from the wire.

Best guess for the 2-byte prefix, by elimination: the official app
sends `"  e2-e4"` (two ASCII spaces) or `"Pwe2-e4"` (piece+colour);
either lands the move chars at the right offsets. The driver in
this repo sends a constant `"  "` prefix, which is the safest
zero-knowledge choice and works if the firmware's prefix bytes are
ignored on receive (as they appear to be).

## Wire format — `06034924` notify (board → app, "I detected a move")

**Confirmed from firmware** by decompiling `FUN_400d5acc`
(the move-text builder). Line 62 of the decompile:

```c
builtin_strncpy(local_39, "M 1 ", 4);  // 4-byte literal prefix
local_39[4] = src_file;   // 'a'..'h'
local_39[5] = src_rank;   // '1'..'8'
local_39[6] = (capture)
              ? 0x78 /* 'x' */
              : 0x2d /* '-' */;
local_39[7] = dst_file;
local_39[8] = dst_rank;
// then byte-by-byte FUN_400ee630 into a std::string for 9 iterations
```

So the outbound notify frame is **exactly 9 bytes**:

```
"M 1 " + src_file + src_rank + ('-' | 'x') + dst_file + dst_rank
```

Examples:

```
"M 1 e2-e4"   normal pawn move e2 → e4
"M 1 e4xd5"   pawn capture e4 takes d5
"M 1 e1-g1"   white kingside castle (firmware emits the king's path
              and follows up with a separate rook push)
"M 1 b8-c6"   black knight develops
```

The push channel is **`06034924-77e8-433e-ac4c-27302e5e853f`**
(props 0x12 = R+N, chr-pointer global `DAT_400d00dc`). Verified by
decompiling `FUN_400d2c3c`, which:

1. Calls `FUN_400d2b38(*DAT_400d00dc + 0x24, buffer)` — NimBLE's
   `setValue()` on the characteristic.
2. Calls `FUN_400dc6f4(*DAT_400d00dc, 1)` — NimBLE's `notify()` to
   actually push the value to subscribers.
3. Logs `Send physical mov to cellphone: <buffer>` for diagnostics.

Cross-referenced against `FUN_400d2594` (GATT setup) where the
result of `createCharacteristic("06034924-…", 0x12, 512)` is
stored into `DAT_400d00dc`.

## Other write channels

- `c60c786b` — mode select (string, persisted to NVS under
  `myApp/`). The official app writes a mode token here when the
  user picks Lichess / Chess.com / offline-vs-AI / two-player.
- `acb6543c` — `vector100` blob, persisted to NVS. Half of an LED
  colour configuration table (the other half is `4f1c9720` /
  `vector200`). Not driven from gameplay code.
- `4f1c9720` — `vector200` blob.
- `93601602` — OTA: first write is the literal string `"BeginOTA"`,
  subsequent writes are 0x180-byte firmware chunks, the last
  short chunk triggers `esp_ota_end()` and reboot. **Don't write
  this characteristic from the driver.**

## Generation gap — the shipped firmware doesn't match the live app

Comparing the UUIDs the **firmware** declares (in `FUN_400d2594`,
the GATT-setup function) against the UUIDs the **Dart app** has in
its string table (`libapp.so → strings | grep -E '[0-9a-f]{8}-…'`)
turns up a substantial diff:

```
                            firmware  app
fd31a840-22e7-11eb-…  ✓ ✓   service (both)
7b204548-30c3-11eb-…  ✓     motor cmd (firmware only)
7b204548-40c4-11eb-…    ✓   newer motor cmd (app only)
06034924-77e8-…       ✓     detected-move push (firmware only)
1b034927-77e8-…         ✓   newer detected-move? (app only)
1b034928-77e8-…         ✓   newer detected-move? sibling (app only)
7b204d4a-30c3-11eb-…  ✓     legacy status / error push (firmware only)
c60c786b-bf3f-…       ✓     mode select (firmware only)
4f1c9720-939a-…       ✓     LED-vector200 config (firmware only)
acb64fb4 acb6520c             passive (firmware only)
acb6532e acb65536
acb65662 acb65af4
acb651f4-92ca-11ee-…    ✓   newer (app only)
b5a650ea-92ca-11ee-…    ✓   newer (app only)
common: 392d9e66, 93601602 (OTA), c08d3691, acb646cc, acb64a32,
        acb650ea, acb6543c
```

The firmware string table also shows debug strings like
`Dentro de funcion onCoonect` (typo: `onCoonect`),
`Modo seleccionado` etc., suggesting an **early development
build**. The Dart app, meanwhile, has full localisation support
(`Mover hacia la derecha`, `Erakutsi menua` Basque, `Agor y ddewislen`
Welsh, …) and download-firmware UI strings (`SettingPageSendingFirmware`,
`Error download firmware`, `Roll Back Firmware`).

The conclusion is that **the firmware embedded in the APK
(`assets/flutter_assets/assets/files/firmware.bin`) is an OTA
bootstrap — possibly the original 2021 build kept around for
fresh-out-of-box boards** — and production boards OTA-update
themselves on first connect to a newer firmware that the app
actually expects (with the `40c4`/`1b034927`/`acb651f4` UUIDs).
The newer firmware presumably lives in Phantom's
Cloudflare R2 bucket (live credentials are leaked in the APK's
`assets/.env`; **don't use them**).

What this means for the driver in this repo:

- **For boards still running the original firmware** (the one we
  reverse-engineered), the driver's UUID set is correct and the
  motor command should drive pieces.
- **For boards already updated to current firmware**, the UUID set
  is *wrong* — the production motor-cmd channel is
  `7b204548-40c4-11eb-…` not `7b204548-30c3-11eb-…`, and the
  detected-move push goes to `1b034927-77e8-…` (or its sibling)
  not `06034924-77e8-…`. The driver will connect, log raw frames,
  fail to find its expected write characteristic, and emit
  `ERROR move-cmd characteristic … not exposed by this peripheral`.

Useful follow-ups, in order of effort:

1. **HCI snoop log from a real Phantom** running the current app
   (Developer Options → "Bluetooth HCI snoop log" → play a move →
   pull `/sdcard/Android/data/btsnoop_hci.log` → Wireshark). This
   reveals the production wire format directly.
2. **Dart-side decompile** of `libapp.so` with
   [`blutter`](https://github.com/worawit/blutter) to recover the
   class structure of `sendBleMoveComm` / `sendMove` and see what
   payload the app actually writes.
3. **Reverse a current-version firmware**. The R2 bucket
   `caff8efc4a6e73a41c1095fb30373092.r2.cloudflarestorage.com/phantom`
   presumably hosts the firmware downloads referenced by Dart's
   `downloadFirmware`, but pulling them would require credentials
   we shouldn't be using; better-faith path is to capture the OTA
   download from a real device with a proxy.

## Driver — what's actually shipped

Both Chessnut Move and Phantom drivers implement the shared
`IBoardBridge` interface declared in `board_bridge.h`. The picker
in `app_state.cpp` decides which concrete bridge to instantiate
based on the picked device's advertised-name keyword
(`chessnut` → ChessnutBridge, `phantom`/`gochess` → PhantomBridge),
sets `g_active_bridge` to the right one, and the rest of the app
dispatches per-move events polymorphically via that pointer. No
protocol-specific branching outside the bridge implementations.

`phantom_bridge.{h,cpp}` (desktop) and the Phantom branches in
`web/chessnut_web.cpp` (web):

- Discover the Phantom by device name (advertised name contains
  `phantom` or `gochess`).
- Connect to service `fd31a840-…`, subscribe to every
  notify-capable characteristic, log each notify frame raw to
  stderr / the JS console.
- Implement `IBoardBridge::on_move_played(fen, src, dst, capture)`
  by writing a 9-byte ASCII MOVE_CMD to `7b204548` — the
  motor-drive command. Capture detection (alive-piece-count delta
  between the last two snapshots) drives the `'-'`/`'x'` separator
  byte so the firmware lifts the captured piece via
  `comerVersion3` before driving the moving piece.
- `IBoardBridge::on_full_position_set` is a no-op for Phantom —
  the firmware has no setMoveBoard primitive.
- `IBoardBridge::on_highlight_move` falls through to the default
  no-op — Phantom has no per-move LED API on verified channels.
- **Do not** push to `c60c786b` (mode-select), `acb6543c` /
  `4f1c9720` (LED-vector NVS configs), or `93601602` (OTA). Those
  are off-limits without a real board to verify against.

The Chessnut driver (`chessnut_bridge.{h,cpp}` and
`web/chessnut_web.cpp`) gained the same `IBoardBridge` overrides
but its protocol-specific helpers (`send_fen`, `send_led_move_grid`,
`blink_square`, `probe_piece_state`, `start_scan`,
`request_connect`) are unchanged — the picker scan and reconnect
paths still go through them directly.

## Security note

The shipped APK contains live Cloudflare R2 credentials in
`assets/.env` (publicly readable to anyone who downloads the app).
**Do not redistribute or commit the APK or its `.env`.** This is a
responsible-disclosure issue for the Phantom team; out of scope
for this driver.
