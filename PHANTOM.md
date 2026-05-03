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

## Driver status: not yet scaffolded

No `phantom_bridge.{h,cpp}` was added to this repo yet. Adding code
that *connects* to a Phantom but can't speak the wire format would
be misleading — a Phantom user trying it would think the driver was
broken when really we'd just be guessing the bytes. The
`chessnut_bridge.cpp` driver is **untouched**.

When the wire format is recovered (HCI capture or deeper firmware
work), the bridge slots in cleanly as a sibling of
`chessnut_bridge.cpp`: SimpleBLE worker + command queue + the same
picker integration the Chessnut path uses. The protocol layer
(opcodes, frame builders, handshake bytes) is all the change
needed; the BLE plumbing is already factored.

## Security note

The shipped APK contains live Cloudflare R2 credentials in
`assets/.env` (publicly readable to anyone who downloads the app).
**Do not redistribute or commit the APK or its `.env`.** This is a
responsible-disclosure issue for Phantom; not in scope of this driver.
## Update: deeper Ghidra analysis

After importing the firmware as a multi-segment ESP32 image (segments at
their actual load addresses — `0x3f400020` DROM, `0x3ffbdb60` DRAM,
`0x40080000` IRAM, `0x400d0018` IROM, `0x40082d80` IRAM2), Xtensa xrefs
resolve correctly.

### The registration table at `0x400d00b0`

ALL twenty UUIDs are referenced from a single 160-byte contiguous table
at `0x400d00b0`-`0x400d0148`. Each entry is 8 bytes:

```c
struct {
    const char* uuid_str;          // pointer into DROM (0x3f4019f4..0x3f401cb3)
    NimBLECharacteristic** chr_p;  // pointer to a DRAM variable that holds
                                   // the NimBLECharacteristic* after creation
};
```

Order in the table (file offset → entry):

```
400d00b0   fd31a840-22e7-11eb-adc1-0242ac120002   (legacy)
400d00b8   9cc3b57e-eee5-4d3e-8c1d-3fbd636d6780
400d00c0   7b204548-30c3-11eb-adc1-0242ac120002   (legacy)
400d00c8   7b204d4a-30c3-11eb-adc1-0242ac120002   (legacy)
400d00d0   c60c786b-bf3f-49d8-bd9e-c268e0519a7b
400d00d8   06034924-77e8-433e-ac4c-27302e5e853f
400d00e0   c08d3691-e60f-4467-b2d0-4a4b7c72777e
400d00e8   acb646cc-92ca-11ee-b9d1-0242ac120002   ← Service A primary
400d00f0   acb64a32  ┐
400d00f8   acb64fb4  │
400d0100   acb650ea  │  Service A characteristics (6)
400d0108   acb6520c  │
400d0110   acb6532e  │
400d0118   acb6543c  ┘
400d0120   4f1c9720-939a-11ee-b9d1-0242ac120002   ← Service B primary
400d0128   acb65536  ┐
400d0130   acb65662  │  Service B characteristics (3)
400d0138   acb65af4  ┘
400d0140   392d9e66-937a-11ee-b9d1-0242ac120002   (extra service?)
400d0148   93601602-bbc2-4e53-95bd-a3ba326bc04b   (extra service?)
```

**Confirmed: TWO primary services on the modern Phantom firmware,**
not one. The clustering of UUIDs by their generation timestamp
(all `92ca-11ee` chars under `acb646cc`; the three `92ca-11ee` chars
*after* `4f1c9720` belong to it) and the literal-table layout match.

### Still missing for an actual driver

Mapping a UUID to its role still needs: for each `chr_p` variable
in the second column above, find its xrefs in the firmware code.
The function that calls `notify()` / `setValue()` on it is the side
of the protocol writing data; the function that registers an
access callback for it is what processes inbound writes. The
opcodes/frame layouts then come from those handler bodies.

We have the addresses of the DRAM variables (column 2 above:
`0x3ffc1218`-`0x3ffc1260` plus the anomalous `0x3f423604` for the
first legacy entry). Each is a 4-byte slot in the `.bss`/`.data`
section that gets populated at runtime by `createCharacteristic`.

Not chasing those further in this pass — the work is genuinely
hours of Xtensa pseudo-code reading and the user doesn't own a
board to validate findings against. HCI capture from a real
device remains the fastest path to a verified driver.

### Driver strategy

Given the unverified wire format, no `phantom_bridge.cpp` was
added to `3d_chess/`. Adding speculative encoders that we can't
test would be worse than nothing: a Phantom user trying the
driver would think it was broken when really we'd be guessing.

`chessnut_bridge.cpp` is **untouched**.
