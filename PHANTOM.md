# Phantom Chessboard — protocol notes (RE in progress)

> **Status: incomplete.** GATT structure identified, frame layouts not yet
> reverse-engineered. Driver in this repo is scaffolding only —
> connection will succeed but writes/notifies will not be interpretable
> until the frame formats are understood.

## What we know

### Hardware
- **MCU**: ESP32 (Xtensa LX6), built with PlatformIO (`platformio.ini` paths
  visible in firmware strings).
- **BLE stack**: `NimBLE-Arduino` (paths like
  `.pio/libdeps/esp32/NimBLE-Arduino/src/NimBLECharacteristic.cpp`).
- **Stockfish**: shipped *in the app*, not the board (the 80 MB
  `libstockfish.so` lives in `lib/arm64-v8a/` of the APK).
- **Firmware size**: ~755 KB, packed into the Android APK at
  `assets/flutter_assets/assets/files/firmware.bin` and pushed to the
  board via OTA.

### App
- **Stack**: Flutter (Dart compiled to ARM AOT in `libapp.so`),
  `flutter_blue_plus` for BLE.
- **Package**: `com.phantomapp.flutter_ble_plus`, version 4.0.8 (build 80
  at time of analysis).
- **Permissions** (from `manifest.json`):
  `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, `BLUETOOTH`, `BLUETOOTH_ADMIN`,
  `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION`, `RECORD_AUDIO`,
  `INTERNET`.

### GATT — primary service for the Phantom v2 board

UUIDs were extracted from the firmware binary; declaration order in C
source typically matches order in compiled rodata, so the cluster around
firmware offset `0x1AF7` looks like:

```
SERVICE   acb646cc-92ca-11ee-b9d1-0242ac120002

CHAR_1    acb64a32-92ca-11ee-b9d1-0242ac120002
CHAR_2    acb64fb4-92ca-11ee-b9d1-0242ac120002
CHAR_3    acb650ea-92ca-11ee-b9d1-0242ac120002
CHAR_4    acb6520c-92ca-11ee-b9d1-0242ac120002
CHAR_5    acb6532e-92ca-11ee-b9d1-0242ac120002
CHAR_6    acb6543c-92ca-11ee-b9d1-0242ac120002
CHAR_7    acb65536-92ca-11ee-b9d1-0242ac120002
CHAR_8    acb65662-92ca-11ee-b9d1-0242ac120002
CHAR_9    acb65af4-92ca-11ee-b9d1-0242ac120002
```

Likely roles inferred from firmware strings near the GATT setup
("READ ChessName", "READ Movement", "READ Result Check Movement",
"READ status", "READ vector 200"; functions `moveChessPiece`,
`comerVersion3`, `onCoonect` (sic), `onWrite`):

| Strings nearby                       | Plausible role                             |
| ------------------------------------ | ------------------------------------------ |
| READ ChessName                       | board / device name string read            |
| READ Movement                        | last detected move (board → app)           |
| READ Result Check Movement           | move-validation result echo                |
| READ status                          | device-status read                         |
| READ vector 200                      | sensor vector / piece-presence             |
| Dentro de funcion moveChessPiece     | motor command write (app → board)          |
| Dentro de funcion comerVersion3      | "comer" = capture in Spanish — capture cmd |
| Modo seleccionado onWrite            | mode-select write                          |

**The mapping of UUID → role is not yet confirmed.** We have to either
disassemble the firmware to read the `ble_gatt_chr_def` table, or
HCI-capture a real board's traffic.

### Secondary services (likely OTA / config)

```
acb650ea  ← already in main cluster but has an alt timestamp suffix
392d9e66-937a-11ee-b9d1-0242ac120002   (~minutes after main cluster)
4f1c9720-939a-11ee-b9d1-0242ac120002   (~hour after main cluster)

c60c786b-bf3f-49d8-bd9e-c268e0519a7b   (random; firmware only)
9cc3b57e-eee5-4d3e-8c1d-3fbd636d6780   (random; firmware only)

06034924-77e8-433e-ac4c-27302e5e853f   (firmware) ↔ 1b034927/8 (app)
                                       — same {77e8…} family,
                                       different prefix, plausibly
                                       a separate config service
```

### Legacy services (Jan 2021 — old hardware revision support)

```
7b204548-30c3-11eb-adc1-0242ac120002   (firmware)
7b204d4a-30c3-11eb-adc1-0242ac120002   (firmware)
7b204548-40c4-11eb-adc1-0242ac120002   (app — slightly different ts)
fd31a840-22e7-11eb-adc1-0242ac120002   (both)
```

Probably retained for backward compatibility with original-Kickstarter
hardware. Modern Phantom boards advertise the `acb6…` service.

## What we DON'T know yet

- Which characteristic is the **write** target for motor commands
  (the `setMoveBoard` equivalent).
- Which characteristic carries **notify** pushes for hand-detected
  moves (the inbound `0x01 0x24 …` equivalent).
- The **wire format** of any frame: opcode byte, payload length,
  encoding, force/animate flag, etc.
- The **handshake** sequence at connect time (the `0x21 0x01 0x00` +
  `0x0B 0x04 …` analogue we found for Chessnut).
- The **device-name advertising prefix** (e.g. "Phantom Move" vs
  "PhantomChessboard" — string `Phantom Test Chessboard` exists, but
  shipping units may use a different name).

## How to fill in the gaps

In rough order of practicality:

1. **HCI snoop log from a real Phantom board**. Plug an Android phone
   running the official app into adb, enable Developer Options →
   "Bluetooth HCI snoop log", play one game, pull
   `/sdcard/Android/data/btsnoop_hci.log`. Open in Wireshark with the
   BLE dissectors. Read off the bytes directly. **Gold standard**, no
   guesswork.

2. **Ghidra disassembly of `firmware.bin`** (in progress at time of
   writing — Xtensa LX6, base address `0x40080000`). The
   `ble_gatt_svc_def` table will resolve UUID → access-callback
   function pointer for each characteristic, and the callbacks
   themselves contain the frame-parsing code.

3. **Dart-side decompilation of `libapp.so`** with `blutter`. App-side
   sees the *outbound* protocol (what bytes the app writes) and the
   *inbound* parsing. Less authoritative than firmware (the device's
   handler is the contract), but helpful to corroborate.

## Driver scaffolding

`phantom_bridge.{h,cpp}` exists as a sibling of `chessnut_bridge.cpp` and
follows the same shape: SimpleBLE on a worker thread, command queue,
status callbacks, picker integration. **The Chessnut driver is not
modified**; Phantom is opt-in via device-name detection at scan time.

Until the wire format is RE'd, the driver:

- Connects successfully (UUID is real).
- Subscribes to every notify-capable characteristic on the service.
- **Does not attempt to send setMoveBoard / LED frames** — emits a
  status warning instead (better than corrupting board state with
  guesses).
- Logs every NOTIFY frame raw to stderr for protocol analysis.

When the protocol details land, the only changes will be in
`phantom_encode.h` (frame builders) and the handshake bytes inside
`phantom_bridge.cpp::do_connect`.

## Security note

The shipped APK contains live Cloudflare R2 credentials in
`assets/.env` (publicly readable to anyone who downloads the app).
**Do not redistribute or commit the APK or its `.env`.** This is a
responsible-disclosure issue for Phantom; not in scope of this driver.
